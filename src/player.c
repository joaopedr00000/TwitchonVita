/*
 * HLS player: hardware H.264 (sceAvcdec) + hardware AAC (sceAudiodec).
 *
 *  hls thread -> video queue -> decode thread (core 1): sceAvcdecDecode -> ring of RGBA textures
 *                                                  -> main thread: picks the due frame, vita2d
 *             -> audio queue -> audio thread (core 2): sceAudiodecDecode -> sceAudioOut
 *
 * The audio thread drives the master clock; frames are presented when the
 * clock reaches their pts (late ones are dropped, early ones wait in the ring).
 * The decoders are used directly: the system SceAvPlayer cannot open HLS URLs
 * (NetStream ships a modified WebMAF build of it, not usable here).
 */
#include "player.h"
#include "seek_burst.h"
#include "adts.h"
#include "chat.h"
#include "chat_view.h"
#include "hls.h"
#include "images.h"
#include "log.h"
#include "pktqueue.h"
#include "ts_demux.h"
#include "ui.h"
#include "i18n.h"
#include "ime_utf8.h"
#include "ime_guard.h"
#include "playback_policy.h"
#include "task.h"
#include "twitch.h"
#include "http.h"
#include "tls_trust.h"
#include "json.h"
#include <psp2/power.h>

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/videodec.h>

#define SCREEN_W 960
#define CHAT_PANEL_W 320
#define SCREEN_H 544
#define ALIGN16(x) (((x) + 15) & ~15)
#define AUDIO_GRAIN 1024
#define PTS_PER_MS 90

#define COLOR_WHITE  RGBA8(255, 255, 255, 255)
#define COLOR_PURPLE RGBA8(145, 70, 255, 255)
#define COLOR_GREY   RGBA8(180, 180, 190, 255)
#define COLOR_OSD_BG RGBA8(0, 0, 0, 170)
#define COLOR_LIVE   RGBA8(235, 4, 0, 255)

/* ------------------------------------------------------------------ clock */

typedef struct {
    pthread_mutex_t lock;
    int valid;
    int64_t pts;     /* 90 kHz */
    uint64_t time;   /* sceKernelGetProcessTimeWide() when pts was set */
    int64_t cap_us;  /* max extrapolation (freezes the clock on audio stalls) */
} av_clock_t;

static void clock_set(av_clock_t *c, int64_t pts, int64_t cap_us)
{
    pthread_mutex_lock(&c->lock);
    c->pts = pts;
    c->time = sceKernelGetProcessTimeWide();
    c->cap_us = cap_us;
    c->valid = 1;
    pthread_mutex_unlock(&c->lock);
}

static int64_t clock_get(av_clock_t *c)
{
    int64_t r = TS_NOPTS;
    pthread_mutex_lock(&c->lock);
    if (c->valid) {
        int64_t el = (int64_t)(sceKernelGetProcessTimeWide() - c->time);
        if (el > c->cap_us)
            el = c->cap_us;
        r = c->pts + el * 9 / 100;
    }
    pthread_mutex_unlock(&c->lock);
    return r;
}

/* ----------------------------------------------------------------- player */

typedef struct {
    pktqueue_t vq, aq;
    hls_t hls;
    av_clock_t clock;
    int profile;
    volatile int volume;
    volatile int paused;
    int freerun;                /* no audio: clock runs on wall time */

    volatile int audio_running;
    volatile int audio_done;
    volatile int audio_buffering;
    volatile int audio_failed;
    volatile int rebuffers;     /* times audio ran dry and had to buffer again */
    pthread_t audio_thread;
    int audio_started;
} player_t;

static player_t g_pl;

/* ------------------------------------------------------------ audio thread */

static void *audio_thread(void *argp)
{
    player_t *pl = argp;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_2);
    LOGI("startup: audio worker entered");
    static int16_t silence[AUDIO_GRAIN * 2];
    static int16_t accum[AUDIO_GRAIN * 2 * 4];
    int accum_frames = 0;
    int64_t accum_pts = TS_NOPTS;

    int port = -1, rate = 0, chans = 0, current_volume=-1;
    int lib_init = 0, dec_ok = 0, prebuffer = 1, underflows = 0, audio_errors = 0;
    SceAudiodecCtrl ctrl;
    SceAudiodecInfo info;
    uint8_t *es = NULL;
    int16_t *pcm = NULL;

    memset(silence, 0, sizeof(silence));

    while (pl->audio_running) {
        if(pl->paused){sceKernelDelayThread(10000);continue;}
        if (prebuffer) {
            /* wait for ~1 s of audio before (re)starting output */
            if (pq_span(&pl->aq) >= vt_profile_buffer_ms(pl->profile,pl->rebuffers) * PTS_PER_MS ||
                (pl->hls.finished && pq_count(&pl->aq) > 0)) {
                prebuffer = 0;
            } else {
                pl->audio_buffering = 1;
                if (port >= 0)
                    sceAudioOutOutput(port, silence);
                else
                    sceKernelDelayThread(10000);
                continue;
            }
        }

        packet_t *p = pq_get(&pl->aq, 20);
        if (!p) {
            if (port >= 0)
                sceAudioOutOutput(port, silence);
            if (++underflows > 10 && !prebuffer) {
                prebuffer = 1;
                pl->rebuffers++;
            }
            continue;
        }
        underflows = 0;

        adts_info_t ai;
        if (adts_parse(p->data, p->size, &ai) < 0 || ai.channels > 2) {
            pkt_free(p);
            continue;
        }

        if (!dec_ok || ai.sample_rate != rate || ai.channels != chans) {
            if (dec_ok) {
                sceAudiodecDeleteDecoder(&ctrl);
                dec_ok = 0;
            }
            if (!lib_init) {
                SceAudiodecInitParam init;
                memset(&init, 0, sizeof(init));
                init.size = sizeof(init.aac);
                init.aac.totalStreams = 1;
                int ir = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &init);
                if (ir < 0) {
                    LOGE("audio: sceAudiodecInitLibrary 0x%08X, sigo sin sonido", (unsigned)ir);
                    pl->audio_failed = 1;
                    pkt_free(p);
                    break;
                }
                lib_init = 1;
            }
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info.aac);
            info.aac.isAdts = 1;
            info.aac.ch = ai.channels;
            info.aac.samplingRate = ai.sample_rate;
            info.aac.isSbr = 0;
            memset(&ctrl, 0, sizeof(ctrl));
            ctrl.size = sizeof(ctrl);
            ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
            ctrl.pInfo = &info;
            int cr = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_AAC);
            if (cr < 0) {
                LOGE("audio: sceAudiodecCreateDecoder 0x%08X (%d Hz, %d canales), sigo sin sonido",
                     (unsigned)cr, ai.sample_rate, ai.channels);
                pl->audio_failed = 1;
                pkt_free(p);
                break;
            }
            dec_ok = 1;
            free(es);
            free(pcm);
            es = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxEsSize));
            pcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxPcmSize));

            SceAudioOutMode mode = ai.channels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
            if (port < 0) {
                port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, ai.sample_rate, mode);
                if (port >= 0) {
                    current_volume=pl->volume;
                    int vol[2] = { SCE_AUDIO_VOLUME_0DB*current_volume/100, SCE_AUDIO_VOLUME_0DB*current_volume/100 };
                    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
                }
            } else {
                sceAudioOutSetConfig(port, AUDIO_GRAIN, ai.sample_rate, mode);
            }
            if (port < 0 || !es || !pcm) {
                LOGE("audio: no se pudo abrir la salida (puerto 0x%08X, memoria %s)", (unsigned)port,
                     es && pcm ? "ok" : "sin memoria");
                pl->audio_failed = 1;
                pkt_free(p);
                break;
            }
            LOGI("audio: AAC %d Hz, %d canales", ai.sample_rate, ai.channels);
            rate = ai.sample_rate;
            chans = ai.channels;
            accum_frames = 0;
        }

        if ((uint32_t)p->size > ctrl.maxEsSize) {
            pkt_free(p);
            continue;
        }
        memcpy(es, p->data, (size_t)p->size);
        ctrl.pEs = es;
        ctrl.pPcm = pcm;
        int r = sceAudiodecDecode(&ctrl);
        int64_t pts = p->pts;
        pkt_free(p);
        if (r < 0) {
            if (++audio_errors <= 5)
                LOGW("audio: error al decodificar AAC 0x%08X", (unsigned)r);
            sceAudiodecClearContext(&ctrl);
            continue;
        }

        int frames = (int)(ctrl.outputPcmSize / (sizeof(int16_t) * (unsigned)chans));
        int room = (int)(sizeof(accum) / sizeof(int16_t)) / chans - accum_frames;
        if (frames > room)
            frames = room;
        if (accum_frames == 0)
            accum_pts = pts;
        memcpy(accum + accum_frames * chans, pcm, (size_t)frames * chans * sizeof(int16_t));
        accum_frames += frames;

        int64_t grain_dur = (int64_t)AUDIO_GRAIN * 90000 / rate;
        while (accum_frames >= AUDIO_GRAIN && pl->audio_running) {
            if(pl->paused){sceKernelDelayThread(10000);continue;}
            if(current_volume!=pl->volume){
                current_volume=pl->volume;
                int v=SCE_AUDIO_VOLUME_0DB*current_volume/100;
                int vol[2]={v,v};
                sceAudioOutSetVolume(port,SCE_AUDIO_VOLUME_FLAG_L_CH|SCE_AUDIO_VOLUME_FLAG_R_CH,vol);
            }
            sceAudioOutOutput(port, accum);
            pl->audio_buffering = 0;
            /* the previous grain is what is audible right now */
            if (accum_pts != TS_NOPTS)
                clock_set(&pl->clock, accum_pts - grain_dur, grain_dur * 100 / 9 * 2);
            accum_frames -= AUDIO_GRAIN;
            memmove(accum, accum + AUDIO_GRAIN * chans, (size_t)accum_frames * chans * sizeof(int16_t));
            if (accum_pts != TS_NOPTS)
                accum_pts += grain_dur;
        }
    }

    if (port >= 0)
        sceAudioOutReleasePort(port);
    if (dec_ok)
        sceAudiodecDeleteDecoder(&ctrl);
    if (lib_init)
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
    free(es);
    free(pcm);
    pl->audio_done = 1;
    return NULL;
}

/* ---------------------------------------------------------- video decoder */

typedef struct {
    int lib_init;
    SceUID memblock;
    SceAvcdecCtrl ctrl;
    int created;
    int w, h;             /* decoder / texture size (16-aligned) */
    unsigned pitch;       /* texture pitch in pixels */
} vdec_t;

static void vdec_close(vdec_t *v)
{
    if (v->created)
        sceAvcdecDeleteDecoder(&v->ctrl);
    if (v->memblock >= 0)
        sceKernelFreeMemBlock(v->memblock);
    if (v->lib_init)
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    memset(v, 0, sizeof(*v));
    v->memblock = -1;
}

static int vdec_open(vdec_t *v, int w, int h, char *err, int errlen)
{
    static const int refs_try[] = { 6, 4, 3 };
    int ret = -1;

    memset(v, 0, sizeof(*v));
    v->memblock = -1;
    v->w = ALIGN16(w > 0 ? w : 1280);
    v->h = ALIGN16(h > 0 ? h : 720);

    for (unsigned i = 0; i < sizeof(refs_try) / sizeof(refs_try[0]); i++) {
        SceVideodecQueryInitInfoHwAvcdec init;
        memset(&init, 0, sizeof(init));
        init.size = sizeof(init);
        init.horizontal = v->w;
        init.vertical = v->h;
        init.numOfRefFrames = refs_try[i];
        init.numOfStreams = 1;
        ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
        if (ret < 0)
            continue;
        v->lib_init = 1;

        SceAvcdecQueryDecoderInfo q;
        SceAvcdecDecoderInfo di;
        memset(&q, 0, sizeof(q));
        memset(&di, 0, sizeof(di));
        q.horizontal = init.horizontal;
        q.vertical = init.vertical;
        q.numOfRefFrames = init.numOfRefFrames;
        ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di);
        if (ret >= 0) {
            SceSize sz = (di.frameMemSize + 0xFFFFF) & ~0xFFFFF;
            v->memblock = sceKernelAllocMemBlock("vt_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
            if (v->memblock >= 0) {
                sceKernelGetMemBlockBase(v->memblock, &v->ctrl.frameBuf.pBuf);
                v->ctrl.frameBuf.size = sz;
                ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v->ctrl, &q);
                if (ret >= 0) {
                    v->created = 1;
                    break;
                }
                sceKernelFreeMemBlock(v->memblock);
                v->memblock = -1;
            } else {
                ret = v->memblock;
            }
        }
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        v->lib_init = 0;
    }
    if (!v->created) {
        snprintf(err, (size_t)errlen, vt_msg("No se pudo iniciar el decodificador H.264 (0x%08X)"), (unsigned)ret);
        LOGE("video: %s, %dx%d", err, v->w, v->h);
        vdec_close(v);
        return -1;
    }
    LOGI("video: decodificador H.264 %dx%d listo", v->w, v->h);
    return 0;
}

/* Looks at the first slice of an access unit: keyframe (IDR) and/or a frame no
 * other frame references (nal_ref_idc == 0), which can be skipped safely. */
static void au_info(const uint8_t *d, int n, int *idr, int *disposable)
{
    *idr = *disposable = 0;
    for (int i = 0; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int type = d[i + 3] & 0x1F, ref = (d[i + 3] >> 5) & 3;
            if (type == 5) {
                *idr = 1;
                return;
            }
            if (type == 1) {
                *disposable = ref == 0;
                return;
            }
            i += 3;
        }
    }
}

/* ------------------------------------------------- decode thread + frame ring
 *
 * The decoder runs on its own thread (core 1) and writes pictures into a ring
 * of textures; the main thread only picks the frame that is due on the audio
 * clock and draws it. Before, decoding happened on the drawing thread straight
 * into the one texture on screen, so any stall in drawing (emotes, chat panel,
 * GPU waits) made the video late and it never caught up.
 */

#define MAX_SLOTS 8
#define DEC_PICS  3   /* an IDR can make the decoder output several pictures at once */

enum { SLOT_FREE, SLOT_DECODING, SLOT_READY, SLOT_SHOWN };

typedef struct {
    vita2d_texture *tex;
    int64_t pts;
    int w, h;
    int state;
} vslot_t;

typedef struct {
    vdec_t *v;
    const player_params_t *pp;
    vslot_t slot[MAX_SLOTS];
    int nslot;
    int ready[MAX_SLOTS];        /* READY slots in output (display) order */
    int nready;
    pthread_mutex_t lock;
    volatile int running, done;
    volatile int shown_any;      /* lag checks start once something is on screen */
    volatile int fatal;
    char fatal_msg[128];
    pthread_t thread;

    /* stats, reset by the presenter every interval */
    volatile int st_aus, st_pics, st_multi, st_errors, st_undecoded, st_chat_skip, st_idr_jumps;
    volatile uint64_t st_dec_us, st_dec_max;
    volatile int64_t st_max_lag;
    volatile int total_pics, total_errors;
} vpipe_t;

static vpipe_t g_vp;

static int64_t pic_pts(const SceAvcdecPicture *pic)
{
    if (pic->info.pts.upper == 0xFFFFFFFF && pic->info.pts.lower == 0xFFFFFFFF)
        return TS_NOPTS;
    return (int64_t)(((uint64_t)pic->info.pts.upper << 32) | pic->info.pts.lower);
}

static void *vdec_thread(void *argp)
{
    player_t *pl = &g_pl;
    vpipe_t *vp = argp;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);
    LOGI("startup: video worker entered");
    vdec_t *v = vp->v;
    int skip_to_idr = 0, consecutive_err = 0, first_queue = 1;
    int elm = DEC_PICS; /* pictures offered per call; 1 if the firmware rejects more */
    if (!v || !vp->pp || vp->nslot < DEC_PICS + 1) {
        snprintf(vp->fatal_msg, sizeof(vp->fatal_msg), "Invalid video pipeline");
        vp->fatal = 1;
        vp->done = 1;
        return NULL;
    }

    LOGI("startup: video worker before first lock");
    while (vp->running && !vp->fatal) {
        /* reserve DEC_PICS free textures (back-pressure when the ring is full) */
        int fr[DEC_PICS], nf = 0;
        pthread_mutex_lock(&vp->lock);
        for (int i = 0; i < vp->nslot && nf < DEC_PICS; i++)
            if (vp->slot[i].state == SLOT_FREE)
                fr[nf++] = i;
        if (nf == DEC_PICS)
            for (int k = 0; k < nf; k++)
                vp->slot[fr[k]].state = SLOT_DECODING;
        pthread_mutex_unlock(&vp->lock);
        if (nf < DEC_PICS) {
            sceKernelDelayThread(2000);
            continue;
        }

        if (first_queue) LOGI("startup: video worker first queue wait");
        packet_t *p = pq_get(&pl->vq, 20);
        if (first_queue) { LOGI("startup: video worker queue wait returned"); first_queue = 0; }
        int skip = 0;
        if (p && (!p->data || p->size <= 0)) { pkt_free(p); p = NULL; }
        if (p) {
            int idr = 0, disposable = 0;
            au_info(p->data, p->size, &idr, &disposable);
            /* lag of the AU's decode time against the audio clock (dts: AUs arrive
             * in decode order; pts runs ahead and hid the real lag) */
            int64_t clk = clock_get(&pl->clock);
            int64_t lag = 0;
            if (clk != TS_NOPTS && p->dts != TS_NOPTS && vp->shown_any)
                lag = clk - p->dts;
            if (lag < 0 || lag > 60000 * PTS_PER_MS)
                lag = 0;
            if (lag > vp->st_max_lag)
                vp->st_max_lag = lag;
            int chat = vp->pp->chat_on && *vp->pp->chat_on;

            if (skip_to_idr && !idr) {
                skip = 1;
                vp->st_undecoded++;
            } else if (skip_to_idr) {
                skip_to_idr = 0;
                LOGI("video: reanudo en keyframe");
            } else if (!vp->pp->recorded&&lag > 1000 * PTS_PER_MS && !idr) {
                skip_to_idr = 1;
                skip = 1;
                vp->st_idr_jumps++;
                vp->st_undecoded++;
                LOGW("video: %.1f s de retraso, salto al siguiente keyframe", lag / PTS_PER_MS / 1000.0);
            } else if (!vp->pp->recorded&&lag > 150 * PTS_PER_MS && disposable) {
                skip = 1;
                vp->st_undecoded++;
            } else if (disposable && chat && vp->pp->fps > 40 && lag > 80 * PTS_PER_MS) {
                /* chat visible on a 60 fps stream: unreferenced frames (about half
                 * on Twitch) are not decoded, halving the decoder's work */
                skip = 1;
                vp->st_chat_skip++;
            }
        }
        if (!p || skip) {
            if (p)
                pkt_free(p);
            pthread_mutex_lock(&vp->lock);
            for (int k = 0; k < nf; k++)
                vp->slot[fr[k]].state = SLOT_FREE;
            pthread_mutex_unlock(&vp->lock);
            continue;
        }

        SceAvcdecAu au;
        SceAvcdecArrayPicture arr;
        SceAvcdecPicture pic[DEC_PICS];
        SceAvcdecPicture *pics[DEC_PICS];
        memset(&au, 0, sizeof(au));
        memset(&arr, 0, sizeof(arr));
        memset(pic, 0, sizeof(pic));
        for (int k = 0; k < DEC_PICS; k++) {
            pic[k].size = sizeof(pic[k]);
            pic[k].frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
            pic[k].frame.framePitch = v->pitch;
            pic[k].frame.frameWidth = v->w;
            pic[k].frame.frameHeight = v->h;
            pic[k].frame.pPicture[0] = vita2d_texture_get_datap(vp->slot[fr[k]].tex);
            pics[k] = &pic[k];
        }
        arr.numOfElm = (uint32_t)elm;
        arr.pPicture = pics;
        au.es.pBuf = p->data;
        au.es.size = (uint32_t)p->size;
        if (p->pts != TS_NOPTS) {
            au.pts.upper = (uint32_t)((uint64_t)p->pts >> 32);
            au.pts.lower = (uint32_t)p->pts;
        } else {
            au.pts.upper = au.pts.lower = 0xFFFFFFFF;
        }
        if (p->dts != TS_NOPTS) {
            au.dts.upper = (uint32_t)((uint64_t)p->dts >> 32);
            au.dts.lower = (uint32_t)p->dts;
        } else {
            au.dts.upper = au.dts.lower = 0xFFFFFFFF;
        }

        if (vp->st_aus == 0 && vp->total_pics == 0 && vp->total_errors == 0)
            LOGI("startup: first AVC decode, bytes=%d", p->size);
        uint64_t t0 = sceKernelGetProcessTimeWide();
        int r = sceAvcdecDecode(&v->ctrl, &au, &arr);
        uint64_t dt = sceKernelGetProcessTimeWide() - t0;
        int psize = p->size;
        pkt_free(p);
        vp->st_aus++;
        vp->st_dec_us += dt;
        if (dt > vp->st_dec_max)
            vp->st_dec_max = dt;

        int nout = 0;
        if (r < 0) {
            vp->st_errors++;
            if (++vp->total_errors <= 5)
                LOGW("video: error al decodificar 0x%08X (%d bytes)", (unsigned)r, psize);
            if (elm > 1 && vp->total_pics == 0 && consecutive_err >= 29) {
                /* not one picture yet: maybe numOfElm > 1 is not accepted
                 * (only numOfElm = 1 is used in public code) */
                LOGW("video: sin imagenes tras %d errores, pruebo con 1 imagen por llamada", consecutive_err + 1);
                elm = 1;
                skip_to_idr = 1;
                consecutive_err = 0;
            }
            if (++consecutive_err > 300) {
                snprintf(vp->fatal_msg, sizeof(vp->fatal_msg), vt_msg("Error del decodificador H.264 (0x%08X)"), (unsigned)r);
                vp->fatal = 1;
            }
        } else {
            consecutive_err = 0;
            nout = (int)arr.numOfOutput;
            if (nout > elm)
                nout = elm;
            if (nout > 1)
                vp->st_multi++;
            vp->st_pics += nout;
            if (nout > 0 && vp->total_pics == 0)
                LOGI("video: primera imagen decodificada %dx%d; HLS %dx%d; texture %dx%d",
                     (int)pic[0].frame.horizontalSize,(int)pic[0].frame.verticalSize,
                     vp->pp->width,vp->pp->height,v->w,v->h);
            vp->total_pics += nout;
        }

        pthread_mutex_lock(&vp->lock);
        for (int k = 0; k < DEC_PICS; k++) {
            vslot_t *s = &vp->slot[fr[k]];
            if (k < nout && vp->nready < MAX_SLOTS) {
                s->pts = pic_pts(&pic[k]);
                s->w = (int)pic[k].frame.horizontalSize;
                s->h = (int)pic[k].frame.verticalSize;
                s->state = SLOT_READY;
                vp->ready[vp->nready++] = fr[k];
            } else {
                s->state = SLOT_FREE;
            }
        }
        pthread_mutex_unlock(&vp->lock);
    }
    vp->done = 1;
    return NULL;
}

static void vpipe_free(vpipe_t *vp)
{
    vita2d_wait_rendering_done();
    for (int i = 0; i < vp->nslot; i++)
        if (vp->slot[i].tex)
            vita2d_free_texture(vp->slot[i].tex);
    vp->nslot = 0;
    pthread_mutex_destroy(&vp->lock);
}

static int vpipe_start(vpipe_t *vp, vdec_t *v, const player_params_t *pp, char *err, int errlen)
{
    memset(vp, 0, sizeof(*vp));
    vp->v = v;
    vp->pp = pp;
    LOGI("startup: video mutex begin");
    int lock_rc = pthread_mutex_init(&vp->lock, NULL);
    if (lock_rc != 0) {
        snprintf(err, (size_t)errlen, "Video mutex error: %d", lock_rc);
        return -1;
    }
    LOGI("startup: video mutex ready");

    /* Limit the initial texture ring to leave GPU memory for the UI. */
    size_t frame_bytes = (size_t)v->w * v->h * 4;
    int want = (int)((18u * 1024 * 1024) / frame_bytes);
    if (want > 6) want = 6;
    if (want < DEC_PICS + 1) want = DEC_PICS + 1;
    LOGI("startup: texture ring %dx%d target=%d", v->w, v->h, want);
    for (int i = 0; i < want; i++) {
        LOGI("startup: allocate texture %d/%d", i+1, want);
        vita2d_texture *t = vita2d_create_empty_texture_format(v->w, v->h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!t) { LOGW("startup: texture allocation failed at %d", i+1); break; }
        void *data = vita2d_texture_get_datap(t);
        unsigned stride = vita2d_texture_get_stride(t);
        if (!data || stride < (unsigned)v->w * 4) {
            LOGE("startup: invalid texture data or stride=%u", stride);
            vita2d_free_texture(t);
            break;
        }
        LOGI("startup: clear texture %d stride=%u", i+1, stride);
        memset(data, 0, (size_t)stride * v->h);
        if(i==0)LOGI("video: initial texture filters min=%d mag=%d",(int)vita2d_texture_get_min_filter(t),(int)vita2d_texture_get_mag_filter(t));
        vita2d_texture_set_filters(t,SCE_GXM_TEXTURE_FILTER_LINEAR,SCE_GXM_TEXTURE_FILTER_LINEAR);
        LOGI("startup: texture %d ready", i+1);
        vp->slot[vp->nslot].tex = t;
        vp->slot[vp->nslot].state = SLOT_FREE;
        vp->nslot++;
    }
    if (vp->nslot < DEC_PICS + 1) {
        snprintf(err, (size_t)errlen, vt_msg("Sin memoria para las texturas de video"));
        LOGE("video: solo %d texturas de %dx%d", vp->nslot, v->w, v->h);
        vpipe_free(vp);
        return -1;
    }
    v->pitch = vita2d_texture_get_stride(vp->slot[0].tex) / 4;

    vp->running = 1;
    LOGI("startup: create video thread");
    pthread_attr_t attr;
    int start_rc = pthread_attr_init(&attr);
    if (start_rc == 0) {
        start_rc = pthread_attr_setstacksize(&attr, 128 * 1024);
        if (start_rc == 0) start_rc = pthread_create(&vp->thread, &attr, vdec_thread, vp);
        pthread_attr_destroy(&attr);
    }
    if (start_rc != 0) {
        snprintf(err, (size_t)errlen, vt_msg("No se pudo crear el hilo de video"));
        LOGE("video: %s (0x%08X)", err, (unsigned)start_rc);
        vp->running = 0;
        vpipe_free(vp);
        return -1;
    }
    LOGI("startup: video thread running with %d textures", vp->nslot);
    return 0;
}

static void vpipe_stop(vpipe_t *vp)
{
    vp->running = 0;
    pthread_join(vp->thread, NULL);
}

/* ---------------------------------------------------------------- drawing */

/* Public count only: no OAuth, no recurring requests, verified TLS.
 * A single cancellable worker keeps network I/O out of the render loop. */
typedef struct {
    char login[32];
    int count;
} viewer_job_t;
static int viewer_fetch(void *arg,volatile int *abort_flag)
{
    viewer_job_t *j=arg;
    char login[32],body[256];
    if(twitch_sanitize_login(j->login,login,sizeof(login))<1||strcmp(login,j->login))return -1;
    snprintf(body,sizeof(body),"{\"query\":\"query { user(login: \\\"%s\\\") { stream { viewersCount } } }\"}",login);
    CURL *curl=http_new();if(!curl)return -1;
    if(vt_tls_configure(curl)!=CURLE_OK){http_free(curl);return -1;}
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,8L);
    struct curl_slist *headers=NULL;
    headers=curl_slist_append(headers,"Client-ID: " TWITCH_CLIENT_ID);
    struct curl_slist *next=curl_slist_append(headers,"Content-Type: text/plain;charset=UTF-8");
    if(!next){curl_slist_free_all(headers);http_free(curl);return -1;}
    headers=next;
    buf_t response={0};long status=0;int rc=-1;
    if(http_request(curl,"https://gql.twitch.tv/gql",body,headers,&response,&status,abort_flag)==0&&status==200&&response.data){
        json_t *root=json_parse((const char *)response.data);
        json_t *value=json_path(root,"data.user.stream.viewersCount");
        if(value&&value->type==JSON_NUMBER&&value->num>=0&&value->num<=2147483647.0){j->count=(int)value->num;rc=0;}
        json_free(root);
    }
    buf_free(&response);curl_slist_free_all(headers);http_free(curl);
    return rc;
}

typedef struct {char login[32],bio[512];} live_profile_job_t;
static int live_profile_fetch(void *arg,volatile int *abort_flag)
{
    live_profile_job_t *j=arg;
    return twitch_fetch_channel_bio(j->login,j->bio,sizeof(j->bio),abort_flag);
}

typedef struct {
    const player_params_t *pp;
    int osd;
    int recording_pos,recording_duration,paused;
    vt_seek_burst seek;
    int viewers_busy;
    int dropped;
    float fps_now;
    int cpu_mhz;
    char volume_notice[96],chat_notice[96];
    const char *status;   /* centred message (buffering, ads...) or NULL */
    int menu;             /* in-player extras opened by SELECT */
    int menu_sel;
    int ime_active;
    int quality_menu,quality_sel,quality_top;
    int profile_open,profile_sel,profile_loading,profile_fetched;
    int follow_busy,follow_state,follow_status;
    char profile_url[96],profile_bio[512];
    int decoded_w,decoded_h,display_w,display_h;
    const char *notice;   /* transient send/authentication feedback */
    uint64_t notice_until;
} view_t;

static SceWChar16 chat_title16[64],chat_initial16[256],chat_buffer16[256];
static int open_chat_keyboard(vt_ime_pump_t pump,void *ctx){
    vt_utf8_to_utf16((uint16_t*)chat_title16,64,vt_tr(VT_I_SEND_MESSAGE));
    memset(chat_initial16,0,sizeof(chat_initial16));memset(chat_buffer16,0,sizeof(chat_buffer16));
    SceImeDialogParam param;sceImeDialogParamInit(&param);
    param.supportedLanguages=SCE_IME_LANGUAGE_ENGLISH|SCE_IME_LANGUAGE_SPANISH|SCE_IME_LANGUAGE_PORTUGUESE_BR|SCE_IME_LANGUAGE_FRENCH|SCE_IME_LANGUAGE_GERMAN|SCE_IME_LANGUAGE_ITALIAN|SCE_IME_LANGUAGE_RUSSIAN|SCE_IME_LANGUAGE_JAPANESE|SCE_IME_LANGUAGE_KOREAN|SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE|SCE_IME_LANGUAGE_TRADITIONAL_CHINESE|SCE_IME_LANGUAGE_TURKISH;
    param.languagesForced=SCE_FALSE;param.type=SCE_IME_TYPE_DEFAULT;param.dialogMode=SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;param.textBoxMode=SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title=chat_title16;param.maxTextLength=150;param.initialText=chat_initial16;param.inputTextBuffer=chat_buffer16;
    int rc=vt_ime_open(&param,pump,ctx);
    LOGI("chat: sceImeDialogInit rc=%08X",(unsigned)rc);
    return rc;
}
static void draw_view(const vslot_t *frame, view_t *vw)
{
    int chat = vw->pp->chat_on && *vw->pp->chat_on;
    int AW = chat ? SCREEN_W - CHAT_PANEL_W : SCREEN_W; /* video area width */

    img_frame(); /* emote textures (must run before start_drawing) */
    vita2d_texture *profile_avatar=NULL;
    if(vw->profile_open&&vw->pp->profile_avatar&&*vw->pp->profile_avatar)
        profile_avatar=img_get(vw->pp->profile_avatar);
    if (chat)
        chat_view_update(CHAT_PANEL_W, SCREEN_H, vw->pp->font, vw->pp->channel);
    vita2d_start_drawing();
    vita2d_clear_screen();
    if(!frame&&vw->pp->loading_backdrop){
        vw->pp->loading_backdrop();
        vita2d_draw_rectangle(0,0,SCREEN_W,SCREEN_H,C_SCRIM);
        ui_spinner(SCREEN_W/2,SCREEN_H/2-24,16,C_PURPLE);
        ui_text_center(SCREEN_W/2,SCREEN_H/2+20,C_WHITE,1.0f,vt_tr(VT_I_LOADING));
        vita2d_end_drawing();vita2d_common_dialog_update();vita2d_swap_buffers();vita2d_wait_rendering_done();return;
    }

    if (frame && frame->w > 0 && frame->h > 0) {
        /* visible size: playlist resolution if it fits, else decoder output size */
        /* Only remove alignment padding, never crop a different actual rendition
         * just because the master playlist claims a smaller size. */
        int tw=(int)vita2d_texture_get_width(frame->tex),th=(int)vita2d_texture_get_height(frame->tex);
        int vis_w=frame->w<tw?frame->w:tw,vis_h=frame->h<th?frame->h:th;
        if(vw->pp->width>0&&vis_w==ALIGN16(vw->pp->width))vis_w=vw->pp->width;
        if(vw->pp->height>0&&vis_h==ALIGN16(vw->pp->height))vis_h=vw->pp->height;
        vw->decoded_w=frame->w;vw->decoded_h=frame->h;
        float sx = (float)AW / vis_w;
        float sy = (float)SCREEN_H / vis_h;
        float s = sx < sy ? sx : sy;
        float dw = vis_w * s, dh = vis_h * s;
        vw->display_w=(int)(dw+0.5f);vw->display_h=(int)(dh+0.5f);
        vita2d_draw_texture_part_scale(frame->tex, (AW - dw) / 2, (SCREEN_H - dh) / 2,
                                       0, 0, vis_w, vis_h, s, s);
    }
    if (chat){
        chat_view_draw(AW, 0);
        vita2d_draw_rectangle(AW,SCREEN_H-38,CHAT_PANEL_W,38,C_PANEL2);
        ui_hint_bounded(AW+10,SCREEN_H-13,CHAT_PANEL_W-20,"R",vw->pp->chat_oauth?vt_tr(VT_I_WRITE):vt_tr(VT_I_LOGIN_TO_CHAT));
    }

    vita2d_pgf *f = vw->pp->font;
    if(vw->pp->recorded&&vw->osd){
        char line[160];int duration=vw->recording_duration,pos=vw->seek.active?vw->seek.target:vw->recording_pos;
        vita2d_draw_rectangle(0,0,AW,62,COLOR_OSD_BG);ui_text_fit(18,27,AW-210,C_WHITE,1.03f,vw->pp->title);ui_text_fit(AW-180,27,160,C_WHITE,.9f,vw->pp->quality);
        ui_text_fit(18,51,AW-36,C_GREY,.85f,vw->pp->channel);
        vita2d_draw_rectangle(0,SCREEN_H-88,AW,88,COLOR_OSD_BG);
        vita2d_draw_rectangle(24,SCREEN_H-76,AW-48,6,C_GREY);
        if(duration>0){float fraction=(float)pos/duration;if(fraction>1)fraction=1;if(fraction<0)fraction=0;vita2d_draw_rectangle(24,SCREEN_H-76,(AW-48)*fraction,6,C_PURPLE);}
        snprintf(line,sizeof(line),"%02d:%02d:%02d / %02d:%02d:%02d",pos/3600,pos/60%60,pos%60,duration/3600,duration/60%60,duration%60);
        ui_text_fit(24,SCREEN_H-43,AW-48,C_WHITE,.94f,line);
        if(vw->pp->preview_end)ui_text_fit(24,SCREEN_H-13,AW-48,C_WHITE,.9f,vt_lang_current()==2?"Prévia | Confirmar pausa | Voltar":"Preview | Confirm pause | Back");
        else {
            const char *hint=vt_lang_current()==2?"X Pausa | O Voltar | L/R Vídeo | Setas 10/30s | Triângulo Qualidade":
                vt_lang_current()==1?"X Pausa | O Volver | L/R Vídeo | Flechas 10/30s | Triángulo Calidad":
                "X Pause | O Back | L/R Video | D-pad 10/30s | Triangle Quality";
            ui_text_fit(24,SCREEN_H-13,AW-48,C_WHITE,.84f,hint);
        }
        if(vw->seek.active){
            int delta=vw->seek.target-vw->seek.base;
            snprintf(line,sizeof(line),"%+d s",delta);
            int cx=delta<0?AW/4:AW*3/4;
            ui_round_rect(cx-70,SCREEN_H/2-32,140,64,10,C_SCRIM);
            ui_text_center(cx,SCREEN_H/2+8,C_WHITE,1.3f,line);
        }
    }else if(vw->osd){
        char line[160];
        vita2d_draw_rectangle(0, 0, AW, 64, COLOR_OSD_BG);
        const char *live=vt_tr(VT_I_LIVE);
        int badge_w=ui_text_w(0.75f,live)+20;
        ui_round_rect(14,10,badge_w,24,6,C_RED);
        ui_text_center(14+badge_w/2,28,C_WHITE,0.75f,live);
        int64_t buffered = pq_span(&g_pl.vq) / PTS_PER_MS;
        if(vw->pp->diagnostics)
            snprintf(line,sizeof(line),vt_msg("%s   buffer %.1f s   descartados %d"),
                     vw->pp->quality,buffered/1000.0,vw->dropped);
        else if(vw->decoded_w>0&&vw->decoded_h>0)snprintf(line,sizeof(line),"%s | %dx%d",vw->pp->quality,vw->decoded_w,vw->decoded_h);
        else snprintf(line,sizeof(line),"%s",vw->pp->quality);
        int lw=ui_text_w(0.92f,line);
        if(lw>AW/2)lw=AW/2;
        ui_text_fit(AW-14-lw,28,lw,C_WHITE,0.92f,line);
        int name_x=badge_w+26;
        ui_text_fit(name_x,29,AW-24-lw-name_x,C_WHITE,1.05f,vw->pp->channel);
        if (vw->pp->title && vw->pp->title[0]) {
            ui_text_fit(14,54,AW-28,COLOR_GREY,0.96f,vw->pp->title);
        }
        vita2d_draw_rectangle(0,SCREEN_H-36,AW,36,COLOR_OSD_BG);
        int count=vw->pp->viewer_count?*vw->pp->viewer_count:-1;
        char digits[32],caption[96];
        if(count>=0)ui_fmt_viewers(count,digits,sizeof(digits));
        else snprintf(digits,sizeof(digits),"%s",vw->viewers_busy?"...":"--");
        snprintf(caption,sizeof(caption),vt_msg("%s espectadores"),digits);
        int vw_width=ui_text_w(0.92f,caption)+20;if(vw_width>AW/2)vw_width=AW/2;
        ui_text_fit(AW-vw_width,SCREEN_H-12,vw_width-12,C_WHITE,0.92f,caption);
        if(vw->viewers_busy)ui_spinner(AW-vw_width-8,SCREEN_H-19,5,C_PURPLE);
        int cell=(AW-vw_width-30)/5;
        ui_hint_bounded(10,SCREEN_H-12,cell,vw->pp->btn_back==SCE_CTRL_CIRCLE?"O":"X",vt_tr(VT_I_EXIT));
        ui_hint_bounded(10+cell,SCREEN_H-12,cell,"△",vt_tr(VT_I_SHOW_INFO));
        ui_hint_bounded(10+2*cell,SCREEN_H-12,cell,"□",vt_tr(VT_I_CHAT));
        ui_hint_bounded(10+3*cell,SCREEN_H-12,cell,"START",vw->pp->profile_label?vw->pp->profile_label:"Profile");
        ui_hint_bounded(10+4*cell,SCREEN_H-12,cell,"L",vt_lang_current()==2?"Clipe":"Clip");
    }
    if(vw->pp->diagnostics && !vw->menu && !vw->quality_menu && !vw->profile_open){
        char a[160],b[160];
        snprintf(a,sizeof(a),"%s %.1f | %s %.2f Mbps | %s %d MHz",vt_opt(VT_O_FPS),vw->fps_now,vt_opt(VT_O_RATE),g_pl.hls.download_kbps/1000.0,vt_opt(VT_O_CPU),vw->cpu_mhz);
        snprintf(b,sizeof(b),"%s %.1f s | %s %d | %s %d",vt_opt(VT_O_BUFFER),pq_span(&g_pl.aq)/90000.0,vt_opt(VT_O_REBUFFERS),g_pl.rebuffers,vt_opt(VT_O_DROPPED),vw->dropped);
        char dims[160];long bandwidth=0;
        if(vw->pp->variants&&vw->pp->variant_current>=0&&vw->pp->variant_current<vw->pp->variant_count)
            bandwidth=vw->pp->variants[vw->pp->variant_current]->bandwidth;
        snprintf(dims,sizeof(dims),"HLS %dx%d | AVC %dx%d | LCD %dx%d | %.2f Mbps",
            vw->pp->width,vw->pp->height,vw->decoded_w,vw->decoded_h,vw->display_w,vw->display_h,bandwidth/1000000.0);
        vita2d_draw_rectangle(0,70,AW,82,COLOR_OSD_BG);
        ui_text_fit(10,91,AW-20,COLOR_WHITE,0.85f,a);
        ui_text_fit(10,116,AW-20,COLOR_WHITE,0.85f,b);
        ui_text_fit(10,141,AW-20,COLOR_WHITE,0.85f,dims);
    }
    if(vw->profile_open){
        const int x=92,y=32,w=776,h=480;
        vita2d_draw_rectangle(0,0,SCREEN_W,SCREEN_H,RGBA8(0,0,0,190));
        ui_round_rect(x,y,w,h,12,C_SURFACE);
        vita2d_draw_rectangle(x+12,y,w-24,3,C_PURPLE);
        ui_text_fit(x+24,y+36,w-48,C_WHITE,1.15f,vw->pp->profile_label?vw->pp->profile_label:"Profile");
        if(profile_avatar)ui_image(profile_avatar,x+24,y+52,76,76);
        else vita2d_draw_fill_circle(x+62,y+90,38,C_PANEL2);
        ui_text_fit(x+116,y+77,w-142,C_WHITE,1.35f,vw->pp->channel);
        char line[192],digits[32];
        snprintf(line,sizeof(line),"@%s",vw->pp->login?vw->pp->login:"");
        ui_text_fit(x+116,y+103,w-142,C_GREY,.9f,line);
        ui_text_fit(x+116,y+48,w-142,C_GREY,.72f,vt_lang_current()==2?"Toque no nome ou foto para abrir o perfil completo":"Tap name or photo for full profile");
        int count=vw->pp->viewer_count?*vw->pp->viewer_count:-1;
        if(count>=0)ui_fmt_viewers(count,digits,sizeof(digits));else snprintf(digits,sizeof(digits),"--");
        snprintf(line,sizeof(line),"%s | %s | %s",vt_tr(VT_I_LIVE),digits,vw->pp->profile_game?vw->pp->profile_game:"");
        ui_text_fit(x+116,y+127,w-142,C_RED,.82f,line);
        ui_text_fit(x+24,y+157,w-48,C_WHITE,.88f,vw->pp->title?vw->pp->title:"");
        if(vw->profile_loading)ui_text_fit(x+24,y+187,w-48,C_GREY,.85f,vt_tr(VT_I_LOADING));
        else {
            const char *s=vw->profile_bio;
            for(int row=0;row<3&&*s;row++){
                char part[256];size_t n=0,best=0,space=0;
                while(s[n]&&s[n]!='\n'){
                    size_t k=n+1;while(s[k]&&((unsigned char)s[k]&0xc0)==0x80)k++;
                    if(k>=sizeof(part))break;
                    memcpy(part,s,k);part[k]=0;if(ui_text_w(.82f,part)>w-48)break;
                    if(s[n]==' ')space=n;best=k;n=k;
                }
                if(!best&&*s!='\n'){best=1;while(s[best]&&((unsigned char)s[best]&0xc0)==0x80)best++;}
                if(s[best]&&s[best]!='\n'&&space)best=space;
                if(row==2){ui_text_fit(x+24,y+187+row*24,w-48,C_GREY,.82f,s);break;}
                memcpy(part,s,best);part[best]=0;ui_text_fit(x+24,y+187+row*24,w-48,C_GREY,.82f,part);
                s+=best;while(*s==' '||*s=='\n')s++;
            }
        }
        ui_text_fit(x+24,y+270,w-48,C_GREY,.75f,twitch_follow_notice(vw->follow_state==1?TW_FOLLOW_ALREADY:TW_FOLLOW_LOCAL_SAVED));
        int saved=vw->pp->saved_has&&vw->pp->saved_has(vw->pp->login);
        const char *detail=vw->follow_status==TW_FOLLOW_LOCAL_SAVED?twitch_follow_notice_detail(vw->follow_status):
            (vw->follow_status==TW_FOLLOW_LOCAL_ERROR||vw->follow_status==TW_FOLLOW_LOCAL_REMOVED)?twitch_follow_notice(vw->follow_status):
            saved?twitch_follow_label(0,1):vw->follow_busy?vt_tr(VT_I_LOADING):vw->follow_state==1?twitch_follow_notice_detail(TW_FOLLOW_ALREADY):"";
        ui_text_fit(x+24,y+294,w-48,C_GREY,.8f,detail);
        const char *labels[]={vw->pp->saved_label?vw->pp->saved_label(saved):"Save on Vita",vt_lang_current()==2?"Perfil completo":vt_lang_current()==1?"Perfil completo":"Full profile",vt_tr(VT_I_BACK)};
        for(int i=0;i<3;i++){
            int yy=y+310+i*44;ui_round_rect(x+24,yy,w-48,41,5,i==vw->profile_sel?C_PURPLE:C_PANEL);
            ui_text_fit(x+38,yy+26,w-76,C_WHITE,.93f,labels[i]);
        }
    }else if(vw->quality_menu){
        int n=vw->pp->variant_count,vis=n<6?n:6;
        const int x=210,w=540,row=48,h=120+vis*row,y=(SCREEN_H-h)/2,ly=y+55;
        vita2d_draw_rectangle(0,0,SCREEN_W,SCREEN_H,RGBA8(0,0,0,190));
        ui_round_rect(x,y,w,h,12,C_SURFACE);
        vita2d_draw_rectangle(x+12,y,w-24,3,C_PURPLE);
        ui_text_fit(x+24,y+36,w-48,C_WHITE,1.15f,vt_tr(VT_I_QUALITY));
        for(int k=0;k<vis&&vw->quality_top+k<n;k++){
            int i=vw->quality_top+k,ry=ly+k*row;
            const m3u8_variant_t *v=vw->pp->variants[i];
            char info[96];
            vita2d_draw_rectangle(x+16,ry,w-32,row-4,i==vw->quality_sel?C_PURPLE:C_PANEL);
            if(i==vw->pp->variant_current)vita2d_draw_rectangle(x+16,ry,4,row-4,C_GREEN);
            ui_text_fit(x+29,ry+28,162,C_WHITE,0.95f,v->name);
            snprintf(info,sizeof(info),"%dx%d / %.0f fps",v->width,v->height,v->fps);
            ui_text_fit(x+205,ry+28,w-241,C_WHITE,0.94f,info);
        }
        vita2d_draw_rectangle(x+16,y+h-52,w/2-24,38,C_PANEL2);
        vita2d_draw_rectangle(x+w/2+8,y+h-52,w/2-24,38,C_PANEL2);
        ui_hint_bounded(x+28,y+h-27,w/2-48,(vw->pp->media_navigation||vw->pp->btn_back==SCE_CTRL_CIRCLE)?"X":"O",vt_tr(VT_I_PLAY));
        ui_hint_bounded(x+w/2+20,y+h-27,w/2-48,(vw->pp->media_navigation||vw->pp->btn_back==SCE_CTRL_CIRCLE)?"O":"X",vt_tr(VT_I_BACK));
    }else if(vw->menu){
        static const vt_text_id labels[]={VT_I_CHAT,VT_I_SHOW_INFO,VT_I_QUALITY,VT_I_SEND_MESSAGE,VT_I_CLOSE,VT_I_CLOSE,VT_I_EXIT};
        vita2d_draw_rectangle(0,0,SCREEN_W,SCREEN_H,RGBA8(0,0,0,190));
        ui_round_rect(205,40,550,464,12,C_SURFACE);
        ui_text_fit(233,78,494,C_WHITE,1.25f,vt_tr(VT_I_MENU));
        for(int i=0;i<7;i++){
            int y=90+i*56;
            if(i==vw->menu_sel)ui_round_rect(222,y,516,49,8,C_PURPLE);
            const char *label=labels[i]==VT_I_SHOW_INFO?
                 (vw->osd?vt_tr(VT_I_HIDE_INFO):vt_tr(VT_I_SHOW_INFO)):i==4?(vw->pp->profile_label?vw->pp->profile_label:"Profile"):vt_tr(labels[i]);
            ui_text_fit(245,y+32,(i==0||i==2)?370:470,C_WHITE,1.02f,label);
            if(i==0)ui_text_fit(625,y+32,100,C_WHITE,0.92f,chat?vt_tr(VT_I_ON):vt_tr(VT_I_OFF));
            if(i==2)ui_text_fit(625,y+32,100,C_WHITE,0.92f,vw->pp->quality);
        }
    }
    /* Loading/advertising status belongs in the middle of the video area.
     * Keep the lower notification area separate, also when chat is visible.
     * Do not cover an active menu or the system keyboard. */
    if (vw->status && !vw->menu && !vw->quality_menu && !vw->profile_open && !vw->ime_active) {
        int tw=ui_text_w(0.94f,vw->status)+54;
        if(tw>AW-24)tw=AW-24;
        int bx=(AW-tw)/2,by=(SCREEN_H-42)/2;
        ui_round_rect(bx,by,tw,42,9,C_PANEL);
        ui_spinner(bx+20,by+21,7,C_PURPLE);
        ui_text_fit(bx+38,by+28,tw-48,C_WHITE,0.94f,vw->status);
    }
    /* Error and action feedback stays at the bottom and is drawn last.
     * Fit inside the video area rather than obscuring the chat panel. */
    if(vw->notice&&sceKernelGetProcessTimeWide()<vw->notice_until){
        int nw=AW-32;
        if(nw>640)nw=640;
        int nx=(AW-nw)/2;
        ui_round_rect(nx,SCREEN_H-83,nw,47,9,C_PANEL);
        ui_text_fit(nx+20,SCREEN_H-54,nw-40,C_WHITE,0.95f,vw->notice);
    }

    vita2d_end_drawing();
    vita2d_common_dialog_update(); /* required by nonblocking Vita OSK */
    vita2d_wait_rendering_done();
    img_free_trash(); /* the GPU is idle now: evicted emote textures can go */
    vita2d_swap_buffers();
}


typedef struct {const vslot_t *frame;view_t *view;} ime_player_ctx_t;
static void ime_player_pump(void *arg){
    ime_player_ctx_t *ctx=arg;
    draw_view(ctx->frame,ctx->view);
}

/* ------------------------------------------------------------------- main */

int player_run(const player_params_t *pp, char *err, int errlen)
{
    player_t *pl = &g_pl;
    vpipe_t *vp = &g_vp;
    vdec_t vdec;
    view_t view;
    int ret = 0;

    memset(pl, 0, sizeof(*pl));
    memset(&view, 0, sizeof(view));
    view.pp = pp;
    view.osd = 1;view.recording_pos=pp->start_seconds-pp->timeline_offset;view.recording_duration=pp->duration_seconds;
    pl->profile=pp->profile;
    pl->volume=pp->volume?*pp->volume:100;
    view.cpu_mhz=vt_profile_clock(pp->profile,pp->height,pp->fps);
    err[0] = 0;
    LOGI("startup: clock mutex begin");
    int clock_rc = pthread_mutex_init(&pl->clock.lock, NULL);
    if (clock_rc != 0) {
        snprintf(err, (size_t)errlen, "Clock mutex error: %d", clock_rc);
        return -1;
    }
    LOGI("startup: clear thumbnail cache");
    img_clear();
    img_frame();
    LOGI("startup: thumbnail cache cleared; open decoder");

    if (vdec_open(&vdec, pp->width, pp->height, err, errlen) < 0) {
        pthread_mutex_destroy(&pl->clock.lock);
        return -1;
    }

    LOGI("startup: decoder ready; initialize video queue");
    if (pq_init(&pl->vq, pp->recorded?240:900, (pp->recorded?6:24) * 1024 * 1024) != 0) {
        snprintf(err, (size_t)errlen, "Video queue initialization failed");
        vdec_close(&vdec);
        pthread_mutex_destroy(&pl->clock.lock);
        return -1;
    }
    LOGI("startup: initialize audio queue");
    if (pq_init(&pl->aq, pp->recorded?300:1500, pp->recorded?512*1024:2*1024*1024) != 0) {
        snprintf(err, (size_t)errlen, "Audio queue initialization failed");
        pq_destroy(&pl->vq);
        vdec_close(&vdec);
        pthread_mutex_destroy(&pl->clock.lock);
        return -1;
    }
    LOGI("startup: queues ready; start video pipeline");

    if (vpipe_start(vp, &vdec, pp, err, errlen) < 0) {
        ret = -1;
        goto out_queues;
    }
    LOGI("startup: start HLS thread");
    int hls_rc=pp->preview_playlist?hls_start_preview(&pl->hls,pp->preview_playlist,&pl->vq,&pl->aq,pp->start_seconds):pp->recorded==2?hls_start_clip(&pl->hls,pp->url,&pl->vq,&pl->aq,pp->duration_seconds,pp->start_seconds):pp->recorded?hls_start_recorded(&pl->hls,pp->url,&pl->vq,&pl->aq,pp->start_seconds):hls_start_capture(&pl->hls,pp->url,&pl->vq,&pl->aq,vt_profile_edge(pp->profile),pp->login,pp->chat_oauth,pp->width,pp->height,pp->fps,pp->ad_backup_url,pp->live_capture);
    if(hls_rc<0){
        snprintf(err, (size_t)errlen, vt_msg("No se pudo crear el hilo de descarga"));
        LOGE("%s", err);
        ret = -1;
        pq_abort(&pl->vq);
        vpipe_stop(vp);
        vpipe_free(vp);
        goto out_queues;
    }

    LOGI("startup: HLS thread started; start audio thread");
    pl->audio_running = 1;
    /* audio on core 2: it must never preempt the drawing thread on core 0 */
    pthread_attr_t audio_attr;
    int audio_rc = pthread_attr_init(&audio_attr);
    if (audio_rc == 0) {
        audio_rc = pthread_attr_setstacksize(&audio_attr, 128 * 1024);
        if (audio_rc == 0) audio_rc = pthread_create(&pl->audio_thread, &audio_attr, audio_thread, pl);
        pthread_attr_destroy(&audio_attr);
    }
    if (audio_rc == 0) pl->audio_started = 1;
    else {
        LOGE("audio: worker creation failed (%d)", audio_rc);
        pl->audio_running = 0;
        pl->audio_done = 1;
        pl->audio_failed = 1;
    }

    LOGI("startup: audio thread setup done; start chat if enabled");
    if (pp->chat_on && *pp->chat_on && pp->login)
        chat_set_session(pp->chat_session);
        chat_start(pp->login,pp->chat_nick,pp->chat_oauth,pp->chat_sender_id);
    img_set_deferred_free(1);
    LOGI("startup: enter player input/render loop");

    ui_input_t input={0};
    ui_input_poll(&input); /* swallow X from quality selector */
    int should_exit=0;
    unsigned previous_held=input.held;
    task_t viewer_task={0};viewer_job_t viewer_job={0};
    task_t profile_task={0},follow_task={0};live_profile_job_t profile_job={0};
    twitch_follow_job_t follow_job={0};follow_job.state=-1;view.follow_state=pp->account_follow_known?1:-1;
    if(pp->login)snprintf(follow_job.login,sizeof(follow_job.login),"%s",pp->login);
    if(pp->chat_oauth)snprintf(follow_job.oauth,sizeof(follow_job.oauth),"%s",pp->chat_oauth);
    if(pp->chat_sender_id)snprintf(follow_job.user_id,sizeof(follow_job.user_id),"%s",pp->chat_sender_id);
    view.follow_status=follow_job.oauth[0]?0:1;
    if(pp->login)snprintf(profile_job.login,sizeof(profile_job.login),"%s",pp->login);
    if(pp->login)snprintf(viewer_job.login,sizeof(viewer_job.login),"%s",pp->login);
    uint64_t viewer_last_request=0;

    int64_t recording_base=TS_NOPTS;uint64_t pause_started=0;
    int cur = -1;                 /* slot on screen */
    int presented = 0, late_dropped = 0;
    uint64_t start_us = sceKernelGetProcessTimeWide();
    uint64_t last_draw = 0, last_tick = 0, last_stats = start_us, osd_until = start_us + 5000000ULL;
    uint64_t last_present = 0, max_present_gap = 0;
    uint64_t draw_us = 0, draw_max = 0;
    unsigned draw_n = 0;
    int freerun_logged = 0, first_logged = 0;
    int fps_frames=0, bad_intervals=0, stable_intervals=0, prev_rebuffers=0;
    uint64_t fps_time=start_us;
    /* a frame is due when its pts is within half a frame of the clock */
    int64_t half = (int64_t)(90000.0 / ((pp->fps > 1 ? pp->fps : 30.0f) * 2.0f));

    for (;;) {
        uint64_t now_us = sceKernelGetProcessTimeWide();

        ui_input_poll(&input);
        unsigned pressed=input.pressed;
        unsigned new_buttons=input.held & ~previous_held;
        previous_held=input.held;
        int refresh_viewers=0,write_chat=0,open_profile=0;
        if(view.follow_busy&&task_done(&follow_task)){
            view.follow_busy=0;view.follow_state=follow_job.state<0&&pp->account_follow_known?1:follow_job.state;if(view.follow_status<TW_FOLLOW_LOCAL_SAVED)view.follow_status=view.follow_state==1?TW_FOLLOW_ALREADY:follow_job.status;last_draw=0;
        }
        if(view.profile_loading&&task_done(&profile_task)){
            view.profile_loading=0;view.profile_fetched=1;
            if(profile_task.result==0)snprintf(view.profile_bio,sizeof(view.profile_bio),"%s",profile_job.bio);
            last_draw=0;
        }
        if(view.ime_active){
            SceCommonDialogStatus ime_status=sceImeDialogGetStatus();
            if(ime_status==SCE_COMMON_DIALOG_STATUS_FINISHED){
                SceImeDialogResult result={0};int result_rc=sceImeDialogGetResult(&result);
                ime_player_ctx_t ime_ctx={cur>=0?&vp->slot[cur]:NULL,&view};
                int term_rc=vt_ime_close(ime_player_pump,&ime_ctx);view.ime_active=0;
                LOGI("chat: keyboard closed result=%08X term=%08X button=%d",(unsigned)result_rc,(unsigned)term_rc,result.button);
                if(result_rc>=0&&result.button==SCE_IME_DIALOG_BUTTON_ENTER){
                    char message[CHAT_TEXT_MAX];vt_utf16_to_utf8(message,sizeof(message),(const uint16_t*)chat_buffer16,256);int rc=chat_queue_message(message);
                    view.notice=vt_msg(rc==0?"Message queued (server confirmation pending)":rc==-2?"Please slow down (chat rate limit)":rc==-3?"Message empty or too long":"Failed to send chat message");view.notice_until=now_us+3000000ULL;
                }
                last_draw=0;
            }else if(ime_status==SCE_COMMON_DIALOG_STATUS_NONE){
                LOGW("chat: keyboard dialog unexpectedly ended");
                vt_ime_close(NULL,NULL);view.ime_active=0;last_draw=0;
            }
            pressed=0;input.touch_tap=0;
        }else if(view.profile_open){
            int accept=pp->btn_back==SCE_CTRL_CIRCLE?SCE_CTRL_CROSS:SCE_CTRL_CIRCLE;
            int choose=!!(pressed&accept);
            if(pressed&(pp->btn_back|SCE_CTRL_START|SCE_CTRL_SELECT)){view.profile_open=0;choose=0;}
            else {
                if(ui_touch_hit(&input,116,84,728,76)){ret=PLAYER_FULL_PROFILE;should_exit=1;choose=0;}
                if(pressed&SCE_CTRL_UP)view.profile_sel=(view.profile_sel+2)%3;
                if(pressed&SCE_CTRL_DOWN)view.profile_sel=(view.profile_sel+1)%3;
                for(int i=0;i<3;i++)if(ui_touch_hit(&input,116,342+i*44,728,41)){view.profile_sel=i;choose=1;break;}
                if(choose){
                    if(view.profile_sel==2)view.profile_open=0;
                    else if(view.profile_sel==1){ret=PLAYER_FULL_PROFILE;should_exit=1;}
                    else if(view.profile_sel==0){
                        int saved=pp->saved_has&&pp->saved_has(pp->login);
                        const char *avatar=follow_job.avatar[0]?follow_job.avatar:pp->profile_avatar;
                        int rc=saved?(pp->saved_toggle?pp->saved_toggle(pp->login,pp->channel,avatar):-1):(pp->saved_ensure?pp->saved_ensure(pp->login,pp->channel,avatar):-1);
                        view.follow_status=rc<0?TW_FOLLOW_LOCAL_ERROR:rc?TW_FOLLOW_LOCAL_SAVED:TW_FOLLOW_LOCAL_REMOVED;
                    }
                }
            }
            if(pressed||input.touch_tap)last_draw=0;
        }else {
            int accept=pp->media_navigation?SCE_CTRL_CROSS:pp->btn_back==SCE_CTRL_CIRCLE?SCE_CTRL_CROSS:SCE_CTRL_CIRCLE;
            unsigned back=pp->media_navigation?SCE_CTRL_CIRCLE:pp->btn_back;
            if(view.quality_menu){
                int n=pp->variant_count,vis=n<6?n:6;
                const int x=210,w=540,row=48,h=120+vis*row,y=(SCREEN_H-h)/2,ly=y+55;
                int choose=!!(pressed&accept);
                if(pressed&(back|SCE_CTRL_SELECT|(pp->media_navigation?SCE_CTRL_TRIANGLE:0))){view.quality_menu=0;view.menu=!pp->recorded;choose=0;}
                if(view.quality_menu&&n>0){
                    if(pressed&(SCE_CTRL_UP|SCE_CTRL_LEFT))view.quality_sel=(view.quality_sel+n-1)%n;
                    if(pressed&(SCE_CTRL_DOWN|SCE_CTRL_RIGHT))view.quality_sel=(view.quality_sel+1)%n;
                    if(input.touch_tap){
                        if(ui_touch_hit(&input,x+w/2+8,y+h-52,w/2-24,38)){view.quality_menu=0;view.menu=!pp->recorded;choose=0;}
                        else if(ui_touch_hit(&input,x+16,y+h-52,w/2-24,38))choose=1;
                        else for(int k=0;k<vis&&view.quality_top+k<n;k++)
                            if(ui_touch_hit(&input,x+16,ly+k*row,w-32,row-4)){view.quality_sel=view.quality_top+k;choose=1;break;}
                    }
                    if(view.quality_sel<view.quality_top)view.quality_top=view.quality_sel;
                    if(view.quality_sel>=view.quality_top+vis)view.quality_top=view.quality_sel-vis+1;
                    if(choose&&view.quality_menu){
                        if(view.quality_sel!=pp->variant_current&&pp->variant_requested){
                            *pp->variant_requested=view.quality_sel;ret=PLAYER_CHANGE_QUALITY;should_exit=1;
                        }
                        view.quality_menu=0;view.menu=0;
                    }
                }
                if(pressed||input.touch_tap)last_draw=0;
            }else if(pp->recorded){
                if(pressed&back)should_exit=1;
                if((pressed&accept)||(view.osd&&ui_touch_hit(&input,0,SCREEN_H-36,160,36))){
                    pl->paused=!pl->paused;view.paused=pl->paused;view.osd=1;last_draw=0;
                    if(pl->paused)pause_started=now_us;
                    else if(pl->freerun&&pause_started){pthread_mutex_lock(&pl->clock.lock);pl->clock.time+=now_us-pause_started;pthread_mutex_unlock(&pl->clock.lock);}
                }
                if((pressed&(pp->media_navigation?SCE_CTRL_TRIANGLE:SCE_CTRL_SELECT))||(view.osd&&ui_touch_hit(&input,SCREEN_W-180,0,180,64))){
                    view.seek=(vt_seek_burst){0};
                    if(pp->variant_count>0&&pp->variant_requested){view.quality_menu=1;view.quality_sel=pp->variant_current;view.quality_top=view.quality_sel>=6?view.quality_sel-5:0;}
                    last_draw=0;
                }
                if(pressed&(pp->media_navigation?SCE_CTRL_SELECT:SCE_CTRL_TRIANGLE)){view.osd=!view.osd;last_draw=0;}
                if(!should_exit&&!view.quality_menu&&!pp->preview_end&&pp->recorded==1&&(pressed&SCE_CTRL_SQUARE)){ret=PLAYER_CREATE_CLIP;should_exit=1;}
                if(!should_exit&&!view.quality_menu&&pp->media_navigation){
                    if((pressed&SCE_CTRL_LTRIGGER)&&pp->can_previous){ret=PLAYER_PREVIOUS_MEDIA;should_exit=1;}
                    else if((pressed&SCE_CTRL_RTRIGGER)&&pp->can_next){ret=PLAYER_NEXT_MEDIA;should_exit=1;}
                }
                if(!should_exit&&!view.quality_menu&&(!pp->preview_end||pp->media_navigation)&&pp->seek_requested&&view.recording_duration>0){
                    int duration=view.recording_duration;
                    int delta=0;
                    if(new_buttons&SCE_CTRL_UP)delta=30;
                    else if(new_buttons&SCE_CTRL_DOWN)delta=-30;
                    else if(new_buttons&SCE_CTRL_RIGHT)delta=10;
                    else if(new_buttons&SCE_CTRL_LEFT)delta=-10;
                    if(delta){
                        vt_seek_add(&view.seek,view.recording_pos,duration,delta,now_us);
                        view.osd=1;last_draw=0;
                    }else if(view.osd&&ui_touch_hit(&input,24,SCREEN_H-84,SCREEN_W-48,24)){
                        int target=vt_seek_clamp((int64_t)(input.touch_x-24)*duration/(SCREEN_W-48),duration);
                        view.seek=(vt_seek_burst){0};
                        *pp->seek_requested=target;ret=PLAYER_SEEK;should_exit=1;
                    }else if(input.touch_tap&&input.touch_y>=64&&input.touch_y<SCREEN_H-88){
                        int side=input.touch_x<SCREEN_W*2/5?-1:input.touch_x>SCREEN_W*3/5?1:0;
                        if(side){
                            if(vt_seek_tap(&view.seek,side,view.recording_pos,duration,now_us)){view.osd=1;last_draw=0;}
                        }else {view.osd=!view.osd;last_draw=0;}
                    }
                    if(!should_exit&&vt_seek_due(&view.seek,now_us)){
                        *pp->seek_requested=view.seek.target;
                        ret=PLAYER_SEEK;should_exit=1;
                    }
                }
            }else{
            if(pressed&SCE_CTRL_SELECT){view.menu=!view.menu;view.menu_sel=0;last_draw=0;}
            if(view.menu){
                if(pressed&SCE_CTRL_UP)view.menu_sel=(view.menu_sel+6)%7;
                if(pressed&SCE_CTRL_DOWN)view.menu_sel=(view.menu_sel+1)%7;
                if(input.touch_tap){
                    int hit=0;
                    for(int i=0;i<7;i++)
                        if(ui_touch_hit(&input,222,90+i*56,516,49)){
                            view.menu_sel=i;pressed|=accept;hit=1;break;
                        }
                    if(!hit)view.menu=0;
                }
                if(pressed&pp->btn_back)view.menu=0;
                if(view.menu&&(pressed&accept)){
                    switch(view.menu_sel){
                        case 0:
                            if(pp->chat_on&&pp->login){
                                *pp->chat_on=!*pp->chat_on;
                                if(*pp->chat_on&&!chat_running())
                                    chat_start(pp->login,pp->chat_nick,pp->chat_oauth,pp->chat_sender_id);
                            }
                            break;
                        case 1:view.osd=!view.osd;osd_until=view.osd?now_us+5000000ULL:0;refresh_viewers=view.osd;last_draw=0;view.menu=0;break;
                        case 2:
                            if(pp->variants&&pp->variant_count>0&&pp->variant_requested){
                                view.quality_menu=1;view.menu=0;
                                view.quality_sel=pp->variant_current;
                                if(view.quality_sel<0||view.quality_sel>=pp->variant_count)view.quality_sel=0;
                                view.quality_top=view.quality_sel>=6?view.quality_sel-5:0;
                            }
                            last_draw=0;break;
                        case 3:write_chat=1;view.menu=0;break;
                        case 4:open_profile=1;view.menu=0;break;
                        case 5:view.menu=0;break;
                        case 6:should_exit=1;break;
                    }
                }
            }else{
                if(pressed&pp->btn_back)should_exit=1;
                if(pressed&SCE_CTRL_START)open_profile=1;
                if(pressed&SCE_CTRL_RTRIGGER)write_chat=1;
                if(pressed&SCE_CTRL_LTRIGGER){ret=PLAYER_CREATE_CLIP;should_exit=1;}
                if(pressed&SCE_CTRL_TRIANGLE){view.osd=!view.osd;osd_until=view.osd?now_us+5000000ULL:0;refresh_viewers=view.osd;last_draw=0;}
                if((pressed&SCE_CTRL_SQUARE)&&pp->chat_on&&pp->login){
                    *pp->chat_on=!*pp->chat_on;
                    if(*pp->chat_on&&!chat_running())
                        chat_start(pp->login,pp->chat_nick,pp->chat_oauth,pp->chat_sender_id);
                    LOGI("chat %s",*pp->chat_on?"visible":"oculto");
                }
                if(pressed&(SCE_CTRL_LEFT|SCE_CTRL_RIGHT)){
                    int v=pl->volume+(pressed&SCE_CTRL_RIGHT?5:-5);
                    if(v<0)v=0;
                    if(v>100)v=100;
                    pl->volume=v; if(pp->volume)*pp->volume=v;
                    snprintf(view.volume_notice,sizeof(view.volume_notice),"%s: %d%%",vt_opt(VT_O_VOLUME),v);
                    view.notice=view.volume_notice;view.notice_until=now_us+2000000ULL;
                }
                if(input.touch_tap){
                    int aw=(pp->chat_on&&*pp->chat_on)?SCREEN_W-CHAT_PANEL_W:SCREEN_W;
                    if(aw<SCREEN_W&&input.touch_x>=aw&&input.touch_y>=SCREEN_H-38)write_chat=1;
                    else if(view.osd&&input.touch_y<64&&input.touch_x<aw/2)open_profile=1;
                    else if(view.osd&&input.touch_y<64&&input.touch_x>=aw/2&&input.touch_x<aw&&pp->variants&&pp->variant_count>0&&pp->variant_requested){
                        view.quality_menu=1;view.quality_sel=pp->variant_current;
                        if(view.quality_sel<0||view.quality_sel>=pp->variant_count)view.quality_sel=0;
                        view.quality_top=view.quality_sel>=6?view.quality_sel-5:0;
                    }else{view.osd=!view.osd;osd_until=view.osd?now_us+5000000ULL:0;refresh_viewers=view.osd;}
                    last_draw=0;
                }
            }
            }
        }
        if(should_exit)break;
        if(open_profile&&twitch_channel_url(pp->login,view.profile_url,sizeof(view.profile_url))==0){
            view.profile_open=1;view.profile_sel=0;view.menu=0;view.quality_menu=0;last_draw=0;
            if(follow_job.oauth[0]&&!view.follow_busy){view.follow_busy=task_start(&follow_task,twitch_follow_run,&follow_job)==0;view.follow_status=view.follow_busy?2:4;}
            if(!view.profile_loading&&!view.profile_fetched&&task_start(&profile_task,live_profile_fetch,&profile_job)==0)view.profile_loading=1;
            LOGI("profile: opened in player @%s",pp->login);
        }
        if(write_chat&&!view.profile_open&&!view.ime_active){
            LOGI("chat: compose requested logged=%d reader=%d ready=%d",pp->chat_oauth&&*pp->chat_oauth,chat_running(),chat_can_send());
            if(!pp->chat_oauth||!*pp->chat_oauth||!pp->chat_nick||!*pp->chat_nick){
                view.notice=vt_tr(VT_I_LOGIN_TO_CHAT);view.notice_until=now_us+3000000ULL;
            }else{
                if(pp->chat_on)*pp->chat_on=1;
                if(!chat_running())chat_start(pp->login,pp->chat_nick,pp->chat_oauth,pp->chat_sender_id);
                /* Composer does not depend on anonymous IRC JOIN or network readiness.
                   Queue and HTTPS confirmation handle delivery separately. */
                view.menu=0;view.quality_menu=0;view.notice=NULL;
                ime_player_ctx_t ime_ctx={cur>=0?&vp->slot[cur]:NULL,&view};
                int ime_rc=open_chat_keyboard(ime_player_pump,&ime_ctx);
                if(ime_rc>=0)view.ime_active=1;
                else{
                    snprintf(view.chat_notice,sizeof(view.chat_notice),"%s [%08X]",vt_msg("Cannot open PS Vita keyboard"),(unsigned)ime_rc);
                    view.notice=view.chat_notice;view.notice_until=now_us+5000000ULL;
                }
            }
            last_draw=0;
        }
        if(view.viewers_busy&&task_done(&viewer_task)){
            view.viewers_busy=0;
            if(viewer_task.result==0&&pp->viewer_count)*pp->viewer_count=viewer_job.count;
            last_draw=0;
        }
        if(refresh_viewers&&!view.viewers_busy&&viewer_job.login[0]&&pp->viewer_count&&
           (!viewer_last_request||now_us-viewer_last_request>=2000000ULL)){
            viewer_last_request=now_us;
            if(task_start(&viewer_task,viewer_fetch,&viewer_job)==0)view.viewers_busy=1;
            last_draw=0;
        }
        if(pp->recorded&&(pressed||input.touch_tap)){
            osd_until=view.osd?now_us+5000000ULL:0;
        }
        if (osd_until && now_us >= osd_until && !view.quality_menu) {
            view.osd = 0;
            osd_until = 0;
            last_draw = 0;
        }
        if (now_us - last_tick > 1000000) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); /* keep the screen on */
            last_tick = now_us;
        }
        if(now_us-fps_time>=1000000ULL){view.fps_now=(float)(fps_frames*1000000.0/(now_us-fps_time));fps_frames=0;fps_time=now_us;}
        if (now_us - last_stats > 10000000ULL) { /* every 10 s */
            int idec, iev, ifail, ifree;
            img_stats(&idec, &iev, &ifail, &ifree);
            LOGI("estado: %d AUs -> %d imagenes (%d multiples), %d mostradas, %d tarde, %d sin decodificar, "
                 "%d omitidas por chat, %d saltos, %d errores; cola video %.1f s, audio %.1f s, listas %d, "
                 "%d rebuffers, chat %s",
                 vp->st_aus, vp->st_pics, vp->st_multi, presented, late_dropped, vp->st_undecoded,
                 vp->st_chat_skip, vp->st_idr_jumps, vp->st_errors, pq_span(&pl->vq) / PTS_PER_MS / 1000.0,
                 pq_span(&pl->aq) / PTS_PER_MS / 1000.0, vp->nready, pl->rebuffers,
                 pp->chat_on && *pp->chat_on ? "visible" : "oculto");
            LOGI("tiempos: decodificar %.1f ms (max %.1f), dibujar %.1f ms (max %.1f), hueco max entre "
                 "imagenes %.0f ms, retraso max %.2f s; panel chat %d x %.1f ms; emotes: %d decodificados, "
                 "%d expulsados, %d fallidos, %d liberados",
                 vp->st_aus ? vp->st_dec_us / 1000.0 / vp->st_aus : 0.0, vp->st_dec_max / 1000.0,
                 draw_n ? draw_us / 1000.0 / draw_n : 0.0, draw_max / 1000.0, max_present_gap / 1000.0,
                 vp->st_max_lag / PTS_PER_MS / 1000.0, chat_view_renders(), chat_view_avg_ms_x10() / 10.0,
                 idec, iev, ifail, ifree);
            int poor=(pl->rebuffers-prev_rebuffers>=2)||vp->st_idr_jumps>=2||(presented>30&&late_dropped>presented/4);
            prev_rebuffers=pl->rebuffers;
            if(poor){bad_intervals++;stable_intervals=0;if(view.cpu_mhz<444 && scePowerSetArmClockFrequency(444)>=0)view.cpu_mhz=444;}
            else {bad_intervals=0;stable_intervals++;}
            if(stable_intervals>=3 && pp->profile!=VT_PROFILE_QUALITY && pp->height<=540 && pp->fps<=40 && view.cpu_mhz>333){
                if(scePowerSetArmClockFrequency(333)>=0)view.cpu_mhz=333;
            }
            if(bad_intervals>=2 && pp->auto_quality && pp->has_lower && !pl->hls.in_ad){
                ret=PLAYER_LOWER_QUALITY;break;
            }
            vp->st_aus = vp->st_pics = vp->st_multi = vp->st_errors = vp->st_undecoded = 0;
            vp->st_chat_skip = vp->st_idr_jumps = 0;
            vp->st_dec_us = vp->st_dec_max = 0;
            vp->st_max_lag = 0;
            presented = late_dropped = 0;
            draw_us = draw_max = max_present_gap = 0;
            draw_n = 0;
            last_stats = now_us;
        }
        if (pl->freerun && !freerun_logged) {
            LOGW("video: reloj sin audio (audio fallido o ausente)");
            freerun_logged = 1;
        }
        if (vp->fatal) {
            snprintf(err, (size_t)errlen, "%s", vp->fatal_msg);
            ret = -1;
            break;
        }
        if(pl->hls.finished && pp->ad_backup_url && pp->ad_backup_url[0]) {
            ret=PLAYER_AD_SWITCH;
            break;
        }
        /* end: everything shown, or the last frames can never become due (audio
         * ran out first, its clock stopped) and nothing was shown for 3 s */
        if (!pl->paused&&!view.seek.active&&pl->hls.finished && pq_count(&pl->vq) == 0 &&
            (vp->nready == 0 ||
             (pq_count(&pl->aq) == 0 && now_us - (last_present ? last_present : start_us) > 3000000ULL))) {
            snprintf(err, (size_t)errlen, "%s", pl->hls.error[0] ? pl->hls.error : vt_msg("El stream ha terminado"));
            ret = pp->recorded&&!pl->hls.error[0]?PLAYER_RECORDING_END:pl->hls.retryable ? PLAYER_RECONNECT : -1;
            break;
        }

        /* status line */
        if (pl->hls.in_ad)
            view.status = vt_msg("Anuncio de Twitch: saltando...");
        else if (cur < 0 || pl->audio_buffering)
            view.status = vt_msg("Cargando...");
        else
            view.status = NULL;

        /* no audio thread to drain it: a full audio queue would block the hls
         * thread in pq_put and starve the video */
        if (pl->audio_done && pq_count(&pl->aq) > 0)
            pq_flush(&pl->aq);

        /* clock: audio is master; without audio, run on wall time from the first frame */
        int64_t clk = clock_get(&pl->clock);
        {
            pthread_mutex_lock(&vp->lock);
            int64_t head = vp->nready ? vp->slot[vp->ready[0]].pts : TS_NOPTS;
            pthread_mutex_unlock(&vp->lock);
            if (!pl->freerun) {
                int no_audio = pl->audio_failed ||
                               (clk == TS_NOPTS && pq_count(&pl->aq) == 0 && pq_span(&pl->vq) > 3000 * PTS_PER_MS);
                if (no_audio && head != TS_NOPTS) {
                    clock_set(&pl->clock, head, INT64_MAX / 2);
                    pl->freerun = 1;
                    clk = head;
                }
            } else if (head != TS_NOPTS && clk != TS_NOPTS && clk - head > 1000 * PTS_PER_MS) {
                /* wall clock ran ahead (download stall): nothing would ever be on
                 * time again, the decoder would only show keyframes. Re-anchor. */
                clock_set(&pl->clock, head, INT64_MAX / 2);
                clk = head;
            }
        }

        /* pick the newest frame that is due; older due ones are dropped */
        int next = -1;
        if (!pl->paused&&clk != TS_NOPTS) {
            pthread_mutex_lock(&vp->lock);
            while (vp->nready > 0) {
                int h = vp->ready[0];
                int64_t hp = vp->slot[h].pts;
                int64_t d = hp == TS_NOPTS ? 0 : hp - clk;
                if (d > 5000 * PTS_PER_MS)
                    d = 0; /* broken timestamps: don't stall */
                if (d > half)
                    break; /* not due yet */
                if (vp->nready > 1) {
                    int64_t np = vp->slot[vp->ready[1]].pts;
                    if (np != TS_NOPTS && hp != TS_NOPTS && np - clk <= half && np - clk > -5000 * PTS_PER_MS) {
                        /* the following frame is due too: this one is late */
                        vp->slot[h].state = SLOT_FREE;
                        memmove(vp->ready, vp->ready + 1, sizeof(int) * (size_t)(vp->nready - 1));
                        vp->nready--;
                        late_dropped++;
                        view.dropped++;
                        continue;
                    }
                }
                next = h;
                memmove(vp->ready, vp->ready + 1, sizeof(int) * (size_t)(vp->nready - 1));
                vp->nready--;
                break;
            }
            pthread_mutex_unlock(&vp->lock);
        }

        if(pp->recorded&&pl->hls.position_ready){
            view.recording_duration=(pp->timeline_offset||(pp->media_navigation&&pp->preview_end))?pp->duration_seconds:pl->hls.duration_seconds;
            if(next>=0&&vp->slot[next].pts!=TS_NOPTS){
                if(recording_base==TS_NOPTS)recording_base=vp->slot[next].pts;
                int64_t delta=vp->slot[next].pts-recording_base;
                if(delta>=0){view.recording_pos=pl->hls.actual_start+(int)(delta/90000)-pp->timeline_offset;if(view.recording_pos<0)view.recording_pos=0;if(view.recording_pos>view.recording_duration)view.recording_pos=view.recording_duration;}
                if(pp->position_seconds)*pp->position_seconds=view.recording_pos;
            }
        }
        if(pp->preview_end&&view.recording_pos>=pp->preview_end){ret=PLAYER_RECORDING_END;break;}
        if (next >= 0) {
            uint64_t t_draw = sceKernelGetProcessTimeWide();
            draw_view(&vp->slot[next], &view); /* ends with vita2d_wait_rendering_done() */
            uint64_t t_end = sceKernelGetProcessTimeWide();
            /* the GPU is done with the previous frame: give its texture back */
            pthread_mutex_lock(&vp->lock);
            if (cur >= 0)
                vp->slot[cur].state = SLOT_FREE;
            vp->slot[next].state = SLOT_SHOWN;
            pthread_mutex_unlock(&vp->lock);
            cur = next;
            vp->shown_any = 1;
            if (!first_logged) {
                LOGI("video: primer frame %dx%d tras %llu ms", vp->slot[cur].w, vp->slot[cur].h,
                     (unsigned long long)(t_end - start_us) / 1000ULL);
                LOGI("video: display %dx%d, filter linear, selected %s",view.display_w,view.display_h,pp->quality);
                first_logged = 1;
                if(pp->recorded&&view.osd)osd_until=t_end+5000000ULL;
            }
            if (last_present && t_draw - last_present > max_present_gap)
                max_present_gap = t_draw - last_present;
            last_present = t_draw;
            last_draw = t_end;
            presented++;
            fps_frames++;
            draw_us += t_end - t_draw;
            draw_n++;
            if (t_end - t_draw > draw_max)
                draw_max = t_end - t_draw;
        } else if (now_us - last_draw > (view.ime_active?16667ULL:100000ULL)) {
            /* nothing new: keep the status/chat/OSD alive */
            if (!last_draw) LOGI("startup: first player draw begin");
            draw_view(cur >= 0 ? &vp->slot[cur] : NULL, &view);
            if (!last_draw) LOGI("startup: first player draw done");
            last_draw = sceKernelGetProcessTimeWide();
        } else {
            sceKernelDelayThread(2000);
        }
    }

    LOGI("fin de reproduccion tras %llu s: %d imagenes, %d descartados, %d errores de video, %d rebuffers%s%s",
         (unsigned long long)(sceKernelGetProcessTimeWide() - start_us) / 1000000ULL, vp->total_pics,
         view.dropped, vp->total_errors, pl->rebuffers, ret < 0 ? ", motivo: " : " (salida del usuario)",
         ret < 0 ? err : "");

    if(view.ime_active){
        ime_player_ctx_t ime_ctx={cur>=0?&vp->slot[cur]:NULL,&view};
        vt_ime_close(ime_player_pump,&ime_ctx);view.ime_active=0;
    }
    if(view.viewers_busy){task_cancel(&viewer_task);while(!task_done(&viewer_task))sceKernelDelayThread(10000);}
    if(view.profile_loading){task_cancel(&profile_task);while(!task_done(&profile_task))sceKernelDelayThread(10000);}
    if(view.follow_busy){task_cancel(&follow_task);while(!task_done(&follow_task))sceKernelDelayThread(10000);}
    /* teardown: unblock producers first, then stop threads */
    pq_abort(&pl->vq);
    pq_abort(&pl->aq);
    vpipe_stop(vp);
    chat_stop();
    chat_set_session(NULL);
    hls_stop(&pl->hls);
    pl->audio_running = 0;
    if (pl->audio_started) pthread_join(pl->audio_thread, NULL);
    vpipe_free(vp);

out_queues:
    pq_destroy(&pl->vq);
    pq_destroy(&pl->aq);
    vdec_close(&vdec);
    chat_view_free();
    img_set_deferred_free(0);
    pthread_mutex_destroy(&pl->clock.lock);
    return ret;
}

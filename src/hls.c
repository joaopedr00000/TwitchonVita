#include "i18n.h"
/*
 * Live HLS downloader: polls the media playlist, downloads new MPEG-TS
 * segments, demuxes them and pushes elementary-stream packets to the
 * player queues. Twitch stitched ads are skipped (like streamlink's
 * --twitch-disable-ads).
 */
#include "hls.h"
#include "adts.h"
#include "http.h"
#include "tls_trust.h"
#include "twitch.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>

/* live edge is selected by the playback profile */

static int on_es(void *user, int stream, const uint8_t *data, int size,
                 int64_t pts, int64_t dts)
{
    hls_t *h = user;

    if (!h->running)
        return -1;

    if (stream == TS_STREAM_VIDEO) {
        packet_t *p = pkt_new(data, size, pts, dts);
        if (!p)
            return 0;
        return pq_put(h->vq, p);
    }

    /* Split the PES into single ADTS frames, each with its own pts. */
    int off = 0, i = 0;
    while (off < size) {
        adts_info_t ai;
        if (adts_parse(data + off, size - off, &ai) < 0) {
            off++; /* resync */
            continue;
        }
        if (ai.frame_len > size - off)
            break;
        int64_t fpts = pts;
        if (pts != TS_NOPTS)
            fpts = pts + (int64_t)i * 1024 * 90000 / ai.sample_rate;
        packet_t *p = pkt_new(data + off, ai.frame_len, fpts, fpts);
        if (p && pq_put(h->aq, p) < 0)
            return -1;
        off += ai.frame_len;
        i++;
    }
    return 0;
}

static void sleep_ms_while_running(hls_t *h, int ms)
{
    while (ms > 0 && h->running) {
        int step = ms > 50 ? 50 : ms;
        sceKernelDelayThread(step * 1000);
        ms -= step;
    }
}

static void demux_discontinuity(hls_t *h)
{
    ts_demux_discontinuity(&h->demux);
    mp4_demux_discontinuity(&h->mp4);
}

/* Store URL references only, never entire live segments. Called after the
 * worker is stopped; the snapshot can expire at the CDN. */
static void live_snapshot(m3u8_preview_t *out,const m3u8_media_t *m,long long next_seq){
    if(!out)return;out->duration=0;out->text[0]=0;
    int end=-1;for(int i=0;i<m->count;i++)if(m->seg[i].seq<next_seq)end=i;
    if(end<0||m->seg[end].is_ad)return;
    int start=end;double duration=m->seg[end].duration;
    while(start>0&&duration<60){
        const m3u8_segment_t *prev=&m->seg[start-1],*cur=&m->seg[start];
        if(prev->is_ad||cur->discontinuity||prev->seq+1!=cur->seq||duration+prev->duration>65)break;
        duration+=prev->duration;start--;
    }
    if(duration<5)return;
    size_t used=0;int n=snprintf(out->text,sizeof(out->text),"#EXTM3U\n#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:%lld\n",(int)m->target_duration+1,m->seg[start].seq);
    if(n<0||(size_t)n>=sizeof(out->text))return;used=n;
    int prev_map=-2;
    for(int i=start;i<=end;i++){
        const m3u8_segment_t *s=&m->seg[i];if(strncmp(s->url,"https://",8)||strpbrk(s->url,"\r\n"))goto fail;
        if(s->map!=prev_map&&s->map>=0){
            if(s->map>=m->nmap||strncmp(m->map_url[s->map],"https://",8)||strpbrk(m->map_url[s->map],"\r\n\""))goto fail;
            n=snprintf(out->text+used,sizeof(out->text)-used,"#EXT-X-MAP:URI=\"%s\"\n",m->map_url[s->map]);if(n<0||(size_t)n>=sizeof(out->text)-used)goto fail;used+=n;
        }prev_map=s->map;
        n=snprintf(out->text+used,sizeof(out->text)-used,"#EXTINF:%.3f,\n%s\n",(double)s->duration,s->url);if(n<0||(size_t)n>=sizeof(out->text)-used)goto fail;used+=n;
    }
    n=snprintf(out->text+used,sizeof(out->text)-used,"#EXT-X-ENDLIST\n");if(n<0||(size_t)n>=sizeof(out->text)-used)goto fail;
    out->duration=(int)duration;return;
fail:out->duration=0;out->text[0]=0;
}
#define CLIP_WINDOW (512u*1024u)
typedef struct {const char *url;volatile int *abort;unsigned long long total;buf_t window[2];uint64_t base[2];int next;long status;} clip_reader_t;
static void clip_reader_free(clip_reader_t *r){buf_free(&r->window[0]);buf_free(&r->window[1]);}
static const uint8_t *clip_read(void *ctx,uint64_t off,size_t n){
    clip_reader_t *r=ctx;if(!n||n>CLIP_WINDOW||(r->total&&(off>r->total||n>r->total-off)))return NULL;
    for(int i=0;i<2;i++)if(r->window[i].size&&off>=r->base[i]&&off-r->base[i]<=r->window[i].size&&n<=r->window[i].size-(size_t)(off-r->base[i]))return r->window[i].data+(size_t)(off-r->base[i]);
    int i=r->next;r->next^=1;r->base[i]=off;
    if(!r->window[i].data){r->window[i].data=malloc(CLIP_WINDOW+1u);if(!r->window[i].data)return NULL;r->window[i].cap=CLIP_WINDOW+1u;}
    if(http_native_range(r->url,off,CLIP_WINDOW,&r->total,&r->window[i],&r->status,r->abort)||r->window[i].size<n){buf_reset(&r->window[i]);return NULL;}
    return r->window[i].data;
}
static uint32_t clip_u32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int clip_moov(clip_reader_t *r,buf_t *meta){
    uint64_t pos=0;
    for(int boxes=0;boxes<128;boxes++){
        const uint8_t *p=clip_read(r,pos,8);if(!p)return -1;
        uint64_t size=clip_u32(p);int moov=!memcmp(p+4,"moov",4);unsigned hdr=8;
        if(size==1){p=clip_read(r,pos,16);if(!p)return -1;size=(uint64_t)clip_u32(p+8)<<32|clip_u32(p+12);hdr=16;}
        if(!size)size=r->total-pos;
        if(size<hdr||pos>r->total||size>r->total-pos)return -1;
        if(moov){
            if(size>2u*1024u*1024u)return -1;
            if(buf_reserve(meta,(size_t)size+1)<0)return -1;
            for(uint64_t i=0;i<size;){size_t n=(size-i>CLIP_WINDOW)?CLIP_WINDOW:(size_t)(size-i);p=clip_read(r,pos+i,n);if(!p||buf_append(meta,p,n)<0)return -1;i+=n;}
            return 0;
        }
        pos+=size;if(pos>=r->total)return -1;
    }
    return -1;
}
int hls_clip_probe(const char *url,int *w,int *h,float *fps,volatile int *abort){
    clip_reader_t r={.url=url,.abort=abort};buf_t meta={0};int rc=clip_moov(&r,&meta);
    if(!rc)rc=mp4_clip_info(meta.data,meta.size,w,h,fps);
    clip_reader_free(&r);buf_free(&meta);return rc;
}
static void *hls_thread(void *argp)
{
    hls_t *h = argp;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1);
    CURL *c = http_new();
    buf_t pl = {0}, seg = {0};
    static m3u8_media_t media; /* large; only one downloader runs at a time */
    long long next_seq = -1;
    int vod_index=-1,vod_loaded=0,media_valid=0,vod_sequence_known=0;
    long long vod_sequence=0;
    int fails = 0, segment_failures = 0, ad_probed = 0;
    unsigned long long last_new = sceKernelGetProcessTimeWide();
    long status = 0;

    int trust=c&&h->recorded?(int)vt_tls_configure(c):CURLE_OK;
    if(c&&h->recorded)curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
    if (!c) {
        snprintf(h->error, sizeof(h->error), vt_msg("No se pudo iniciar curl"));
        h->finished = 1;
        return NULL;
    }

    if(h->recorded==2){
        clip_reader_t reader={.url=h->url,.abort=&h->abort_req};buf_t meta={0};
        int probe=clip_moov(&reader,&meta);
        if(!probe){
            h->actual_start=0;h->position_ready=1;
            LOGI("clip: bounded range playback file=%u metadata=%u",(unsigned)reader.total,(unsigned)meta.size);
            int result=mp4_demux_range_seek(&h->mp4,meta.data,meta.size,(size_t)reader.total,clip_read,&reader,h->start_seconds,&h->actual_start);
            clip_reader_free(&reader);buf_free(&meta);
            if(result<0&&h->running)snprintf(h->error,sizeof(h->error),"Clip range playback failed");
            else if(!result)h->segments_done=1;
            goto done;
        }
        clip_reader_free(&reader);buf_free(&meta);
        if(h->abort_req)goto done;
        LOGI("clip: range unavailable, bounded full-file fallback");
        int rc=http_media_get(c,trust,h->url,&seg,&status,&h->abort_req);
        if(rc||status!=200){snprintf(h->error,sizeof(h->error),"Clip MP4 HTTP %ld curl %d",status,-rc);goto done;}
        h->actual_start=0;h->position_ready=1;
        if(mp4_demux_file_seek(&h->mp4,seg.data,seg.size,h->start_seconds,&h->actual_start)<0){if(h->running)snprintf(h->error,sizeof(h->error),"Unsupported or incomplete clip MP4");}
        else h->segments_done=1;
        goto done;
    }
    while (h->running) {
        int bad=0,net=0,parsed=0;
        media_valid=0;
        if(!h->preview_playlist&&(!h->recorded||!vod_loaded)){
            net=h->recorded?http_media_get(c,trust,h->url,&pl,&status,&h->abort_req):http_request(c,h->url,NULL,NULL,&pl,&status,&h->abort_req);
            bad=net<0||status!=200;
        }
        const char *playlist=h->preview_playlist?h->preview_playlist:(const char*)pl.data;
        if(!bad){
            parsed=h->recorded?m3u8_parse_vod(playlist,h->url,&media,vod_index,h->start_seconds):m3u8_parse_media(playlist,h->url,&media);
            bad=parsed<0;
            if(h->recorded){
                LOGI("vod: playlist HTTP %ld parse=%d segments=%d total=%d end=%d bytes=%u",status,parsed,media.count,media.total_segments,media.ended,(unsigned)pl.size);
                if(vod_sequence_known&&parsed>=0&&media.media_sequence!=vod_sequence){
                    snprintf(h->error,sizeof(h->error),"VOD playlist window changed; reopen recording");break;
                }
                if(parsed>=0&&media.total_segments>0){vod_sequence=media.media_sequence;vod_sequence_known=1;}
                if(parsed==VOD_PARSE_WAIT){
                    if(sceKernelGetProcessTimeWide()-last_new>45000000ULL){snprintf(h->error,sizeof(h->error),"VOD waiting for new segments; try again later");break;}
                    vod_loaded=0;sleep_ms_while_running(h,2000);continue;
                }
                if(parsed<0){
                    const char *why=parsed==VOD_PARSE_RANGE?"byte-range segments unsupported":parsed==VOD_PARSE_KEY?"encrypted playlist unsupported":parsed==VOD_PARSE_MAP?"invalid or unsupported init segment":parsed==VOD_PARSE_EMPTY?"empty recording":parsed==VOD_PARSE_SEEK?"position outside recording":"invalid HLS playlist";
                    snprintf(h->error,sizeof(h->error),"VOD: %s (HTTP %ld)",why,status);
                    LOGW("vod: %s",h->error);break;
                }
            }
        }
        media_valid=!bad;
        if (bad) {
            if (!h->running)
                break;
            LOGW("hls: fallo al leer la playlist (HTTP %ld, intento %d)", status, fails + 1);
            if (++fails >= 4 || status == 404 || status == 403 || status == 401) {
                h->retryable = 1;
                if(h->recorded&&status==403){
                    const char *reason=twitch_vod_rejection((const char*)pl.data);
                    snprintf(h->error,sizeof(h->error),"%s",twitch_vod_access_message(reason));
                    LOGW("vod: media playlist HTTP 403 class=%s curl=%d",reason,-net);
                }else if(h->recorded)snprintf(h->error,sizeof(h->error),"VOD playlist download HTTP %ld curl %d",status,-net);
                else snprintf(h->error, sizeof(h->error), vt_msg("Stream terminado (HTTP %ld)"), status);
                LOGE("hls: %s", h->error);
                break;
            }
            sleep_ms_while_running(h, (fails > 4 ? 4 : fails) * 1000);
            continue;
        }
        fails = 0;
        if(h->recorded){vod_loaded=1;if(!h->position_ready){h->actual_start=(int)media.first_seconds;h->position_ready=1;}h->duration_seconds=(int)media.total_seconds;}

        if (next_seq < 0) { /* join near the live edge */
            int start = !h->recorded&&media.count > h->live_edge ? media.count - h->live_edge : 0;
            next_seq = media.count ? media.seg[start].seq : 0;
            LOGI("hls: playlist con %d segmentos de %.1f s, empiezo en #%lld", media.count,
                 (double)media.target_duration, next_seq);
        }
        if (media.count && media.seg[0].seq > next_seq) {
            /* fell behind the playlist window: jump forward */
            LOGW("hls: descarga demasiado lenta, salto %lld segmentos", media.seg[0].seq - next_seq);
            next_seq = media.seg[0].seq;
            demux_discontinuity(h);
        }

        int got_new = 0;
        for (int i = 0; i < media.count && h->running; i++) {
            m3u8_segment_t *s = &media.seg[i];
            if (s->seq < next_seq)
                continue;
            next_seq = s->seq + 1;
            got_new = 1;
            last_new = sceKernelGetProcessTimeWide();

            if (!h->recorded&&s->is_ad) {
                if (!h->in_ad) {
                    LOGI("hls: SSAI break detected; filtering ad segments");
                    demux_discontinuity(h);
                }
                h->in_ad = 1;
                /* Only probe if the live edge is still advertising. If clean
                 * content is already in this playlist, just resume it. */
                if(!ad_probed && h->backup_url && h->channel &&
                   media.seg[media.count-1].is_ad){
                    ad_probed=1;
                    LOGI("hls: checking direct alternate session");
                    if(twitch_find_clean_variant(h->channel,h->oauth,h->url,
                        h->width,h->height,h->fps,h->backup_url,&h->abort_req))
                        goto done;
                }
                continue;
            }
            if (h->in_ad) {
                LOGI("hls: live content resumed");
                demux_discontinuity(h);
                ad_probed=0;
            }
            h->in_ad = 0;
            if (s->discontinuity)
                demux_discontinuity(h);

            /* fMP4 stream: (re)load the init segment when it changes */
            if (s->map >= 0 && s->map < media.nmap && strcmp(h->map_loaded, media.map_url[s->map])) {
                int iok = 0;
                for (int attempt = 0; attempt < 2 && h->running && !iok; attempt++)
                    iok = (h->recorded?http_media_get(c,trust,media.map_url[s->map],&seg,&status,&h->abort_req):http_request(c, media.map_url[s->map], NULL, NULL, &seg, &status, &h->abort_req)) == 0 &&
                          status == 200;
                if (iok && mp4_demux_set_init(&h->mp4, seg.data, seg.size) == 0) {
                    if (!h->map_loaded[0])
                        LOGI("hls: formato fMP4 (CMAF), %d pistas, H.264 %s, AAC %s", h->mp4.ntrk,
                             h->mp4.ps.size ? "si" : "no", h->mp4.aac_object ? "si" : "no");
                    snprintf(h->map_loaded, sizeof(h->map_loaded), "%s", media.map_url[s->map]);
                } else if (h->running) {
                    LOGE("hls: no se pudo leer la cabecera fMP4 (HTTP %ld, %u bytes)", status, (unsigned)seg.size);
                    if(h->recorded){snprintf(h->error,sizeof(h->error),"Recording init segment unavailable");goto done;}
                    continue;
                }
            }

            int ok = 0;
            unsigned long long t0 = sceKernelGetProcessTimeWide();
            for (int attempt = 0; attempt < 2 && h->running && !ok; attempt++) {
                if ((h->recorded?http_media_get(c,trust,s->url,&seg,&status,&h->abort_req):http_request(c, s->url, NULL, NULL, &seg, &status, &h->abort_req)) == 0 &&
                    status == 200)
                    ok = 1;
            }
            if (!ok) {
                if (h->running)
                    LOGW("hls: segmento #%lld perdido (HTTP %ld)", s->seq, status);
                demux_discontinuity(h);
                h->segment_losses++;
                if(h->recorded){snprintf(h->error,sizeof(h->error),"Recording segment unavailable");goto done;}
                if (++segment_failures >= 4) { h->retryable=1; h->running=0; }
                continue;
            }
            segment_failures=0;
            unsigned long long dt=sceKernelGetProcessTimeWide()-t0;
            if(dt>0){
                unsigned kbps=(unsigned)(((unsigned long long)seg.size*8000ULL)/dt);
                h->download_kbps=h->download_kbps?(h->download_kbps*3+kbps)/4:kbps;
            }
            if (mp4_looks_like(seg.data, seg.size)) {
                int r = mp4_demux_feed(&h->mp4, seg.data, seg.size);
                if (r == -2) {
                    if (!h->segments_done)
                        LOGE("hls: segmento fMP4 sin cabecera (EXT-X-MAP)");
                    if(h->recorded){snprintf(h->error,sizeof(h->error),"Unsupported recording media");goto done;}
                    continue;
                }
                if (r < 0)
                    break; /* aborted */
            } else if (ts_demux_feed(&h->demux, seg.data, seg.size) < 0 ||
                       ts_demux_flush(&h->demux) < 0) {
                break; /* aborted */
            }
            h->segments_done++;
        }

        if(h->recorded){
            vod_index=media.first_index+media.count;
            if(vod_index>=media.total_segments){if(media.ended||h->preview_playlist)break;vod_loaded=0;sleep_ms_while_running(h,2000);}
            continue;
        }
        if (media.ended) {
            snprintf(h->error, sizeof(h->error), vt_msg("El directo ha terminado"));
            LOGI("hls: la playlist indica fin del directo");
            break;
        }
        if (!got_new) {
            if (sceKernelGetProcessTimeWide()-last_new>(h->in_ad?180000000ULL:45000000ULL)) { h->retryable=1; break; }
            int delay=(int)(media.target_duration*500);
            if(delay<250)delay=250;
            sleep_ms_while_running(h,delay);
        }
    }

done:
    if(!h->recorded&&h->capture&&media_valid)live_snapshot(h->capture,&media,next_seq);
    buf_free(&pl);
    buf_free(&seg);
    http_free(c);
    h->finished = 1;
    return NULL;
}

static int hls_start_mode(hls_t *h, const char *media_url, pktqueue_t *vq, pktqueue_t *aq, int live_edge,
              const char *channel,const char *oauth,int width,int height,float fps,char *backup_url,int recorded,int start_seconds,const char *preview,m3u8_preview_t *capture)
{
    memset(h, 0, sizeof(*h));
    h->recorded=recorded;h->start_seconds=start_seconds;if(recorded==2)h->duration_seconds=width;h->preview_playlist=preview;h->capture=capture;
    if(capture){capture->duration=0;capture->text[0]=0;}
    h->channel=channel;h->oauth=oauth;h->width=width;h->height=height;h->fps=fps;
    h->backup_url=backup_url;
    if(backup_url)backup_url[0]=0;
    snprintf(h->url, sizeof(h->url), "%s", media_url);
    h->live_edge=live_edge<1?1:live_edge>6?6:live_edge;
    h->vq = vq;
    h->aq = aq;
    h->running = 1;
    ts_demux_init(&h->demux, on_es, h);
    mp4_demux_init(&h->mp4, on_es, h);

    /* curl + OpenSSL need a generous stack */
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstacksize(&attr, 512 * 1024);
        if (rc == 0) rc = pthread_create(&h->thread, &attr, hls_thread, h);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        h->running = 0;
        ts_demux_free(&h->demux);
        mp4_demux_free(&h->mp4);
        return -1;
    }
    h->thread_started = 1;
    return 0;
}

int hls_start(hls_t *h,const char *url,pktqueue_t *vq,pktqueue_t *aq,int edge,const char *channel,const char *oauth,int width,int height,float fps,char *backup){return hls_start_mode(h,url,vq,aq,edge,channel,oauth,width,height,fps,backup,0,0,NULL,NULL);}
int hls_start_recorded(hls_t *h,const char *url,pktqueue_t *vq,pktqueue_t *aq,int start){return hls_start_mode(h,url,vq,aq,1,NULL,NULL,0,0,0,NULL,1,start,NULL,NULL);}

int hls_start_capture(hls_t *h,const char *url,pktqueue_t *vq,pktqueue_t *aq,int edge,const char *channel,const char *oauth,int width,int height,float fps,char *backup,m3u8_preview_t *capture){return hls_start_mode(h,url,vq,aq,edge,channel,oauth,width,height,fps,backup,0,0,NULL,capture);}
int hls_start_preview(hls_t *h,const char *playlist,pktqueue_t *vq,pktqueue_t *aq,int start){return hls_start_mode(h,"https://usher.ttvnw.net/",vq,aq,1,NULL,NULL,0,0,0,NULL,1,start,playlist,NULL);}
int hls_start_clip(hls_t *h,const char *url,pktqueue_t *vq,pktqueue_t *aq,int duration,int start){return hls_start_mode(h,url,vq,aq,1,NULL,NULL,duration,0,0,NULL,2,start,NULL,NULL);}
void hls_stop(hls_t *h)
{
    h->running = 0;
    h->abort_req = 1;
    pq_abort(h->vq);
    pq_abort(h->aq);
    if (h->thread_started) {
        pthread_join(h->thread, NULL);
        h->thread_started = 0;
    }
    ts_demux_free(&h->demux);
    mp4_demux_free(&h->mp4);
}

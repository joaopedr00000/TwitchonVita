#include "images.h"
#include "buf.h"
#include "http.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>

#define MAX_IMAGES 192
#define HOME_IMAGES 64 /* independent home and playback cache quotas */
#define MAX_URL 192
#define DECODES_PER_FRAME 1
#define EVICT_MS 3000u

enum { IMG_FREE = 0, IMG_QUEUED, IMG_LOADING, IMG_READY, IMG_TEXTURE, IMG_FAILED };

#define WORKERS 3        /* parallel downloads: one HTTPS request at a time is too slow */

typedef struct {
    int state;
    char url[MAX_URL];
    unsigned stamp;      /* last frame it was requested */
    int prio;            /* 1 = on screen, 0 = prefetch (just off screen) */
    buf_t data;
    vita2d_texture *tex;
} image_t;

static image_t g_img[MAX_IMAGES];
static vita2d_texture *g_trash[MAX_IMAGES * 2];
static int g_ntrash;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_frame = 1;
static int g_defer_free;   /* player: textures freed by img_free_trash() after its GPU wait */
static int g_stat_decoded, g_stat_evicted, g_stat_failed, g_stat_freed;

void img_set_deferred_free(int on) { g_defer_free = on; }
static volatile int g_running;
static volatile int g_stop;      /* abort flag for curl: non-zero cancels */
static volatile int g_workers_alive;
static unsigned g_version; /* bumped when a texture becomes available (main thread) */

unsigned img_version(void) { return g_version; }

static int worker(SceSize args, void *argp)
{
    CURL *c = http_new();
    buf_t tmp = {0};
    (void)args; (void)argp;

    while (g_running) {
        int pick = -1;
        unsigned long long best = 0;
        char url[MAX_URL];

        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_IMAGES; i++) {
            /* on-screen first, then the newest request */
            unsigned long long key = ((unsigned long long)(g_img[i].prio ? 1 : 0) << 32) | g_img[i].stamp;
            if (g_img[i].state == IMG_QUEUED && key >= best) {
                best = key;
                pick = i;
            }
        }
        if (pick >= 0) {
            g_img[pick].state = IMG_LOADING;
            memcpy(url, g_img[pick].url, sizeof(url));
        }
        pthread_mutex_unlock(&g_lock);

        if (pick < 0 || !c) {
            sceKernelDelayThread(30000);
            continue;
        }

        long status = 0;
        int ok = http_request(c, url, NULL, NULL, &tmp, &status, &g_stop) == 0 &&
                 status == 200 && tmp.size > 8;

        pthread_mutex_lock(&g_lock);
        image_t *im = &g_img[pick];
        /* the slot may have been cleared/reused meanwhile */
        if (im->state == IMG_LOADING && !strcmp(im->url, url)) {
            if (ok) {
                buf_reset(&im->data);
                if (buf_append(&im->data, tmp.data, tmp.size) == 0)
                    im->state = IMG_READY;
                else
                    im->state = IMG_FAILED;
            } else {
                im->state = IMG_FAILED;
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    buf_free(&tmp);
    http_free(c);
    pthread_mutex_lock(&g_lock);
    g_workers_alive--;
    pthread_mutex_unlock(&g_lock);
    return sceKernelExitDeleteThread(0);
}

void img_init(void)
{
    memset(g_img, 0, sizeof(g_img));
    g_running = 1;
    g_stop = 0;
    g_workers_alive = 0;
    for (int i = 0; i < WORKERS; i++) {
        SceUID th = sceKernelCreateThread("vt_img", worker, 0x10000100 + 10, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
        if (th < 0) {
            LOGE("imagenes: no se pudo crear el hilo %d (0x%08X)", i, (unsigned)th);
            break;
        }
        pthread_mutex_lock(&g_lock);
        g_workers_alive++;
        pthread_mutex_unlock(&g_lock);
        if (sceKernelStartThread(th, 0, NULL) < 0) {
            pthread_mutex_lock(&g_lock);
            g_workers_alive--;
            pthread_mutex_unlock(&g_lock);
        }
    }
}

/* lock held */
static void release_slot(image_t *im)
{
    if (im->tex && g_ntrash < (int)(sizeof(g_trash) / sizeof(g_trash[0])))
        g_trash[g_ntrash++] = im->tex;
    im->tex = NULL;
    buf_free(&im->data);
    im->state = IMG_FREE;
    im->url[0] = 0;
}

/* vita2d only accepts JPEGs that start with a JFIF (FF E0) or Exif (FF E1)
 * segment, but Twitch's thumbnails and avatars start with the quantisation
 * table (FF D8 FF DB). A standard JFIF APP0 segment is inserted after SOI. */
static const uint8_t JFIF_APP0[18] = {
    0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
};

int img_fix_jpeg(const uint8_t *in, size_t n, uint8_t **out, size_t *out_n)
{
    *out = NULL;
    if (n < 4 || in[0] != 0xFF || in[1] != 0xD8 || in[2] != 0xFF)
        return -1;
    if (in[3] == 0xE0 || in[3] == 0xE1)
        return 0; /* already accepted as is */
    uint8_t *b = malloc(n + sizeof(JFIF_APP0));
    if (!b)
        return -1;
    memcpy(b, in, 2);
    memcpy(b + 2, JFIF_APP0, sizeof(JFIF_APP0));
    memcpy(b + 2 + sizeof(JFIF_APP0), in + 2, n - 2);
    *out = b;
    *out_n = n + sizeof(JFIF_APP0);
    return 1;
}

static vita2d_texture *decode(const buf_t *b)
{
    const uint8_t *d = b->data;
    if (b->size > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return vita2d_load_PNG_buffer(d);
    if (b->size > 4 && d[0] == 0xFF && d[1] == 0xD8) {
        uint8_t *fixed;
        size_t fn;
        int r = img_fix_jpeg(d, b->size, &fixed, &fn);
        if (r < 0)
            return NULL;
        if (r == 0)
            return vita2d_load_JPEG_buffer(d, (unsigned long)b->size);
        vita2d_texture *t = vita2d_load_JPEG_buffer(fixed, (unsigned long)fn);
        free(fixed);
        return t;
    }
    return NULL;
}

static unsigned now_ms(void)
{
    return (unsigned)(sceKernelGetProcessTimeWide() / 1000ULL);
}

void img_free_trash(void)
{
    /* caller guarantees the GPU is idle (it just did vita2d_wait_rendering_done) */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ntrash; i++)
        vita2d_free_texture(g_trash[i]);
    g_stat_freed += g_ntrash;
    g_ntrash = 0;
    pthread_mutex_unlock(&g_lock);
}

void img_frame(void)
{
    int budget = DECODES_PER_FRAME;

    g_frame++;
    pthread_mutex_lock(&g_lock);
    /* evicted textures are normally freed by img_free_trash() after the frame's
     * own GPU wait; only wait here when many are pending */
    if (g_ntrash > 0 && (!g_defer_free || g_ntrash > MAX_IMAGES / 2)) {
        vita2d_wait_rendering_done();
        for (int i = 0; i < g_ntrash; i++)
            vita2d_free_texture(g_trash[i]);
        g_stat_freed += g_ntrash;
        g_ntrash = 0;
    }
    for (int i = 0; i < MAX_IMAGES && budget > 0; i++) {
        image_t *im = &g_img[i];
        if (im->state != IMG_READY)
            continue;
        im->tex = decode(&im->data);
        buf_free(&im->data);
        im->state = im->tex ? IMG_TEXTURE : IMG_FAILED;
        g_version++;
        g_stat_decoded++;
        if (!im->tex)
            g_stat_failed++; /* counted, not logged: logging here blocked the video thread */
        else
            vita2d_texture_set_filters(im->tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        budget--;
    }
    pthread_mutex_unlock(&g_lock);
}

void img_stats(int *decoded, int *evicted, int *failed, int *freed)
{
    pthread_mutex_lock(&g_lock);
    *decoded = g_stat_decoded;
    *evicted = g_stat_evicted;
    *failed = g_stat_failed;
    *freed = g_stat_freed;
    g_stat_decoded = g_stat_evicted = g_stat_failed = g_stat_freed = 0;
    pthread_mutex_unlock(&g_lock);
}

static vita2d_texture *request(const char *url, int prio);

vita2d_texture *img_get(const char *url)
{
    return request(url, 1);
}

void img_prefetch(const char *url)
{
    request(url, 0);
}

static vita2d_texture *request(const char *url, int prio)
{
    vita2d_texture *t = NULL;
    int free_slot = -1, lru = -1;
    unsigned lru_stamp = 0xFFFFFFFFu;
    unsigned now = now_ms();

    if (!url || !url[0] || strlen(url) >= MAX_URL)
        return NULL;

    pthread_mutex_lock(&g_lock);
    int begin=g_defer_free?HOME_IMAGES:0;
    int end=g_defer_free?MAX_IMAGES:HOME_IMAGES;
    for (int i = begin; i < end; i++) {
        image_t *im = &g_img[i];
        if (im->state == IMG_FREE) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (!strcmp(im->url, url)) {
            im->stamp = now;
            im->prio = prio;
            t = im->state == IMG_TEXTURE ? im->tex : NULL;
            pthread_mutex_unlock(&g_lock);
            return t;
        }
        /* eviction candidate: unused for EVICT_MS (time, not frames: the chat
         * panel requests its emotes only when it re-renders, a few times per
         * second, so frame-based ageing evicted everything on screen) */
        if (im->state != IMG_LOADING && now - im->stamp > EVICT_MS && im->stamp < lru_stamp) {
            lru_stamp = im->stamp;
            lru = i;
        }
    }
    int slot = free_slot >= 0 ? free_slot : lru;
    if (slot >= 0) {
        image_t *im = &g_img[slot];
        if (im->state != IMG_FREE) {
            release_slot(im);
            g_stat_evicted++;
        }
        snprintf(im->url, sizeof(im->url), "%s", url);
        im->stamp = now;
        im->prio = prio;
        im->state = IMG_QUEUED;
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void img_clear(void)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_IMAGES; i++)
        if (g_img[i].state != IMG_FREE)
            release_slot(&g_img[i]);
    pthread_mutex_unlock(&g_lock);
}

void img_shutdown(void)
{
    g_running = 0;
    g_stop = 1;
    for (int i = 0; i < 500 && g_workers_alive > 0; i++)
        sceKernelDelayThread(10000);
    img_clear();
    img_frame();
}

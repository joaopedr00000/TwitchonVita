#ifndef VT_HLS_H
#define VT_HLS_H

#include <psp2/types.h>
#include "m3u8.h"
#include "pktqueue.h"
#include "ts_demux.h"
#include "mp4_demux.h"

typedef struct {
    char url[M3U8_URL_MAX];   /* media playlist */
    pktqueue_t *vq, *aq;      /* outputs: H.264 AUs, single ADTS frames */

    volatile int running;
    volatile int abort_req;   /* non-zero cancels in-flight HTTP requests */
    volatile int in_ad;       /* currently skipping Twitch ad segments */
    volatile int finished;    /* stream ended or failed */
    volatile int segments_done;
    volatile int retryable;
    volatile unsigned download_kbps;
    volatile unsigned segment_losses;
    int live_edge;
    const char *channel,*oauth;
    int width,height;
    float fps;
    char *backup_url; /* caller-owned M3U8_URL_MAX buffer, NULL disables probing */
    char error[128];

    pthread_t thread;
    int thread_started;
    ts_demux_t demux;
    mp4_demux_t mp4;                  /* fMP4 streams (#EXT-X-MAP), e.g. Enhanced Broadcasting */
    char map_loaded[M3U8_URL_MAX];    /* init segment currently in mp4 */
} hls_t;

int hls_start(hls_t *h, const char *media_url, pktqueue_t *vq, pktqueue_t *aq, int live_edge,
              const char *channel,const char *oauth,int width,int height,float fps,char *backup_url);
void hls_stop(hls_t *h);

#endif

#include "images.h"
#include "image_decode.h"
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

/* All GXM allocations, uploads and frees happen on the render thread before
 * start_drawing, after a GPU fence. Workers own compressed bytes only. */
#define HOME_IMAGES 48
#define MAX_IMAGES 112
#define MAX_URL 192
#define WORKERS 2
#define MAX_READY 8
#define HOME_BUDGET (12u*1024u*1024u)
#define PLAYER_BUDGET (8u*1024u*1024u)
#define HOME_TTL 4000u
#define PLAYER_TTL 20000u
#define ALLOC_GRANULE (256u*1024u)
enum { IMG_FREE, IMG_QUEUED, IMG_LOADING, IMG_READY, IMG_TEXTURE, IMG_FAILED };
typedef struct {
    int state,prio;
    char url[MAX_URL];
    unsigned stamp,serial,failed_at;
    buf_t data;
    vita2d_texture *tex;
    size_t bytes;
} image_t;
static image_t g_img[MAX_IMAGES];
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_threads[WORKERS];
static int g_nthreads,g_player,g_clear_pending;
static volatile int g_running,g_stop;
static unsigned g_serial,g_version,g_last_log;
static size_t g_bytes,g_peak;
static int g_stat_decoded,g_stat_evicted,g_stat_failed,g_stat_freed,g_reused,g_rejected,g_alloc_failed;

static unsigned now_ms(void){return (unsigned)(sceKernelGetProcessTimeWide()/1000ULL);}
unsigned img_version(void){return g_version;}
/* These compatibility hooks remain for the player. No frees during a draw. */
void img_set_deferred_free(int on){pthread_mutex_lock(&g_lock);g_player=!!on;pthread_mutex_unlock(&g_lock);}
void img_free_trash(void){}
static size_t bounded_write(char *p,size_t size,size_t nmemb,void *arg){
    buf_t *b=arg;if(nmemb&&size>SIZE_MAX/nmemb)return 0;
    size_t n=size*nmemb;
    if(b->size>VT_IMAGE_INPUT_MAX||n>VT_IMAGE_INPUT_MAX-b->size)return 0;
    return buf_append(b,p,n)<0?0:n;
}
static void *worker(void *unused){
    (void)unused;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),SCE_KERNEL_CPU_MASK_USER_2);
    CURL *c=http_new();if(!c)return NULL;
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,bounded_write);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,10L);
    while(g_running){
        char url[MAX_URL];int pick=-1,best_prio=-1,ready=0;unsigned serial=0,best_age=0,now=now_ms();
        pthread_mutex_lock(&g_lock);
        for(int i=0;i<MAX_IMAGES;i++)if(g_img[i].state==IMG_READY)ready++;
        int begin=g_player?HOME_IMAGES:0,end=g_player?MAX_IMAGES:HOME_IMAGES;
        if(ready<MAX_READY&&!g_clear_pending)for(int i=begin;i<end;i++){
            image_t *im=&g_img[i];unsigned age=now-im->stamp;
            if(im->state!=IMG_QUEUED||age>(g_player?5000u:800u))continue;
            int prio=im->prio&&age<300u?2:im->prio?1:0;
            if(pick<0||prio>best_prio||(prio==best_prio&&age<best_age)){pick=i;best_prio=prio;best_age=age;}
        }
        if(pick>=0){image_t *im=&g_img[pick];im->state=IMG_LOADING;serial=im->serial;memcpy(url,im->url,sizeof(url));}
        pthread_mutex_unlock(&g_lock);
        if(pick<0){sceKernelDelayThread(30000);continue;}
        buf_t tmp={0};long status=0;
        int ok=http_request(c,url,NULL,NULL,&tmp,&status,&g_stop)==0&&status==200&&tmp.size>=8;
        pthread_mutex_lock(&g_lock);
        image_t *im=&g_img[pick];
        if(im->state==IMG_LOADING&&im->serial==serial&&!g_clear_pending){
            if(ok){im->data=tmp;memset(&tmp,0,sizeof(tmp));im->state=IMG_READY;}
            else{im->state=IMG_FAILED;im->failed_at=now_ms();g_stat_failed++;}
        }
        pthread_mutex_unlock(&g_lock);
        buf_free(&tmp);
    }
    http_free(c);return NULL;
}
void img_init(void){
    memset(g_img,0,sizeof(g_img));g_running=1;g_stop=0;
    pthread_attr_t attr;
    if(pthread_attr_init(&attr)!=0)return;
    if(pthread_attr_setstacksize(&attr,512*1024)==0)
        for(int i=0;i<WORKERS;i++){
            if(pthread_create(&g_threads[g_nthreads],&attr,worker,NULL)!=0){LOGW("images: worker start failed");break;}
            g_nthreads++;
        }
    pthread_attr_destroy(&attr);
}
/* Main thread only, lock held, no GPU operations until fence. */
static void release_slot(image_t *im,int *fenced){
    if(im->tex){
        if(!*fenced){vita2d_wait_rendering_done();*fenced=1;}
        vita2d_free_texture(im->tex);g_bytes-=im->bytes;g_stat_freed++;g_version++;
    }
    buf_free(&im->data);memset(im,0,sizeof(*im));
}
static size_t charge(size_t stride,unsigned h){
    /* Conservative accounting: Vita CDRAM allocations have coarse granularity.
     * Include a granule for alignment/driver overhead in the budget. */
    size_t raw=stride*h;
    return ((raw+ALLOC_GRANULE-1u)/ALLOC_GRANULE)*ALLOC_GRANULE;
}
static int oldest_texture(unsigned now,unsigned age_min,int exclude){
    int best=-1;unsigned age_best=0;
    for(int i=0;i<MAX_IMAGES;i++){
        image_t *im=&g_img[i];unsigned age=now-im->stamp;
        if(i==exclude||im->state!=IMG_TEXTURE||age<age_min)continue;
        if(best<0||age>age_best){best=i;age_best=age;}
    }
    return best;
}
static int reuse_texture(image_t *dst,unsigned w,unsigned h,unsigned now,size_t limit,int *fenced){
    int best=-1;unsigned age_best=0;
    for(int i=0;i<MAX_IMAGES;i++){
        image_t *im=&g_img[i];unsigned age=now-im->stamp;
        if(im==dst||im->state!=IMG_TEXTURE||age<((g_bytes>=limit)?500u:2000u))continue;
        if(vita2d_texture_get_width(im->tex)!=w||vita2d_texture_get_height(im->tex)!=h)continue;
        if(best<0||age>age_best){best=i;age_best=age;}
    }
    if(best<0)return 0;
    if(!*fenced){vita2d_wait_rendering_done();*fenced=1;}
    image_t *old=&g_img[best];dst->tex=old->tex;dst->bytes=old->bytes;
    old->tex=NULL;old->bytes=0;release_slot(old,fenced);
    g_reused++;g_stat_evicted++;return 1;
}
void img_frame(void){
    unsigned now=now_ms();int fenced=0;
    pthread_mutex_lock(&g_lock);
    int begin=g_player?HOME_IMAGES:0,end=g_player?MAX_IMAGES:HOME_IMAGES;
    unsigned ttl=g_player?PLAYER_TTL:HOME_TTL;
    size_t limit=g_player?PLAYER_BUDGET:HOME_BUDGET;
    for(int i=0;i<MAX_IMAGES;i++){
        image_t *im=&g_img[i];unsigned age=now-im->stamp;
        if(im->state==IMG_FREE)continue;
        /* A serial number prevents a cancelled request from writing into a reused slot. */
        if(g_clear_pending||i<begin||i>=end||
           age>(im->state==IMG_TEXTURE||im->state==IMG_FAILED?ttl:g_player?5000u:1200u)){
            release_slot(im,&fenced);g_stat_evicted++;
        }
    }
    g_clear_pending=0;
    while(g_bytes>limit){
        int k=oldest_texture(now,500u,-1);if(k<0)break;
        release_slot(&g_img[k],&fenced);g_stat_evicted++;
    }
    int pick=-1,prio=-1;unsigned age_best=0;
    for(int i=begin;i<end;i++){
        image_t *im=&g_img[i];unsigned age=now-im->stamp;
        if(im->state==IMG_READY&&(pick<0||im->prio>prio||(im->prio==prio&&age<age_best))){
            pick=i;prio=im->prio;age_best=age;
        }
    }
    /* One bounded image per frame; no allocation or decode in a draw call. */
    if(pick>=0){
        image_t *im=&g_img[pick];vt_image_t decoded={0};
        int rc=vt_image_decode(im->data.data,im->data.size,&decoded);
        buf_free(&im->data);
        if(rc==0){
            unsigned w=decoded.w,h=decoded.h;
            size_t need=charge((size_t)((w+63u)&~63u)*4u,h);
            if(g_bytes<=limit)reuse_texture(im,w,h,now,limit,&fenced);
            if(!im->tex){
                while(g_bytes+need>limit){
                    int k=oldest_texture(now,500u,pick);if(k<0)break;
                    release_slot(&g_img[k],&fenced);g_stat_evicted++;
                }
                if(g_bytes+need<=limit){
                    if(!fenced){vita2d_wait_rendering_done();fenced=1;}
                    im->tex=vita2d_create_empty_texture_format(w,h,SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
                    if(im->tex){
                        unsigned stride=vita2d_texture_get_stride(im->tex);size_t actual=charge(stride,h);
                        if(stride<w*4u||g_bytes+actual>limit||!vita2d_texture_get_datap(im->tex)){
                            vita2d_free_texture(im->tex);im->tex=NULL;
                        }else{im->bytes=actual;g_bytes+=actual;if(g_bytes>g_peak)g_peak=g_bytes;}
                    }
                    if(!im->tex)g_alloc_failed++;
                }
            }
            if(im->tex){
                unsigned stride=vita2d_texture_get_stride(im->tex);
                uint8_t *dst=vita2d_texture_get_datap(im->tex);
                for(unsigned y=0;y<h;y++)memcpy(dst+(size_t)y*stride,decoded.rgba+(size_t)y*w*4u,(size_t)w*4u);
                vita2d_texture_set_filters(im->tex,SCE_GXM_TEXTURE_FILTER_LINEAR,SCE_GXM_TEXTURE_FILTER_LINEAR);
                im->state=IMG_TEXTURE;g_stat_decoded++;
            }else{im->state=IMG_FAILED;im->failed_at=now;g_stat_failed++;}
        }else{im->state=IMG_FAILED;im->failed_at=now;g_stat_failed++;g_rejected++;}
        vt_image_free(&decoded);g_version++;
    }
    if(now-g_last_log>=10000u){
        int count=0,ready=0,loading=0;size_t compressed=0;
        for(int i=0;i<MAX_IMAGES;i++){count+=g_img[i].state==IMG_TEXTURE;ready+=g_img[i].state==IMG_READY;loading+=g_img[i].state==IMG_LOADING;compressed+=g_img[i].data.cap;}
        LOGI("image cache: mode=%s textures=%d budget_used=%uKiB limit=%uKiB peak=%uKiB ready=%d compressed=%uKiB loading=%d reuse=%d invalid=%d alloc_fail=%d",
             g_player?"player":"home",count,(unsigned)(g_bytes/1024),(unsigned)(limit/1024),(unsigned)(g_peak/1024),ready,(unsigned)(compressed/1024),loading,g_reused,g_rejected,g_alloc_failed);
        g_last_log=now;
    }
    pthread_mutex_unlock(&g_lock);
}
void img_stats(int *decoded,int *evicted,int *failed,int *freed){
    pthread_mutex_lock(&g_lock);
    *decoded=g_stat_decoded;*evicted=g_stat_evicted;*failed=g_stat_failed;*freed=g_stat_freed;
    g_stat_decoded=g_stat_evicted=g_stat_failed=g_stat_freed=0;
    pthread_mutex_unlock(&g_lock);
}
static vita2d_texture *request(const char *url,int prio){
    if(!url||strncmp(url,"https://",8)||strlen(url)>=MAX_URL)return NULL;
    unsigned now=now_ms();int free_slot=-1;
    pthread_mutex_lock(&g_lock);
    if(g_clear_pending){pthread_mutex_unlock(&g_lock);return NULL;}
    int begin=g_player?HOME_IMAGES:0,end=g_player?MAX_IMAGES:HOME_IMAGES;
    for(int i=begin;i<end;i++){
        image_t *im=&g_img[i];
        if(im->state==IMG_FREE){if(free_slot<0)free_slot=i;continue;}
        if(strcmp(im->url,url))continue;
        if(prio||now-im->stamp>300u){im->stamp=now;im->prio=prio;}
        if(im->state==IMG_FAILED&&prio&&now-im->failed_at>15000u){im->state=IMG_QUEUED;im->serial=++g_serial;}
        vita2d_texture *t=im->state==IMG_TEXTURE?im->tex:NULL;
        pthread_mutex_unlock(&g_lock);return t;
    }
    /* Never evict from request(): a draw may already refer to the old texture. */
    if(free_slot>=0){
        image_t *im=&g_img[free_slot];snprintf(im->url,sizeof(im->url),"%s",url);
        im->stamp=now;im->prio=prio;im->serial=++g_serial;im->state=IMG_QUEUED;
    }
    pthread_mutex_unlock(&g_lock);return NULL;
}
vita2d_texture *img_get(const char *url){return request(url,1);}
void img_prefetch(const char *url){(void)request(url,0);}
void img_clear(void){pthread_mutex_lock(&g_lock);g_clear_pending=1;pthread_mutex_unlock(&g_lock);}
void img_shutdown(void){
    g_running=0;g_stop=1;
    for(int i=0;i<g_nthreads;i++)pthread_join(g_threads[i],NULL);
    g_nthreads=0;img_clear();img_frame();
}

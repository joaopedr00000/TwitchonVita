#include "playback_policy.h"
#include <limits.h>
int vt_profile_edge(int p) { return p==VT_PROFILE_ECO?4:p==VT_PROFILE_LOW_LATENCY?2:3; }
int vt_profile_buffer_ms(int p,int r) {
    int base=p==VT_PROFILE_ECO?1600:p==VT_PROFILE_LOW_LATENCY?600:p==VT_PROFILE_QUALITY?1200:1000;
    if(r<0)r=0;
    if(r>6)r=6;
    int ms=base+r*250;
    return ms>2800?2800:ms;
}
int vt_profile_clock(int p,int h,float fps) {
    return p==VT_PROFILE_QUALITY || h>540 || fps>40 ? 444 : 333;
}
static long score(const m3u8_variant_t *v) {
    if(v->bandwidth>0)return v->bandwidth;
    return (long)v->width*v->height*(long)(v->fps>1?v->fps:30)/8;
}
int vt_profile_pick(m3u8_variant_t *const *list,int n,int p) {
    int target=p==VT_PROFILE_ECO?480:720;
    int wantfps=p==VT_PROFILE_QUALITY?60:30;
    long best=LONG_MAX; int pick=-1;
    for(int i=0;i<n;i++){
        const m3u8_variant_t *v=list[i];
        if(!v||v->audio_only||v->height<=0)continue;
        long delta=v->height-target; if(delta<0)delta=-delta; else delta*=3;
        long fps=(long)(v->fps>1?v->fps:30)-wantfps; if(fps<0)fps=-fps;
        long value=delta*100+fps*20;
        if(value<best){best=value;pick=i;}
    }
    return pick;
}
int vt_profile_lower(m3u8_variant_t *const *list,int n,const m3u8_variant_t *current) {
    if(!current)return -1;
    long cap=score(current),best=-1;int pick=-1;
    for(int i=0;i<n;i++){
        const m3u8_variant_t *v=list[i];
        if(!v||v->audio_only||v->height<=0||v->height>current->height)continue;
        long s=score(v);
        if(s<cap && s>best){best=s;pick=i;}
    }
    return pick;
}

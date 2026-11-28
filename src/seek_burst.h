#ifndef VT_SEEK_BURST_H
#define VT_SEEK_BURST_H
#include <stdint.h>
#define VT_SEEK_WINDOW_US 450000ULL
typedef struct { int active,base,target,tap_side; uint64_t until,tap_until; } vt_seek_burst;
static inline int vt_seek_clamp(int64_t value,int duration){
    if(duration<=0)return 0;
    return value<0?0:value>=duration?duration-1:(int)value;
}
static inline void vt_seek_add(vt_seek_burst *s,int pos,int duration,int delta,uint64_t now){
    if(duration<=0)return;
    if(!s->active){s->base=vt_seek_clamp(pos,duration);s->target=s->base;s->active=1;}
    s->target=vt_seek_clamp((int64_t)s->target+delta,duration);
    s->until=now+VT_SEEK_WINDOW_US;
    s->tap_side=0;
}
static inline int vt_seek_tap(vt_seek_burst *s,int side,int pos,int duration,uint64_t now){
    if(duration<=0||!side)return 0;
    if(s->active||(s->tap_side==side&&now<=s->tap_until)){
        vt_seek_add(s,pos,duration,side*10,now);return 1;
    }
    s->tap_side=side;s->tap_until=now+VT_SEEK_WINDOW_US;return 0;
}
static inline int vt_seek_due(const vt_seek_burst *s,uint64_t now){
    return s->active&&now>=s->until;
}
#endif

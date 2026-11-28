#include "mp4_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
typedef struct {const uint8_t *data;size_t len;unsigned calls;size_t max_read;int v,a;} fixture_t;
static const uint8_t *read_at(void *ctx,uint64_t off,size_t n){
    fixture_t *f=ctx;if(off>f->len||n>f->len-off||n>512*1024)return NULL;
    f->calls++;if(n>f->max_read)f->max_read=n;return f->data+(size_t)off;
}
static int on_sample(void *ctx,int stream,const uint8_t *d,int n,int64_t pts,int64_t dts){
    fixture_t *f=ctx;(void)pts;(void)dts;assert(n>0&&d);
    if(stream==TS_STREAM_VIDEO)f->v++;else f->a++;return 0;
}
static unsigned be32(const uint8_t *p){return (unsigned)p[0]<<24|(unsigned)p[1]<<16|(unsigned)p[2]<<8|p[3];}
int main(int argc,char **argv){
    assert(argc==2);FILE *fp=fopen(argv[1],"rb");assert(fp);
    assert(!fseek(fp,0,SEEK_END));long length=ftell(fp);assert(length>0);rewind(fp);
    uint8_t *p=malloc((size_t)length);assert(p&&fread(p,1,(size_t)length,fp)==(size_t)length);fclose(fp);
    const uint8_t *moov=NULL;size_t ml=0;
    for(size_t off=0;off+8<=(size_t)length;){unsigned n=be32(p+off);assert(n>=8&&n<=(size_t)length-off);if(!memcmp(p+off+4,"moov",4)){moov=p+off;ml=n;break;}off+=n;}
    assert(moov&&ml>8);int w,h;float fps;assert(!mp4_clip_info(moov,ml,&w,&h,&fps));assert(w==320&&h==180&&fps>24.9f&&fps<25.1f);
    fixture_t f={.data=p,.len=(size_t)length};mp4_demux_t d;mp4_demux_init(&d,on_sample,&f);
    assert(!mp4_demux_file(&d,p,(size_t)length));int v=f.v,a=f.a;assert(v>0&&a>0);mp4_demux_free(&d);
    f.v=f.a=0;mp4_demux_init(&d,on_sample,&f);int actual=-1;
    assert(!mp4_demux_range_seek(&d,moov,ml,(size_t)length,read_at,&f,0,&actual));
    assert(actual==0&&f.v==v&&f.a==a&&f.calls>0);mp4_demux_free(&d);
    f.v=f.a=0;mp4_demux_init(&d,on_sample,&f);
    assert(!mp4_demux_range_seek(&d,moov,ml,(size_t)length,read_at,&f,2,&actual));assert(actual==2&&f.v>0&&f.v<v);mp4_demux_free(&d);
    free(p);puts("PASS MP4 metadata, range samples, and keyframe seek");return 0;
}
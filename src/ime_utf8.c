#include "ime_utf8.h"
static size_t emit_utf8(char *dst,size_t cap,size_t n,unsigned cp) {
    unsigned char buf[4];int count;
    if(cp<=0x7f)buf[0]=cp,count=1;
    else if(cp<=0x7ff)buf[0]=0xc0|(cp>>6),buf[1]=0x80|(cp&63),count=2;
    else if(cp<=0xffff)buf[0]=0xe0|(cp>>12),buf[1]=0x80|((cp>>6)&63),buf[2]=0x80|(cp&63),count=3;
    else buf[0]=0xf0|(cp>>18),buf[1]=0x80|((cp>>12)&63),buf[2]=0x80|((cp>>6)&63),buf[3]=0x80|(cp&63),count=4;
    if(n+(size_t)count>=cap)return n;
    for(int k=0;k<count;k++)dst[n++]=(char)buf[k];
    return n;
}
size_t vt_utf16_to_utf8(char *dst,size_t cap,const uint16_t *src,size_t max_units){
    size_t n=0; if(!cap)return 0;dst[0]=0;
    if(!src)return 0;
    for(size_t i=0;i<max_units&&src[i];i++){
        unsigned cp=src[i];
        if(cp>=0xd800&&cp<=0xdbff){
            unsigned lo=i+1<max_units?src[i+1]:0;
            if(lo>=0xdc00&&lo<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);i++;}
            else cp='?';
        }else if(cp>=0xdc00&&cp<=0xdfff)cp='?';
        size_t nn=emit_utf8(dst,cap,n,cp);
        if(nn==n)break;
        n=nn;
    }
    dst[n]=0;return n;
}
size_t vt_utf8_to_utf16(uint16_t *dst,size_t cap,const char *src){
    size_t n=0; if(!cap)return 0;dst[0]=0;if(!src)return 0;
    const unsigned char *p=(const unsigned char*)src;
    while(*p&&n+1<cap){
        unsigned cp;size_t b=1;
        if(p[0]<0x80)cp=p[0];
        else if((p[0]&0xe0)==0xc0&&p[1]&&(p[1]&0xc0)==0x80){cp=((p[0]&31)<<6)|(p[1]&63);b=2;if(cp<0x80)cp='?';}
        else if((p[0]&0xf0)==0xe0&&p[1]&&p[2]&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80){cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63);b=3;if(cp<0x800|| (cp>=0xd800&&cp<=0xdfff))cp='?';}
        else if((p[0]&0xf8)==0xf0&&p[1]&&p[2]&&p[3]&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80&&(p[3]&0xc0)==0x80){cp=((p[0]&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63);b=4;if(cp<0x10000||cp>0x10ffff)cp='?';}
        else cp='?';
        p+=b;
        if(cp<=0xffff)dst[n++]=(uint16_t)cp;
        else{if(n+2>=cap)break;cp-=0x10000;dst[n++]=(uint16_t)(0xd800|(cp>>10));dst[n++]=(uint16_t)(0xdc00|(cp&0x3ff));}
    }
    dst[n]=0;return n;
}

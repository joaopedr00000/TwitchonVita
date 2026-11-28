#include "image_decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <png.h>
#include <jpeglib.h>

static int valid(unsigned w,unsigned h){return w&&h&&w<=VT_IMAGE_EDGE_MAX&&h<=VT_IMAGE_EDGE_MAX;}
void vt_image_free(vt_image_t *im){free(im->rgba);memset(im,0,sizeof(*im));}
static int decode_png(const uint8_t *p,size_t n,vt_image_t *out){
    png_image im;memset(&im,0,sizeof(im));im.version=PNG_IMAGE_VERSION;
    if(!png_image_begin_read_from_memory(&im,p,n)){png_image_free(&im);return -1;}
    if(!valid(im.width,im.height)){png_image_free(&im);return -1;}
    im.format=PNG_FORMAT_RGBA;
    uint8_t *rgba=malloc(PNG_IMAGE_SIZE(im));
    if(!rgba){png_image_free(&im);return -1;}
    if(!png_image_finish_read(&im,NULL,rgba,0,NULL)){free(rgba);png_image_free(&im);return -1;}
    out->w=im.width;out->h=im.height;out->rgba=rgba;
    png_image_free(&im);return 0;
}
typedef struct {struct jpeg_error_mgr pub;jmp_buf jump;} jpeg_err_t;
typedef struct {struct jpeg_decompress_struct c;jpeg_err_t err;uint8_t *rgba;int created;} jpeg_ctx_t;
static void jpeg_fail(j_common_ptr c){jpeg_err_t *e=(jpeg_err_t*)c->err;longjmp(e->jump,1);}
static int decode_jpeg(const uint8_t *p,size_t n,vt_image_t *out){
    jpeg_ctx_t *j=calloc(1,sizeof(*j));if(!j)return -1;
    j->c.err=jpeg_std_error(&j->err.pub);j->err.pub.error_exit=jpeg_fail;
    if(setjmp(j->err.jump))goto fail;
    j->created=1;jpeg_create_decompress(&j->c);
    jpeg_mem_src(&j->c,p,(unsigned long)n);
    if(jpeg_read_header(&j->c,TRUE)!=JPEG_HEADER_OK||!valid(j->c.image_width,j->c.image_height))goto fail;
    j->c.out_color_space=JCS_RGB;
    if(!jpeg_start_decompress(&j->c)||!valid(j->c.output_width,j->c.output_height)||j->c.output_components!=3)goto fail;
    unsigned w=j->c.output_width,h=j->c.output_height;
    j->rgba=malloc((size_t)w*h*4u);if(!j->rgba)goto fail;
    uint8_t row[VT_IMAGE_EDGE_MAX*3u];
    while(j->c.output_scanline<h){
        unsigned y=j->c.output_scanline;JSAMPROW rows[1]={row};
        if(jpeg_read_scanlines(&j->c,rows,1)!=1)goto fail;
        uint8_t *dst=j->rgba+(size_t)y*w*4u;
        for(unsigned x=0;x<w;x++){dst[x*4]=row[x*3];dst[x*4+1]=row[x*3+1];dst[x*4+2]=row[x*3+2];dst[x*4+3]=255;}
    }
    if(!jpeg_finish_decompress(&j->c)||j->err.pub.num_warnings)goto fail;
    jpeg_destroy_decompress(&j->c);out->w=w;out->h=h;out->rgba=j->rgba;free(j);return 0;
fail:
    if(j->created)jpeg_destroy_decompress(&j->c);
    free(j->rgba);free(j);return -1;
}
int vt_image_decode(const uint8_t *p,size_t n,vt_image_t *out){
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!p||n<8||n>VT_IMAGE_INPUT_MAX)return -1;
    if(!png_sig_cmp(p,0,8)){
        /* Reject oversized IHDR before libpng allocates. */
        if(n<33||memcmp(p+12,"IHDR",4))return -1;
        unsigned w=(unsigned)p[16]<<24|(unsigned)p[17]<<16|(unsigned)p[18]<<8|p[19];
        unsigned h=(unsigned)p[20]<<24|(unsigned)p[21]<<16|(unsigned)p[22]<<8|p[23];
        if(!valid(w,h))return -1;
        return decode_png(p,n,out);
    }
    if(p[0]==0xff&&p[1]==0xd8)return decode_jpeg(p,n,out);
    return -1;
}

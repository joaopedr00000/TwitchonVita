#include "image_decode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <jpeglib.h>
int main(void){
    vt_image_t out;png_image im={0};im.version=PNG_IMAGE_VERSION;im.width=2;im.height=2;im.format=PNG_FORMAT_RGBA;
    unsigned char pixels[16]={255,0,0,255,0,255,0,255,0,0,255,128,255,255,255,0};
    png_alloc_size_t n=0;assert(png_image_write_to_memory(&im,NULL,&n,0,pixels,0,NULL));
    unsigned char *p=malloc(n);assert(p&&png_image_write_to_memory(&im,p,&n,0,pixels,0,NULL));
    assert(vt_image_decode(p,n,&out)==0&&out.w==2&&out.h==2&&!memcmp(out.rgba,pixels,16));vt_image_free(&out);
    assert(vt_image_decode(p,8,&out)<0);assert(vt_image_decode(p,n/2,&out)<0);
    p[16]=0x7f;assert(vt_image_decode(p,n,&out)<0);free(p);
    struct jpeg_compress_struct c;struct jpeg_error_mgr e;c.err=jpeg_std_error(&e);jpeg_create_compress(&c);
    unsigned char *jpg=NULL;unsigned long js=0;jpeg_mem_dest(&c,&jpg,&js);
    c.image_width=2;c.image_height=2;c.input_components=3;c.in_color_space=JCS_RGB;
    jpeg_set_defaults(&c);jpeg_start_compress(&c,TRUE);
    unsigned char rgb[6]={255,0,0,0,255,0};JSAMPROW row[1]={rgb};
    while(c.next_scanline<c.image_height)jpeg_write_scanlines(&c,row,1);
    jpeg_finish_compress(&c);jpeg_destroy_compress(&c);
    assert(vt_image_decode(jpg,js,&out)==0&&out.w==2&&out.h==2&&out.rgba[3]==255);vt_image_free(&out);
    assert(vt_image_decode(jpg,js/2,&out)<0);free(jpg);
    assert(vt_image_decode(NULL,0,&out)<0);
    puts("PASS bounded PNG/JPEG decode and rejection of invalid inputs");return 0;
}

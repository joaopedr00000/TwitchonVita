#ifndef VT_IMAGE_DECODE_H
#define VT_IMAGE_DECODE_H
#include <stddef.h>
#include <stdint.h>
#define VT_IMAGE_EDGE_MAX 512u
#define VT_IMAGE_INPUT_MAX (512u*1024u-1u)
typedef struct { unsigned w,h; uint8_t *rgba; } vt_image_t;
/* Bounded CPU decode. Caller owns rgba; no GPU calls. */
int vt_image_decode(const uint8_t *data,size_t size,vt_image_t *out);
void vt_image_free(vt_image_t *im);
#endif

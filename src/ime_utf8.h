#ifndef VT_IME_UTF8_H
#define VT_IME_UTF8_H
#include <stddef.h>
#include <stdint.h>
/* Loss-aware conversion for the PS Vita UTF-16 on-screen keyboard.
 * Returns number of bytes or units, excluding NUL. */
size_t vt_utf16_to_utf8(char *dst,size_t cap,const uint16_t *src,size_t max_units);
size_t vt_utf8_to_utf16(uint16_t *dst,size_t cap,const char *src);
#endif

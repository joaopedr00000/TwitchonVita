#ifndef VT_IME_GUARD_H
#define VT_IME_GUARD_H
#include <psp2/ime_dialog.h>
typedef void (*vt_ime_pump_t)(void *);
int vt_ime_open(SceImeDialogParam *param,vt_ime_pump_t pump,void *ctx);
int vt_ime_close(vt_ime_pump_t pump,void *ctx);
#endif

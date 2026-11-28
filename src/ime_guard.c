#include "ime_guard.h"
#include "log.h"
#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
/* Only the main UI thread opens/closes the OSK. Do not terminate a running
 * dialogue: abort, render until FINISHED, then Term, and wait for NONE.
 * No unbounded retries; do not abort a dialogue owned by another subsystem. */
static int owned;
static void pump_frame(vt_ime_pump_t pump,void *ctx){
    if(pump)pump(ctx);
    sceKernelDelayThread(16000);
}
int vt_ime_close(vt_ime_pump_t pump,void *ctx){
    if(!owned)return 0;
    int aborted=0,last=(int)SCE_COMMON_DIALOG_ERROR_BUSY;
    for(int i=0;i<120;i++){
        SceCommonDialogStatus st=sceImeDialogGetStatus();
        if(st==SCE_COMMON_DIALOG_STATUS_NONE){owned=0;return 0;}
        if(st==SCE_COMMON_DIALOG_STATUS_RUNNING&&!aborted){
            last=sceImeDialogAbort();aborted=last>=0;
            LOGI("ime: abort rc=%08X",(unsigned)last);
        }else if(st==SCE_COMMON_DIALOG_STATUS_FINISHED){
            last=sceImeDialogTerm();
            if(last>=0&&sceImeDialogGetStatus()==SCE_COMMON_DIALOG_STATUS_NONE){owned=0;return 0;}
        }
        pump_frame(pump,ctx);
    }
    LOGW("ime: close pending status=%d rc=%08X",(int)sceImeDialogGetStatus(),(unsigned)last);
    return (int)SCE_COMMON_DIALOG_ERROR_BUSY;
}
int vt_ime_open(SceImeDialogParam *param,vt_ime_pump_t pump,void *ctx){
    if(owned&&vt_ime_close(pump,ctx)<0)return (int)SCE_COMMON_DIALOG_ERROR_BUSY;
    int rc=(int)SCE_COMMON_DIALOG_ERROR_BUSY;
    for(int i=0;i<60;i++){
        rc=sceImeDialogInit(param);
        if(rc>=0){owned=1;return rc;}
        if((unsigned)rc!=(unsigned)SCE_COMMON_DIALOG_ERROR_BUSY&&
           (unsigned)rc!=(unsigned)SCE_COMMON_DIALOG_ERROR_IME_IN_USE)break;
        pump_frame(pump,ctx);
    }
    LOGW("ime: init failed rc=%08X status=%d",(unsigned)rc,(int)sceImeDialogGetStatus());
    return rc;
}

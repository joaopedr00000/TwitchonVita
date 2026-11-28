#include "task.h"
#include <psp2/kernel/threadmgr.h>

static int task_thread(SceSize args, void *argp)
{
    task_t *t = *(task_t **)argp;
    t->result = t->fn(t->arg, &t->abort_flag);
    t->busy = 0;
    t->done = 1;
    return sceKernelExitDeleteThread(0);
}

int task_start(task_t *t, task_fn fn, void *arg)
{
    t->fn = fn;
    t->arg = arg;
    t->done = 0;
    t->abort_flag = 0;
    t->result = -1;
    t->busy = 1;
    /* curl + OpenSSL handshakes need a generous stack */
    SceUID th = sceKernelCreateThread("vt_task", task_thread, 0x10000100, 256 * 1024, 0, 0, NULL);
    if (th < 0) {
        t->busy = 0;
        t->done = 1;
        return -1;
    }
    task_t *self = t;
    if(sceKernelStartThread(th,sizeof(self),&self)<0){sceKernelDeleteThread(th);t->busy=0;t->done=1;return -1;}
    return 0;
}

void task_wait(task_t *t)
{
    while (t->busy)
        sceKernelDelayThread(10000);
}

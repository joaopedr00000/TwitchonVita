#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
static SceKernelLwMutexWork g_mtx;
static int g_mtx_ok;
#define LOCK()   do { if (g_mtx_ok) sceKernelLockLwMutex(&g_mtx, 1, NULL); } while (0)
#define UNLOCK() do { if (g_mtx_ok) sceKernelUnlockLwMutex(&g_mtx, 1); } while (0)
static unsigned long long now_ms(void) { return sceKernelGetProcessTimeWide() / 1000ULL; }
#else
#include <time.h>
#define LOCK()   do { } while (0)
#define UNLOCK() do { } while (0)
static unsigned long long now_ms(void) { return (unsigned long long)clock() * 1000ULL / CLOCKS_PER_SEC; }
#endif

#define LOG_MAX_BYTES (1024 * 1024)

static FILE *g_f;
static long g_written;
static int g_full;
static unsigned long long g_last_flush;

void log_open(const char *path, const char *prev_path)
{
#ifdef __vita__
    if (!g_mtx_ok && sceKernelCreateLwMutex(&g_mtx, "vt_log", 0, 0, NULL) >= 0)
        g_mtx_ok = 1;
    sceIoRemove(prev_path);
    sceIoRename(path, prev_path);
#else
    remove(prev_path);
    rename(path, prev_path);
#endif
    g_f = fopen(path, "w");
    g_written = 0;
    g_full = 0;
}

void log_close(void)
{
    LOCK();
    if (g_f) {
        fclose(g_f);
        g_f = NULL;
    }
    UNLOCK();
}

void log_write(char level, const char *fmt, ...)
{
    char line[512];
    unsigned long long ms = now_ms();
    int n = snprintf(line, sizeof(line), "[%5llu.%03llu] %c ", ms / 1000ULL, ms % 1000ULL, level);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    /* one entry per line: flatten embedded newlines */
    for (char *p = line + n; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    strcat(line, "\n");

    LOCK();
    if (g_f && !g_full) {
        if (g_written + (long)strlen(line) > LOG_MAX_BYTES) {
            fputs("--- log lleno (1 MB), no se escribe mas ---\n", g_f);
            g_full = 1;
        } else {
            fputs(line, g_f);
            g_written += (long)strlen(line);
        }
        /* flushing every line blocked whichever thread logged (the video thread
         * too) on memory-card writes: errors go out at once, the rest at most
         * once per second */
        if (level == 'E' || strncmp(fmt, "startup:", 8) == 0 || ms - g_last_flush >= 1000ULL || g_full) {
            fflush(g_f);
            g_last_flush = ms;
        }
    }
#ifndef __vita__
    if (!g_f)
        fputs(line, stderr);
#endif
    UNLOCK();
}

const char *log_url(const char *url, char *out, int cap)
{
    if (!url) url = "";
    int i = 0;
    for (; url[i] && url[i] != '?' && i < cap - 1; i++)
        out[i] = url[i];
    out[i] = 0;
    return out;
}

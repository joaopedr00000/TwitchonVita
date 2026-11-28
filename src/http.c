#include "http.h"
#include "log.h"

#include <string.h>
#include <psp2/kernel/processmgr.h>

int http_global_init(void)
{
    return curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK ? 0 : -1;
}

void http_global_term(void)
{
    curl_global_cleanup();
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    buf_t *b = userdata;
    if(nmemb && size>SIZE_MAX/nmemb)return 0;
    size_t n = size * nmemb;
    if(b->size>16u*1024u*1024u || n>16u*1024u*1024u-b->size)return 0;
    if (buf_append(b, ptr, n) < 0)
        return 0;
    return n;
}

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    volatile int *abort_flag = clientp;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return (abort_flag && *abort_flag) ? 1 : 0;
}

CURL *http_new(void)
{
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    curl_easy_setopt(c, CURLOPT_USERAGENT, VT_USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024L);
    /* Restore original Vita HTTP compatibility mode. HTTPS remains encrypted
     * but server authenticity is NOT verified. Never use this for IRC TLS. */
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    return c;
}

void http_free(CURL *c)
{
    if (c)
        curl_easy_cleanup(c);
}

int http_request(CURL *c, const char *url, const char *post_body,
                 struct curl_slist *headers, buf_t *out, long *status,
                 volatile int *abort_flag)
{
    buf_reset(out);
    if (out->data)
        out->data[0] = 0;
    *status = 0;

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)abort_flag);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if (post_body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    }

    unsigned long long t0 = sceKernelGetProcessTimeWide();
    CURLcode res = curl_easy_perform(c);
    unsigned long long ms = (sceKernelGetProcessTimeWide() - t0) / 1000ULL;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, NULL);
    if (!out->data)
        buf_append(out, "", 0);

    char u[160];
    const char *m = post_body ? "POST" : "GET";
    if (res != CURLE_OK) {
        if (!(abort_flag && *abort_flag)) /* user cancels are not errors */
            LOGE("%s %s -> curl %d (%s) tras %llu ms", m, log_url(url, u, sizeof(u)), (int)res,
                 curl_easy_strerror(res), ms);
    } else if (*status >= 400 && !(out->data && strstr((const char *)out->data, "authorization_pending"))) {
        LOGE("%s %s -> HTTP %ld (%llu ms)", m, log_url(url, u, sizeof(u)), *status, ms);
    } else if (ms > 4000) {
        LOGW("%s %s lenta: %llu ms, %u bytes", m, log_url(url, u, sizeof(u)), ms, (unsigned)out->size);
    }
    return res == CURLE_OK ? 0 : -(int)res;
}

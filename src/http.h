#ifndef VT_HTTP_H
#define VT_HTTP_H

#include <curl/curl.h>
#include "buf.h"

#define VT_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"

int http_global_init(void);
void http_global_term(void);

/* One handle per thread; reusing it keeps connections alive between requests. */
CURL *http_new(void);
void http_free(CURL *c);

/* GET (post_body == NULL) or POST. The body is stored in out (NUL-terminated).
 * abort_flag may be NULL; when it becomes non-zero the transfer is cancelled.
 * Returns 0 on transport success (check *status for the HTTP code). */
int http_request(CURL *c, const char *url, const char *post_body,
                 struct curl_slist *headers, buf_t *out, long *status,
                 volatile int *abort_flag);

/* Native verified HTTPS fallback. Serialized with the chat sender's SSL
 * lifecycle; never follows redirects. Only read-only media callers retry. */
void http_native_lock(void);
void http_native_unlock(void);
int http_native_read(const char *url,const char *body,const char *oauth,int gql,buf_t *out,long *status,volatile int *abort_flag);
#endif

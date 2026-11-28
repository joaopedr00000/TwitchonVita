#ifndef VT_TLS_TRUST_H
#define VT_TLS_TRUST_H
#include <curl/curl.h>
/* Fail closed; verification never disabled. */
CURLcode vt_tls_configure(CURL *c);
CURLcode vt_tls_install_ca(void *ctx);
#endif

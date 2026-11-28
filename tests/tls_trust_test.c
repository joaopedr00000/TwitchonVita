#include "tls_trust.h"
#include <assert.h>
#include <openssl/ssl.h>
#include <stdio.h>
int main(void){
    SSL_CTX *ctx=SSL_CTX_new(TLS_client_method());
    assert(ctx);
    assert(vt_tls_install_ca(ctx)==CURLE_OK);
    assert(sk_X509_OBJECT_num(X509_STORE_get0_objects(SSL_CTX_get_cert_store(ctx)))>=100);
    SSL_CTX_free(ctx);
    assert(curl_global_init(CURL_GLOBAL_ALL)==CURLE_OK);
    CURL *c=curl_easy_init();assert(c);
    assert(vt_tls_configure(c)==CURLE_OK);
    curl_easy_cleanup(c);curl_global_cleanup();
    puts("PASS embedded Mozilla trust store; strict curl TLS configured");
    return 0;
}

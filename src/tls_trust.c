#include "tls_trust.h"
#include "vt_ca_bundle.h"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <string.h>
#include <stdio.h>

CURLcode vt_tls_install_ca(void *ctx)
{
    if(!ctx || sizeof(vt_ca_bundle)<2)return CURLE_SSL_CACERT_BADFILE;
    BIO *bio=BIO_new_mem_buf(vt_ca_bundle,(int)(sizeof(vt_ca_bundle)-1));
    if(!bio)return CURLE_OUT_OF_MEMORY;
    STACK_OF(X509_INFO) *info=PEM_X509_INFO_read_bio(bio,NULL,NULL,NULL);
    BIO_free(bio);
    if(!info){ERR_clear_error();return CURLE_SSL_CACERT_BADFILE;}
    X509_STORE *store=X509_STORE_new();
    int count=0,failed=0;
    if(store){
        for(int i=0;i<sk_X509_INFO_num(info);i++){
            X509_INFO *item=sk_X509_INFO_value(info,i);
            if(item->x509){
                if(X509_STORE_add_cert(store,item->x509)==1)count++;
                else {
                    unsigned long e=ERR_peek_last_error();
                    if(ERR_GET_LIB(e)==ERR_LIB_X509&&ERR_GET_REASON(e)==X509_R_CERT_ALREADY_IN_HASH_TABLE){ERR_clear_error();continue;}
                    failed=1;break;
                }
            }
        }
    }
    sk_X509_INFO_pop_free(info,X509_INFO_free);
    ERR_clear_error();
    if(!store||failed||count<100){X509_STORE_free(store);return CURLE_SSL_CACERT_BADFILE;}
    SSL_CTX_set_cert_store((SSL_CTX*)ctx,store);
    return CURLE_OK;
}
#ifndef __vita__
static CURLcode trust_callback(CURL *c,void *ctx,void *unused)
{
    (void)c;(void)unused;
    return vt_tls_install_ca(ctx);
}
#endif
CURLcode vt_tls_configure(CURL *c)
{
    if(!c||sizeof(vt_ca_bundle)<2)return CURLE_SSL_CACERT_BADFILE;
    const curl_version_info_data *info=curl_version_info(CURLVERSION_NOW);
    if(!info||!info->ssl_version||strncmp(info->ssl_version,"OpenSSL/",8)!=0)
        return CURLE_NOT_BUILT_IN;
    CURLcode rc;
#define OPT(k,v) do{rc=curl_easy_setopt(c,k,v);if(rc!=CURLE_OK)return rc;}while(0)
    OPT(CURLOPT_SSL_VERIFYPEER,1L);
    OPT(CURLOPT_SSL_VERIFYHOST,2L);
#ifdef __vita__
    /* Curl can fail with 77 before the SSL context callback when its
     * compiled-in CA path does not exist on Vita. Use the packaged file. */
    FILE *ca=fopen("app0:ca/cacert.pem","rb");
    if(!ca)return CURLE_SSL_CACERT_BADFILE;
    fclose(ca);
    OPT(CURLOPT_CAINFO,"app0:ca/cacert.pem");
    OPT(CURLOPT_CAPATH,(char*)NULL);
#else
    OPT(CURLOPT_CAINFO,(char*)NULL);
    OPT(CURLOPT_CAPATH,(char*)NULL);
    OPT(CURLOPT_SSL_CTX_FUNCTION,trust_callback);
#endif
    OPT(CURLOPT_SSL_SESSIONID_CACHE,0L);
    OPT(CURLOPT_FRESH_CONNECT,1L);
#undef OPT
    return CURLE_OK;
}

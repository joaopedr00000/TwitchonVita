#include "http.h"
#include "log.h"

#include <string.h>
#include <pthread.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>
#include <psp2/sysmodule.h>
#include "twitch.h"
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

static pthread_mutex_t native_lock=PTHREAD_MUTEX_INITIALIZER;
void http_native_lock(void){pthread_mutex_lock(&native_lock);}
void http_native_unlock(void){pthread_mutex_unlock(&native_lock);}
#define VT_NATIVE_VERIFY (SCE_HTTPS_FLAG_SERVER_VERIFY | SCE_HTTPS_FLAG_CN_CHECK | SCE_HTTPS_FLAG_NOT_AFTER_CHECK | SCE_HTTPS_FLAG_NOT_BEFORE_CHECK | SCE_HTTPS_FLAG_KNOWN_CA_CHECK)
int http_native_read(const char *url,const char *body,const char *oauth,int gql,buf_t *out,long *status,volatile int *abort_flag){
    /* Only the two fixed Twitch API hosts are eligible. Never pass media
     * URLs or credentials to arbitrary hosts. GraphQL is read-only here. */
    if(strncmp(url,"https://api.twitch.tv/helix/",27)&&strcmp(url,"https://gql.twitch.tv/gql"))return -CURLE_BAD_FUNCTION_ARGUMENT;
    if(oauth&&(strlen(oauth)>100||strpbrk(oauth,"\r\n")))return -CURLE_BAD_FUNCTION_ARGUMENT;
    if(body&&(!gql||strstr(body,"mutation")))return -CURLE_BAD_FUNCTION_ARGUMENT;
    *status=0;buf_reset(out);if(out->data)out->data[0]=0;
    int ssl=0,http=0,tpl=-1,conn=-1,req=-1,rc=-1,code=0;const char *stage="module";
    char auth[160]="",chunk[4096];http_native_lock();
#define NATIVE(expr) do{rc=(expr);if(rc<0)goto done;}while(0)
    if(abort_flag&&*abort_flag)goto done;
    if(sceSysmoduleIsLoaded(SCE_SYSMODULE_HTTPS)!=0)NATIVE(sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS));
    stage="ssl-init";NATIVE(sceSslInit(1024*1024));ssl=1;
    stage="http-init";NATIVE(sceHttpInit(1024*1024));http=1;
    stage="verify";NATIVE(sceHttpsEnableOption(VT_NATIVE_VERIFY));
    stage="template";NATIVE(tpl=sceHttpCreateTemplate("VitaTwitch/1.34",SCE_HTTP_VERSION_1_1,0));
    NATIVE(sceHttpSetResolveTimeOut(tpl,3000000));NATIVE(sceHttpSetResolveRetry(tpl,1));
    NATIVE(sceHttpSetConnectTimeOut(tpl,5000000));NATIVE(sceHttpSetSendTimeOut(tpl,5000000));NATIVE(sceHttpSetRecvTimeOut(tpl,5000000));
    NATIVE(sceHttpSetAutoRedirect(tpl,SCE_HTTP_DISABLE));NATIVE(sceHttpSetCookieEnabled(tpl,SCE_HTTP_DISABLE));
    NATIVE(sceHttpSetResponseHeaderMaxSize(tpl,16384));
    stage="connection";NATIVE(conn=sceHttpCreateConnectionWithURL(tpl,url,SCE_HTTP_DISABLE));
    NATIVE(req=sceHttpCreateRequestWithURL(conn,body?SCE_HTTP_METHOD_POST:SCE_HTTP_METHOD_GET,url,body?strlen(body):0));
    NATIVE(sceHttpAddRequestHeader(req,"Client-Id",TWITCH_CLIENT_ID,SCE_HTTP_HEADER_OVERWRITE));
    if(oauth&&*oauth){snprintf(auth,sizeof(auth),"%s %s",gql?"OAuth":"Bearer",oauth);NATIVE(sceHttpAddRequestHeader(req,"Authorization",auth,SCE_HTTP_HEADER_OVERWRITE));}
    NATIVE(sceHttpAddRequestHeader(req,"Accept-Encoding","identity",SCE_HTTP_HEADER_OVERWRITE));
    if(body)NATIVE(sceHttpAddRequestHeader(req,"Content-Type","text/plain;charset=UTF-8",SCE_HTTP_HEADER_OVERWRITE));
    stage="send";if(abort_flag&&*abort_flag){rc=-1;goto done;}NATIVE(sceHttpSendRequest(req,body,body?(unsigned)strlen(body):0));
    NATIVE(sceHttpGetStatusCode(req,&code));*status=code;
    stage="read";while(!(abort_flag&&*abort_flag)){
        int n=sceHttpReadData(req,chunk,sizeof(chunk));if(n<0){rc=n;goto done;}if(!n){rc=0;goto done;}
        if(out->size>2u*1024u*1024u||(size_t)n>2u*1024u*1024u-out->size||buf_append(out,chunk,(size_t)n)<0){rc=-1;goto done;}
    }rc=-1;
done:
    if(rc<0){int se=0;unsigned detail=0;if(req>=0)sceHttpsGetSslError(req,&se,&detail);
        LOGW("media native HTTPS: stage=%s rc=%08X ssl=%08X detail=%08X",stage,(unsigned)rc,(unsigned)se,detail);}
    else LOGI("media native HTTPS: HTTP %ld bytes=%u",*status,(unsigned)out->size);
    if(req>=0)sceHttpDeleteRequest(req);if(conn>=0)sceHttpDeleteConnection(conn);if(tpl>=0)sceHttpDeleteTemplate(tpl);
    if(http)sceHttpTerm();if(ssl)sceSslTerm();memset(auth,0,sizeof(auth));http_native_unlock();
    if(!out->data)buf_append(out,"",0);
#undef NATIVE
    return rc<0?-CURLE_SSL_CONNECT_ERROR:0;
}

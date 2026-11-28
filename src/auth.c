#include "i18n.h"
#include "auth.h"
#include "json.h"
#include "twitch.h"
#include <stdio.h>
#include <string.h>

#ifndef VT_NO_NET
#include "http.h"
#endif

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

int auth_parse_device(const char *json, auth_device_t *d, char *err, size_t errlen)
{
    json_t *r = json_parse(json);
    const char *dc = json_str(r, "device_code", NULL);
    const char *uc = json_str(r, "user_code", NULL);
    int ret = -1;

    memset(d, 0, sizeof(*d));
    if (dc && uc) {
        copy_str(d->device_code, sizeof(d->device_code), dc);
        copy_str(d->user_code, sizeof(d->user_code), uc);
        copy_str(d->uri, sizeof(d->uri), json_str(r, "verification_uri", "https://www.twitch.tv/activate"));
        d->interval = (int)json_num(r, "interval", 5);
        d->expires_in = (int)json_num(r, "expires_in", 1800);
        if (d->interval < 1)
            d->interval = 5;
        ret = 0;
    } else {
        snprintf(err, errlen, vt_msg("Twitch rechazo el inicio de sesion: %s"),
                 json_str(r, "message", vt_msg("respuesta no valida")));
    }
    json_free(r);
    return ret;
}

int auth_parse_token(const char *json, auth_t *a, char *err, size_t errlen)
{
    json_t *r = json_parse(json);
    const char *at = json_str(r, "access_token", NULL);
    int ret = -1;

    if (at) {
        copy_str(a->access, sizeof(a->access), at);
        copy_str(a->refresh, sizeof(a->refresh), json_str(r, "refresh_token", ""));
        ret = AUTH_OK;
    } else {
        const char *msg = json_str(r, "message", "");
        if (!strcmp(msg, "authorization_pending"))
            ret = AUTH_PENDING;
        else if (!strcmp(msg, "slow_down"))
            ret = AUTH_SLOW_DOWN;
        else if (!strcmp(msg, "expired_token"))
            snprintf(err, errlen, vt_msg("El codigo ha caducado. Vuelve a intentarlo."));
        else if (!strcmp(msg, "access_denied"))
            snprintf(err, errlen, vt_msg("Has rechazado el acceso en Twitch."));
        else
            snprintf(err, errlen, vt_msg("Error de Twitch: %s"), msg[0] ? msg : vt_msg("respuesta no valida"));
    }
    json_free(r);
    return ret;
}

int auth_parse_validate(const char *json, auth_t *a)
{
    json_t *r = json_parse(json);
    const char *login = json_str(r, "login", NULL);
    const char *uid = json_str(r, "user_id", NULL);
    int ret = -1;
    if (login && uid) {
        copy_str(a->login, sizeof(a->login), login);
        copy_str(a->user_id, sizeof(a->user_id), uid);
        if (!a->display[0])
            copy_str(a->display, sizeof(a->display), login);
        ret = 0;
    }
    json_free(r);
    return ret;
}

/* Simple key=value file. */
int auth_load(auth_t *a, const char *path)
{
    char line[320];
    FILE *f = fopen(path, "r");
    memset(a, 0, sizeof(*a));
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "access")) copy_str(a->access, sizeof(a->access), v);
        else if (!strcmp(line, "refresh")) copy_str(a->refresh, sizeof(a->refresh), v);
        else if (!strcmp(line, "user_id")) copy_str(a->user_id, sizeof(a->user_id), v);
        else if (!strcmp(line, "login")) copy_str(a->login, sizeof(a->login), v);
        else if (!strcmp(line, "display")) copy_str(a->display, sizeof(a->display), v);
        else if (!strcmp(line, "avatar")) copy_str(a->avatar, sizeof(a->avatar), v);
    }
    fclose(f);
    return a->access[0] ? 0 : -1;
}

int auth_save(const auth_t *a, const char *path)
{
    char tmp[512];
    if(snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp))return -1;
    FILE *f=fopen(tmp,"w");if(!f)return -1;
    int ok=fprintf(f,"access=%s\nrefresh=%s\nuser_id=%s\nlogin=%s\ndisplay=%s\navatar=%s\n",a->access,a->refresh,a->user_id,a->login,a->display,a->avatar)>=0;
    if(fflush(f)!=0)ok=0;
    if(fclose(f)!=0)ok=0;
    if(!ok||rename(tmp,path)!=0){remove(tmp);return -1;}
    return 0;
}

#ifndef VT_NO_NET
static int post_form(const char *url, const char *form, buf_t *resp, long *status,
                     volatile int *abort_flag)
{
    CURL *c = http_new();
    struct curl_slist *hdr = NULL;
    int r;
    if (!c)
        return -1;
    hdr = curl_slist_append(hdr, "Content-Type: application/x-www-form-urlencoded");
    r = http_request(c, url, form, hdr, resp, status, abort_flag);
    curl_slist_free_all(hdr);
    http_free(c);
    return r;
}

int auth_device_start(auth_device_t *d, char *err, size_t errlen, volatile int *abort_flag)
{
    buf_t resp = {0};
    long status = 0;
    int ret;
    char form[256];
    snprintf(form, sizeof(form), "client_id=%s&scopes=user%%3Aread%%3Afollows%%20chat%%3Aread%%20chat%%3Aedit%%20user%%3Awrite%%3Achat",
             TWITCH_CLIENT_ID);
    if (post_form("https://id.twitch.tv/oauth2/device", form, &resp, &status, abort_flag) < 0) {
        snprintf(err, errlen, vt_msg("Error de red al contactar id.twitch.tv"));
        buf_free(&resp);
        return -1;
    }
    ret = auth_parse_device((const char *)resp.data, d, err, errlen);
    buf_free(&resp);
    return ret;
}

int auth_device_poll(const auth_device_t *d, auth_t *a, char *err, size_t errlen,
                     volatile int *abort_flag)
{
    buf_t resp = {0};
    long status = 0;
    int ret;
    char form[384];
    snprintf(form, sizeof(form),
             "client_id=%s&scopes=user%%3Aread%%3Afollows%%20chat%%3Aread%%20chat%%3Aedit%%20user%%3Awrite%%3Achat"
             "&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
             TWITCH_CLIENT_ID, d->device_code);
    if (post_form("https://id.twitch.tv/oauth2/token", form, &resp, &status, abort_flag) < 0) {
        buf_free(&resp);
        return AUTH_PENDING; /* transient network error: keep polling */
    }
    ret = auth_parse_token((const char *)resp.data, a, err, errlen);
    buf_free(&resp);
    return ret;
}

static int validate(auth_t *a, volatile int *abort_flag)
{
    CURL *c = http_new();
    struct curl_slist *hdr = NULL;
    buf_t resp = {0};
    long status = 0;
    char h[128];
    int ret = -2;
    if (!c)
        return -2;
    snprintf(h, sizeof(h), "Authorization: OAuth %s", a->access);
    hdr = curl_slist_append(hdr, h);
    if (http_request(c, "https://id.twitch.tv/oauth2/validate", NULL, hdr, &resp, &status, abort_flag) == 0) {
        if (status == 200 && auth_parse_validate((const char *)resp.data, a) == 0)
            ret = 0;
        else if (status == 401)
            ret = -1;
    }
    curl_slist_free_all(hdr);
    buf_free(&resp);
    http_free(c);
    return ret;
}

static int refresh(auth_t *a, char *err, size_t errlen, volatile int *abort_flag)
{
    buf_t resp = {0};
    long status = 0;
    char form[384];
    int ret = -1;
    CURL *c = http_new();
    char *rt;
    if (!c || !a->refresh[0]) {
        http_free(c);
        return -1;
    }
    rt = curl_easy_escape(c, a->refresh, 0);
    snprintf(form, sizeof(form), "client_id=%s&grant_type=refresh_token&refresh_token=%s",
             TWITCH_CLIENT_ID, rt ? rt : "");
    curl_free(rt);
    http_free(c);
    if (post_form("https://id.twitch.tv/oauth2/token", form, &resp, &status, abort_flag) < 0)
        ret = -2;
    else if (auth_parse_token((const char *)resp.data, a, err, errlen) == AUTH_OK)
        ret = 0;
    buf_free(&resp);
    return ret;
}

static void fetch_profile(auth_t *a, volatile int *abort_flag)
{
    buf_t resp = {0};
    char err[64];
    if (twitch_gql("{\"query\":\"query { currentUser { displayName profileImageURL(width: 70) } }\"}",
                   a->access, &resp, err, sizeof(err), abort_flag) == 0) {
        json_t *r = json_parse((const char *)resp.data);
        const char *dn = json_str(r, "data.currentUser.displayName", NULL);
        const char *av = json_str(r, "data.currentUser.profileImageURL", NULL);
        if (dn) copy_str(a->display, sizeof(a->display), dn);
        if (av) copy_str(a->avatar, sizeof(a->avatar), av);
        json_free(r);
    }
    buf_free(&resp);
}

int auth_ensure_valid(auth_t *a, char *err, size_t errlen, volatile int *abort_flag)
{
    int r = validate(a, abort_flag);
    if (r == -1) {
        r = refresh(a, err, errlen, abort_flag);
        if (r == 0)
            r = validate(a, abort_flag);
    }
    if (r == 0)
        fetch_profile(a, abort_flag);
    else if (r == -1)
        snprintf(err, errlen, vt_msg("La sesion ha caducado. Vuelve a iniciar sesion."));
    else
        snprintf(err, errlen, vt_msg("No se pudo comprobar la sesion (sin conexion?)"));
    return r;
}
#endif

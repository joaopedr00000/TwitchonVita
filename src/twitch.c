#include "i18n.h"
#include "twitch.h"
#include "json.h"
#include "log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VT_NO_NET
#include "http.h"
#endif

int twitch_sanitize_login(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        char c = (char)tolower((unsigned char)*in);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            out[n++] = c;
    }
    out[n] = 0;
    return (int)n;
}

int twitch_json_escape(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];
        const char *rep = NULL;
        if (c == '"') rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20) { snprintf(tmp, sizeof(tmp), "\\u%04x", c); rep = tmp; }
        if (rep) {
            size_t l = strlen(rep);
            if (n + l + 1 > cap) return -1;
            memcpy(out + n, rep, l);
            n += l;
        } else {
            if (n + 2 > cap) return -1;
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
    return (int)n;
}

static int build_gql_profile(const char *login,const char *profile,char *out,size_t cap)
{
    /* Inline query (no persisted-query hash) so it keeps working when hashes rotate. */
    return snprintf(out, cap,
        "{\"operationName\":\"PlaybackAccessToken_Template\","
        "\"query\":\"query PlaybackAccessToken_Template($login: String!, $playerType: String!, $platform: String!) "
        "{ streamPlaybackAccessToken(channelName: $login, params: {platform: $platform, playerBackend: \\\"mediaplayer\\\", playerType: $playerType}) "
        "{ value signature __typename } }\","
        "\"variables\":{\"login\":\"%s\",\"playerType\":\"%s\",\"platform\":\"web\"}}",
        login,profile);
}

int twitch_build_gql_body(const char *login,char *out,size_t cap)
{
    return build_gql_profile(login,"site",out,cap);
}

#define STREAM_FIELDS \
    "title viewersCount previewImageURL(width: 320, height: 180) game { displayName }"
#define USER_FIELDS \
    "login displayName profileImageURL(width: 70) stream { " STREAM_FIELDS " }"

int twitch_build_list_body(twitch_list_t kind, const char *query, char *out, size_t cap)
{
    switch (kind) {
    case TW_LIST_FOLLOWED:
        return snprintf(out, cap,
            "{\"query\":\"query { currentUser { followedLiveUsers(first: 100) { edges { node { "
            USER_FIELDS " } } } } }\"}");
    case TW_LIST_TOP: {
        /* Only a fixed set of valid GraphQL language enums can enter the query. */
        static const char *const langs[]={"PT","EN","ES","FR","DE","IT","RU","JA","KO","ZH","ZH_HK","ID","TL","NL","PL","TR","UK","SV","AR","TH","VI","OTHER"};
        const char *lang=NULL;
        for(unsigned i=0;query && i<sizeof(langs)/sizeof(langs[0]);i++)
            if(!strcmp(query,langs[i])){lang=langs[i];break;}
        char filter[64]="";
        if(lang)snprintf(filter,sizeof(filter),", options: {languages: [%s]}",lang);
        return snprintf(out,cap,
            "{\"query\":\"query { streams(first: 30%s) { edges { node { " STREAM_FIELDS
            " broadcaster { login displayName profileImageURL(width: 70) } } } } }\"}",filter);
    }
    case TW_LIST_SEARCH: {
        char q[256];
        if (twitch_json_escape(query ? query : "", q, sizeof(q)) < 0)
            return -1;
        return snprintf(out, cap,
            "{\"query\":\"query Search($q: String!) { searchFor(userQuery: $q, platform: \\\"web\\\", "
            "options: {targets: [{index: CHANNEL}]}) { channels { edges { item { ... on User { "
            USER_FIELDS " } } } } } }\",\"variables\":{\"q\":\"%s\"}}", q);
    }
    }
    return -1;
}

int twitch_parse_token(const char *json, char *token, size_t token_cap,
                       char *sig, size_t sig_cap)
{
    json_t *root = json_parse(json);
    json_t *t = json_path(root, "data.streamPlaybackAccessToken");
    const char *v = json_str(t, "value", NULL);
    const char *s = json_str(t, "signature", NULL);
    int ret = -1;
    if (v && s && strlen(v) < token_cap && strlen(s) < sig_cap) {
        strcpy(token, v);
        strcpy(sig, s);
        ret = 0;
    }
    json_free(root);
    return ret;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

/* user: object with login/displayName/profileImageURL/stream{...} */
static void fill_from_user(twitch_stream_t *s, const json_t *user, const json_t *stream)
{
    copy_str(s->login, sizeof(s->login), json_str(user, "login", ""));
    copy_str(s->display, sizeof(s->display), json_str(user, "displayName", s->login));
    copy_str(s->avatar, sizeof(s->avatar), json_str(user, "profileImageURL", ""));
    if (stream && stream->type == JSON_OBJECT) {
        s->live = 1;
        s->viewers = (int)json_num(stream, "viewersCount", 0);
        copy_str(s->title, sizeof(s->title), json_str(stream, "title", ""));
        copy_str(s->game, sizeof(s->game), json_str(stream, "game.displayName", ""));
        copy_str(s->preview, sizeof(s->preview), json_str(stream, "previewImageURL", ""));
    }
}

/* "service error (currentUser.followedLiveUsers)"; NULL when there are no errors */
static const char *gql_error_text(const json_t *root, char *buf, size_t cap)
{
    const char *msg = json_str(root, "errors.0.message", NULL);
    if (!msg)
        return NULL;
    size_t n = (size_t)snprintf(buf, cap, "%s", msg);
    json_t *path = json_path(root, "errors.0.path");
    if (path && path->type == JSON_ARRAY && path->child && n + 4 < cap) {
        n += (size_t)snprintf(buf + n, cap - n, " (");
        for (json_t *p = path->child; p && n + 2 < cap; p = p->next) {
            if (p->type == JSON_STRING)
                n += (size_t)snprintf(buf + n, cap - n, "%s%s", p == path->child ? "" : ".", p->str);
            else if (p->type == JSON_NUMBER)
                n += (size_t)snprintf(buf + n, cap - n, "[%d]", (int)p->num);
        }
        if (n + 2 < cap)
            snprintf(buf + n, cap - n, ")");
    }
    return buf;
}

int twitch_parse_list(const char *json, twitch_list_t kind, twitch_stream_t *out, int max,
                      char *err, size_t errlen)
{
    json_t *root = json_parse(json);
    json_t *edges = NULL;
    int n = 0;
    char gerr[160];

    if (!root) {
        snprintf(err, errlen, vt_msg("Respuesta de Twitch no valida"));
        return -1;
    }
    /* GraphQL may return errors next to valid data (e.g. one broken stream in
     * the list): only fail when the list itself is missing. */
    const char *gql_err = gql_error_text(root, gerr, sizeof(gerr));
    if (gql_err)
        LOGW("gql: %s", gql_err);

    switch (kind) {
    case TW_LIST_FOLLOWED: {
        json_t *cu = json_path(root, "data.currentUser");
        if ((!cu || cu->type != JSON_OBJECT) && !gql_err) {
            snprintf(err, errlen, vt_msg("Sesion caducada: vuelve a iniciar sesion"));
            json_free(root);
            return -2;
        }
        edges = json_path(cu, "followedLiveUsers.edges");
        break;
    }
    case TW_LIST_TOP:
        edges = json_path(root, "data.streams.edges");
        break;
    case TW_LIST_SEARCH:
        edges = json_path(root, "data.searchFor.channels.edges");
        break;
    }
    if (!edges || edges->type != JSON_ARRAY) {
        snprintf(err, errlen, vt_msg("Twitch: %s"), gql_err ? gql_err : vt_msg("respuesta sin lista"));
        json_free(root);
        return -1;
    }

    for (json_t *e = edges ? edges->child : NULL; e && n < max; e = e->next) {
        twitch_stream_t *s = &out[n];
        memset(s, 0, sizeof(*s));
        if (kind == TW_LIST_TOP) {
            json_t *node = json_get(e, "node");
            json_t *b = json_get(node, "broadcaster");
            if (!b || b->type != JSON_OBJECT)
                continue;
            fill_from_user(s, b, node);
        } else {
            json_t *u = json_get(e, kind == TW_LIST_SEARCH ? "item" : "node");
            if (!u || u->type != JSON_OBJECT)
                continue;
            fill_from_user(s, u, json_get(u, "stream"));
        }
        if (s->login[0])
            n++;
    }
    json_free(root);
    return n;
}

int twitch_build_follows_body(const char *after, char *out, size_t cap)
{
    char cur[256];
    if (twitch_json_escape(after ? after : "", cur, sizeof(cur)) < 0)
        return -1;
    return snprintf(out, cap,
        "{\"query\":\"query F($after: Cursor) { currentUser { follows(first: 100, after: $after) { "
        "edges { cursor node { " USER_FIELDS " } } pageInfo { hasNextPage } } } }\","
        "\"variables\":{\"after\":%s%s%s}}",
        after && after[0] ? "\"" : "", after && after[0] ? cur : "null", after && after[0] ? "\"" : "");
}

int twitch_parse_follows(const char *json, twitch_stream_t *out, int max, int *n,
                         char *cursor, size_t cursor_cap, int *has_next, char *err, size_t errlen)
{
    char gerr[160];
    json_t *root = json_parse(json);
    *has_next = 0;
    if (!root) {
        snprintf(err, errlen, vt_msg("Respuesta de Twitch no valida"));
        return -1;
    }
    const char *gql_err = gql_error_text(root, gerr, sizeof(gerr));
    if (gql_err)
        LOGW("gql follows: %s", gql_err);
    json_t *cu = json_path(root, "data.currentUser");
    if ((!cu || cu->type != JSON_OBJECT) && !gql_err) {
        snprintf(err, errlen, vt_msg("Sesion caducada: vuelve a iniciar sesion"));
        json_free(root);
        return -2;
    }
    json_t *edges = json_path(cu, "follows.edges");
    if (!edges || edges->type != JSON_ARRAY) {
        snprintf(err, errlen, vt_msg("Twitch: %s"), gql_err ? gql_err : vt_msg("respuesta sin lista"));
        json_free(root);
        return -1;
    }
    for (json_t *e = edges->child; e; e = e->next) {
        copy_str(cursor, cursor_cap, json_str(e, "cursor", ""));
        json_t *u = json_get(e, "node");
        json_t *st = json_get(u, "stream");
        if (!u || u->type != JSON_OBJECT || !st || st->type != JSON_OBJECT || *n >= max)
            continue;
        twitch_stream_t *s = &out[*n];
        memset(s, 0, sizeof(*s));
        fill_from_user(s, u, st);
        if (s->login[0])
            (*n)++;
    }
    *has_next = json_num(cu, "follows.pageInfo.hasNextPage", 0) != 0 && cursor[0];
    json_free(root);
    return 0;
}

static int by_viewers(const void *a, const void *b)
{
    return ((const twitch_stream_t *)b)->viewers - ((const twitch_stream_t *)a)->viewers;
}

void twitch_sort_by_viewers(twitch_stream_t *s, int n)
{
    if (n > 1)
        qsort(s, (size_t)n, sizeof(*s), by_viewers);
}

#ifndef VT_NO_NET
int twitch_gql(const char *body, const char *oauth, buf_t *resp,
               char *err, size_t errlen, volatile int *abort_flag)
{
    struct curl_slist *hdr = NULL;
    long status = 0;
    int ret = -1;
    char auth[128];
    CURL *c = http_new();

    if (!c) {
        snprintf(err, errlen, vt_msg("No se pudo iniciar curl"));
        return -1;
    }
    hdr = curl_slist_append(hdr, "Client-ID: " TWITCH_CLIENT_ID);
    hdr = curl_slist_append(hdr, "Content-Type: text/plain;charset=UTF-8");
    if (oauth && oauth[0]) {
        snprintf(auth, sizeof(auth), "Authorization: OAuth %s", oauth);
        hdr = curl_slist_append(hdr, auth);
    }
    int net=http_request(c,"https://gql.twitch.tv/gql",body,hdr,resp,&status,abort_flag);
    if(net==-(int)CURLE_COULDNT_RESOLVE_HOST||net==-(int)CURLE_COULDNT_CONNECT||
       net==-(int)CURLE_OPERATION_TIMEDOUT||net==-(int)CURLE_RECV_ERROR||
       net==-(int)CURLE_SEND_ERROR){
        if(!(abort_flag&&*abort_flag))
            net=http_request(c,"https://gql.twitch.tv/gql",body,hdr,resp,&status,abort_flag);
    }
    if(net<0)
        snprintf(err,errlen,"%s (curl %d)",vt_msg("Error de red al contactar con Twitch"),-net);
    else if (status == 401)
        snprintf(err, errlen, vt_msg("Sesion caducada: vuelve a iniciar sesion"));
    else if (status != 200)
        snprintf(err, errlen, vt_msg("gql.twitch.tv respondio HTTP %ld"), status);
    else
        ret = 0;
    curl_slist_free_all(hdr);
    http_free(c);
    return ret;
}

int twitch_fetch_list(twitch_list_t kind, const char *query, const char *oauth,
                      twitch_stream_t *out, int max, char *err, size_t errlen,
                      volatile int *abort_flag)
{
    char body[1024];
    buf_t resp = {0};
    int n;
    /* followedLiveUsers fails for some accounts every time: once it has failed,
     * go straight to the fallback for the rest of the session */
    static int followed_broken;

    if (kind == TW_LIST_FOLLOWED && (!oauth || !oauth[0])) {
        snprintf(err, errlen, vt_msg("Inicia sesion para ver tus canales seguidos"));
        return -2;
    }
    if (kind == TW_LIST_FOLLOWED && followed_broken) {
        n = -1;
        snprintf(err, errlen, vt_msg("fallo antes en esta sesion"));
    } else {
        if (twitch_build_list_body(kind, query, body, sizeof(body)) < 0) {
            snprintf(err, errlen, vt_msg("Busqueda demasiado larga"));
            return -1;
        }
        if (twitch_gql(body, oauth, &resp, err, errlen, abort_flag) < 0) {
            buf_free(&resp);
            return -1;
        }
        n = twitch_parse_list((const char *)resp.data, kind, out, max, err, errlen);
    }
    if (kind == TW_LIST_FOLLOWED && n == -1 && !(abort_flag && *abort_flag)) {
        followed_broken = 1;
        /* followedLiveUsers fails for some accounts ("service error"): walk the
         * full follow list instead and keep the channels that are live. */
        static char body2[1536];
        char cursor[256] = "";
        int count = 0, has_next = 1, page;
        LOGW("Siguiendo: followedLiveUsers fallo (%s), uso la lista completa de seguidos", err);
        for (page = 0; page < 20 && has_next; page++) {
            if (twitch_build_follows_body(cursor, body2, sizeof(body2)) < 0 ||
                twitch_gql(body2, oauth, &resp, err, errlen, abort_flag) < 0) {
                n = -1;
                break;
            }
            n = twitch_parse_follows((const char *)resp.data, out, max, &count, cursor, sizeof(cursor),
                                     &has_next, err, errlen);
            if (n < 0)
                break;
        }
        if (n >= 0 || count > 0) {
            LOGI("Siguiendo: %d paginas de seguidos, %d en directo", page, count);
            twitch_sort_by_viewers(out, count);
            n = count;
        }
    }
    buf_free(&resp);
    return n;
}

static int get_variants_profile(const char *channel,const char *oauth,const char *profile,m3u8_variant_t *out,int max,
                        char *err, size_t errlen, volatile int *abort_flag)
{
    char login[64];
    static char body[1024];
    static char token[8192];
    static char sig[128];
    static char url[12288];
    buf_t resp = {0};
    long status = 0;
    int ret = -1;
    CURL *c = NULL;

    if (twitch_sanitize_login(channel, login, sizeof(login)) == 0) {
        snprintf(err, errlen, vt_msg("Nombre de canal no valido"));
        return -1;
    }

    /* 1) Playback access token through Twitch's GraphQL API. With the user's
     *    OAuth token, subscribers/Turbo users get their ad-free playlist. */
    build_gql_profile(login,profile,body,sizeof(body));
    if (twitch_gql(body, oauth, &resp, err, errlen, abort_flag) < 0) {
        /* an expired login must not block anonymous viewing */
        if (!(oauth && oauth[0]) ||
            twitch_gql(body, NULL, &resp, err, errlen, abort_flag) < 0)
            goto out;
    }
    if (twitch_parse_token((const char *)resp.data, token, sizeof(token), sig, sizeof(sig)) < 0) {
        snprintf(err, errlen, vt_msg("El canal '%s' no existe"), login);
        goto out;
    }

    c = http_new();
    if (!c) {
        snprintf(err, errlen, vt_msg("No se pudo iniciar curl"));
        goto out;
    }

    /* 2) Master playlist from usher (same parameters streamlink sends) */
    char *etoken = curl_easy_escape(c, token, 0);
    snprintf(url, sizeof(url),
             "https://usher.ttvnw.net/api/channel/hls/%s.m3u8"
             "?player=twitchweb&p=%d&type=any&allow_source=true&allow_audio_only=true"
             "&allow_spectre=false&playlist_include_framerate=true&supported_codecs=h264"
             "&sig=%s&token=%s",
             login, rand() % 1000000, sig, etoken ? etoken : "");
    curl_free(etoken);

    if (http_request(c, url, NULL, NULL, &resp, &status, abort_flag) < 0) {
        snprintf(err, errlen, vt_msg("Error de red al contactar usher.ttvnw.net"));
        goto out;
    }
    if (status == 404) {
        snprintf(err, errlen, vt_msg("'%s' no esta en directo"), login);
        goto out;
    }
    if (status == 403) {
        snprintf(err, errlen, vt_msg("Twitch no permite ver este canal (bloqueo regional o solo suscriptores)"));
        goto out;
    }
    if (status != 200) {
        snprintf(err, errlen, vt_msg("usher respondio HTTP %ld"), status);
        goto out;
    }

    ret = m3u8_parse_master((const char *)resp.data, out, max);
    if (ret <= 0) {
        snprintf(err, errlen, vt_msg("No se encontraron calidades en la playlist"));
        ret = -1;
    }

out:
    buf_free(&resp);
    http_free(c);
    return ret;
}

int twitch_get_variants(const char *channel,const char *oauth,m3u8_variant_t *out,int max,
                        char *err,size_t errlen,volatile int *abort_flag)
{
    return get_variants_profile(channel,oauth,"site",out,max,err,errlen,abort_flag);
}

/* Experimental direct fallback: no external proxy; never swap incompatible
 * renditions. Probe current media, not just the master playlist. The caller
 * reopens its decoder rather than mixing timestamps from two HLS sessions. */
int twitch_find_clean_variant(const char *channel,const char *oauth,const char *current,
                              int width,int height,float fps,char *out,
                              volatile int *abort_flag)
{
    static m3u8_variant_t variants[M3U8_MAX_VARIANTS];
    static m3u8_media_t media;
    static const char *profiles[]={"embed","popout"};
    char err[160];
    buf_t pl={0};
    CURL *c=http_new();
    if(!c)return 0;
    int found=0;
    for(int p=0;p<2&&!found&&!*abort_flag;p++){
        int n=get_variants_profile(channel,oauth,profiles[p],variants,M3U8_MAX_VARIANTS,
                                   err,sizeof(err),abort_flag);
        for(int i=0;i<n&&!*abort_flag;i++){
            const m3u8_variant_t *v=&variants[i];
            if(v->audio_only||v->width!=width||v->height!=height||
               (fps>0&&v->fps>0&&(v->fps-fps>1.0f||fps-v->fps>1.0f))||
               strncmp(v->url,"https://",8)||!strcmp(v->url,current))continue;
            long status=0;
            if(http_request(c,v->url,NULL,NULL,&pl,&status,abort_flag)==0&&status==200&&
               m3u8_parse_media((const char *)pl.data,v->url,&media)==0&&
               !media.ended&&media.count>=2&&
               !media.seg[media.count-1].is_ad&&!media.seg[media.count-2].is_ad){
                snprintf(out,M3U8_URL_MAX,"%s",v->url);
                found=1;
                LOGI("hls: alternate clean rendition found (%s)",profiles[p]);
            }
            break; /* one compatible rendition per profile */
        }
    }
    buf_free(&pl);
    http_free(c);
    return found;
}
#endif

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
#include "tls_trust.h"
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

int twitch_channel_url(const char *login, char *out, size_t cap)
{
    if (!login || !*login || !out || !cap || strlen(login) > 25) return -1;
    for (const unsigned char *p=(const unsigned char *)login;*p;p++)
        if (!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_')) return -1;
    int n=snprintf(out,cap,"https://www.twitch.tv/%s",login);
    return n>=0 && (size_t)n<cap?0:-1;
}

int twitch_parse_channel_bio(const char *json, char *bio, size_t cap)
{
    if (!bio || !cap) return -1;
    bio[0]=0;
    if (!json) return -1;
    json_t *root=json_parse(json);
    json_t *u=json_path(root,"data.user");
    if (!u || u->type!=JSON_OBJECT) {json_free(root);return -1;}
    const char *s=json_str(u,"description","");
    size_t n=strlen(s);
    if(n>=cap){n=cap-1;while(n&&((unsigned char)s[n]&0xc0)==0x80)--n;}
    memcpy(bio,s,n);bio[n]=0;
    json_free(root);return 0;
}

int twitch_parse_follow_state(const char *json,char *id,size_t cap)
{
    if(!id||!cap)return -1;
    id[0]=0;if(!json)return -1;
    json_t *root=json_parse(json);
    json_t *u=json_path(root,"data.user");
    const char *s=json_str(u,"id",NULL);
    json_t *rel=json_path(u,"self.follower"),*can=json_path(u,"self.canFollow");
    int state=-1;
    if(!json_get(root,"errors")&&s&&*s&&strlen(s)<cap&&strlen(s)==strspn(s,"0123456789")&&rel){
        if(rel->type==JSON_OBJECT)state=1;
        else if(rel->type==JSON_NULL&&can&&can->type==JSON_BOOL)state=can->num?0:2;
        if(state>=0)snprintf(id,cap,"%s",s);
    }
    json_free(root);return state;
}
int twitch_parse_helix_follow(const char *json,const char *target)
{
    if(!json||!target||!*target)return -1;
    json_t *root=json_parse(json);
    json_t *data=json_get(root,"data");
    int state=-1;
    if(data&&data->type==JSON_ARRAY&&!json_get(root,"error")){
        if(data->count==0)state=0;
        else for(json_t *x=data->child;x;x=x->next){
            const char *id=json_str(x,"broadcaster_id","");
            if(!strcmp(id,target)){state=1;break;}
        }
    }
    json_free(root);return state;
}
const char *twitch_follow_label(int state)
{
    int l=vt_lang_current();
    if(state==1)return l==2?"Deixar de seguir":l==1?"Dejar de seguir":"Unfollow";
    if(state==0)return l==2||l==1?"Seguir":"Follow";
    if(state==2)return l==2?"Indisponível":l==1?"No disponible":"Unavailable";
    return l==2?"Consultar Follow":l==1?"Consultar seguimiento":"Check follow status";
}
const char *twitch_follow_notice(int status)
{
    int l=vt_lang_current();
    switch(status){
    case 1:return l==2?"Entre na sua conta para seguir.":l==1?"Inicia sesión para seguir.":"Sign in to follow.";
    case 2:return l==2?"Confirmando com a Twitch...":l==1?"Confirmando con Twitch...":"Checking with Twitch...";
    case 3:return l==2?"Estado confirmado pela Twitch.":l==1?"Estado confirmado por Twitch.":"Confirmed by Twitch.";
    case 4:return l==2?"A Twitch não confirmou a ação. Tente consultar novamente.":l==1?"Twitch no confirmó la acción.":"Twitch did not confirm. Check again.";
    case 5:return l==2?"Twitch exige verificação de integridade.":l==1?"Twitch exige verificación de integridad.":"Twitch requires an integrity check.";
    case 6:return l==2?"Sessão ou permissão recusada pela Twitch.":l==1?"Sesión o permiso rechazado.":"Twitch rejected session or permission.";
    case 7:return l==2?"Falha na conexão segura (TLS).":l==1?"Error de conexión segura (TLS).":"Secure connection failed (TLS).";
    case 8:return l==2?"Falha de rede ao consultar Twitch.":l==1?"Error de red al consultar Twitch.":"Twitch network request failed.";
    case 9:return l==2?"Resposta da API incompatível.":l==1?"Respuesta de API incompatible.":"Unsupported API response.";
    case 10:return l==2?"Twitch recusou ou não aplicou o Follow.":l==1?"Twitch no aplicó el seguimiento.":"Twitch rejected or did not apply follow.";
    default:return l==2?"Follow experimental na conta Twitch.":l==1?"Seguimiento experimental en Twitch.":"Experimental Twitch account follow.";
    }
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
    case TW_LIST_ALL_FOLLOWS:
    case TW_LIST_CATEGORY: return -1; /* Helix path, not GQL */
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

/* Twitch Helix image templates, bounded before inserting them into cache. */
static void category_image_url(const char *src,char *out,size_t cap,int w,int h)
{
    out[0]=0;
    if(!src||strncmp(src,"https://",8))return;
    size_t n=0;
    while(*src){
        char tmp[16];const char *piece=src;size_t len=1,advance=1;
        if(!strncmp(src,"{width}",7)){snprintf(tmp,sizeof(tmp),"%d",w);piece=tmp;len=strlen(tmp);advance=7;}
        else if(!strncmp(src,"{height}",8)){snprintf(tmp,sizeof(tmp),"%d",h);piece=tmp;len=strlen(tmp);advance=8;}
        if(n+len>=cap){out[0]=0;return;}
        memcpy(out+n,piece,len);n+=len;src+=advance;
    }
    out[n]=0;
}
void twitch_category_cover(const char *name,char *out,size_t cap)
{
    if(!out||!cap)return;
    out[0]=0;if(!name||!*name)return;
    static const char prefix[]="https://static-cdn.jtvnw.net/ttv-boxart/";
    static const char suffix[]="-144x192.jpg";
    static const char hex[]="0123456789ABCDEF";
    size_t n=sizeof(prefix)-1;
    if(n+sizeof(suffix)>=cap)return;
    memcpy(out,prefix,n);
    for(const unsigned char *s=(const unsigned char*)name;*s;s++){
        int plain=(*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'||*s=='.'||*s=='~';
        if(n+(plain?1:3)+sizeof(suffix)>cap){out[0]=0;return;}
        if(plain)out[n++]=(char)*s;
        else{out[n++]='%';out[n++]=hex[*s>>4];out[n++]=hex[*s&15];}
    }
    memcpy(out+n,suffix,sizeof(suffix));
}
int twitch_parse_gql_categories(const char *json,twitch_category_t *out,int max)
{
    if(!json||!out||max<0)return -1;
    json_t *root=json_parse(json),*edges=json_path(root,"data.games.edges");
    if(!edges||edges->type!=JSON_ARRAY){json_free(root);return -1;}
    int n=0;
    for(json_t *e=edges->child;e&&n<max;e=e->next){
        json_t *v=json_get(e,"node");
        const char *id=json_str(v,"id",""),*name=json_str(v,"name","");
        const char *display=json_str(v,"displayName",name);
        if(!*id||strlen(id)>=sizeof(out[n].id)||strlen(id)!=strspn(id,"0123456789")||!*name)continue;
        twitch_category_t *g=&out[n++];memset(g,0,sizeof(*g));
        copy_str(g->id,sizeof(g->id),id);copy_str(g->name,sizeof(g->name),*display?display:name);
        copy_str(g->slug,sizeof(g->slug),json_str(v,"slug",""));
        category_image_url(json_str(v,"boxArtURL",""),g->art,sizeof(g->art),144,192);
        if(!g->art[0])twitch_category_cover(name,g->art,sizeof(g->art));
    }
    json_free(root);return n;
}
int twitch_parse_categories(const char *json,twitch_category_t *out,int max)
{
    if(!json||!out||max<0)return -1;
    json_t *root=json_parse(json),*data=json_get(root,"data");
    int n=0;
    if(!data||data->type!=JSON_ARRAY){json_free(root);return -1;}
    for(json_t *v=data->child;v&&n<max;v=v->next){
        const char *id=json_str(v,"id",""),*name=json_str(v,"name","");
        if(!*id||strlen(id)>=sizeof(out[n].id)||strlen(id)!=strspn(id,"0123456789")||!*name)continue;
        twitch_category_t *g=&out[n++];memset(g,0,sizeof(*g));
        copy_str(g->id,sizeof(g->id),id);copy_str(g->name,sizeof(g->name),name);
        category_image_url(json_str(v,"box_art_url",""),g->art,sizeof(g->art),144,192);
        if(!g->art[0])twitch_category_cover(name,g->art,sizeof(g->art));
    }
    json_free(root);return n;
}
int twitch_parse_category_streams(const char *json,twitch_stream_t *out,int max)
{
    if(!json||!out||max<0)return -1;
    json_t *root=json_parse(json),*data=json_get(root,"data");
    int n=0;
    if(!data||data->type!=JSON_ARRAY){json_free(root);return -1;}
    for(json_t *v=data->child;v&&n<max;v=v->next){
        const char *login=json_str(v,"user_login","");char url[96];
        if(twitch_channel_url(login,url,sizeof(url))<0)continue;
        twitch_stream_t *s=&out[n++];memset(s,0,sizeof(*s));s->live=1;
        copy_str(s->login,sizeof(s->login),login);copy_str(s->display,sizeof(s->display),json_str(v,"user_name",login));
        copy_str(s->title,sizeof(s->title),json_str(v,"title",""));copy_str(s->game,sizeof(s->game),json_str(v,"game_name",""));
        double count=json_num(v,"viewer_count",0);s->viewers=count>=0&&count<=2147483647.0?(int)count:0;
        category_image_url(json_str(v,"thumbnail_url",""),s->preview,sizeof(s->preview),320,180);
    }
    json_free(root);return n;
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
    case TW_LIST_ALL_FOLLOWS: break;
    case TW_LIST_CATEGORY: edges=json_path(root,"data.game.streams.edges");break;
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
        if (kind == TW_LIST_TOP || kind == TW_LIST_CATEGORY) {
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

static int parse_follows_mode(const char *json, twitch_stream_t *out, int max, int *n,
                         char *cursor, size_t cursor_cap, int *has_next, char *err, size_t errlen,int all)
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
        if (!u || u->type != JSON_OBJECT || (!all&&(!st || st->type != JSON_OBJECT)) || *n >= max)
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

int twitch_parse_follows(const char *json,twitch_stream_t *out,int max,int *n,char *cursor,size_t cursor_cap,int *has_next,char *err,size_t errlen){
    return parse_follows_mode(json,out,max,n,cursor,cursor_cap,has_next,err,errlen,0);
}
int twitch_parse_follows_all(const char *json,twitch_stream_t *out,int max,int *n,char *cursor,size_t cursor_cap,int *has_next,char *err,size_t errlen){
    return parse_follows_mode(json,out,max,n,cursor,cursor_cap,has_next,err,errlen,1);
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

/* Authenticated GraphQL requests: verified TLS only, no redirect, no automatic
 * mutation retry and no logging of credentials or response bodies. */
/* Use verified TLS for all new account operations. Do not bypass integrity
 * checks or retry mutations. Log only fixed categories and HTTP/curl codes. */
static int follow_request(const char *url,const char *body,twitch_follow_job_t *j,
                          int helix,buf_t *resp,volatile int *abort_flag)
{
    if(!j->oauth[0]||strlen(j->oauth)>100||strpbrk(j->oauth,"\r\n")){j->status=6;return -1;}
    CURL *c=http_new();if(!c){j->status=8;return -1;}
    if(vt_tls_configure(c)!=CURLE_OK){http_free(c);j->status=7;LOGW("follow: TLS configuration failed");return -1;}
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,10L);
    char auth[160];snprintf(auth,sizeof(auth),"Authorization: %s %s",helix?"Bearer":"OAuth",j->oauth);
    struct curl_slist *h=NULL,*next;
    const char *headers[]={"Client-ID: " TWITCH_CLIENT_ID,"Content-Type: text/plain;charset=UTF-8",auth};
    int rc=-1;long status=0;
    for(int i=0;i<3;i++){
        next=curl_slist_append(h,headers[i]);if(!next){j->status=8;goto done;}h=next;
    }
    int net=http_request(c,url,body,h,resp,&status,abort_flag);
    LOGI("follow: %s %s http=%ld curl=%d",helix?"helix":"gql",body&&strstr(body,"mutation")?"mutation":"read",status,net);
    if(net<0){
        j->status=(net==-(int)CURLE_SSL_CONNECT_ERROR||net==-(int)CURLE_PEER_FAILED_VERIFICATION||net==-(int)CURLE_SSL_CACERT_BADFILE)?7:8;
    }else if(status==401||status==403)j->status=6;
    else if(status!=200)j->status=8;
    else{
        json_t *root=resp->data?json_parse((const char*)resp->data):NULL;
        const char *msg=json_str(root,"errors.0.message","");
        const char *challenge=json_str(root,"extensions.challenge.type","");
        if(strstr(msg,"integrity")||!strcmp(challenge,"integrity"))j->status=5;
        else if(strstr(msg,"unauthorized")||strstr(msg,"Unauthorized")||strstr(msg,"authentication"))j->status=6;
        else if(!root||json_get(root,"errors")||json_get(root,"error"))j->status=9;
        else rc=0;
        json_free(root);
    }
    if(rc<0)LOGW("follow: failure category=%d",j->status);
done:
    curl_slist_free_all(h);http_free(c);return rc;
}
static int numeric_id(const char *id)
{
    return id&&*id&&strlen(id)<32&&strlen(id)==strspn(id,"0123456789");
}
static int follow_read_helix(twitch_follow_job_t *j,volatile int *abort_flag)
{
    char url[192];
    if(!numeric_id(j->user_id)){j->status=6;return -1;}
    if(!numeric_id(j->id)){
        char check[96];if(twitch_channel_url(j->login,check,sizeof(check))<0){j->status=9;return -1;}
        snprintf(url,sizeof(url),"https://api.twitch.tv/helix/users?login=%s",j->login);
        buf_t resp={0};
        int rc=follow_request(url,NULL,j,1,&resp,abort_flag);
        if(rc==0){
            json_t *root=json_parse((const char*)resp.data);
            const char *id=json_str(root,"data.0.id","");
            if(numeric_id(id))snprintf(j->id,sizeof(j->id),"%s",id);
            else {j->status=9;rc=-1;}
            json_free(root);
        }
        buf_free(&resp);if(rc<0)return -1;
    }
    snprintf(url,sizeof(url),"https://api.twitch.tv/helix/channels/followed?user_id=%s&broadcaster_id=%s",j->user_id,j->id);
    buf_t resp={0};int rc=follow_request(url,NULL,j,1,&resp,abort_flag);
    if(rc==0){
        j->state=twitch_parse_helix_follow((const char*)resp.data,j->id);
        if(j->state<0){j->status=9;rc=-1;}
    }
    buf_free(&resp);return rc;
}
static int follow_read(twitch_follow_job_t *j,volatile int *abort_flag)
{
    char url[96],body[512];
    j->state=-1;
    if(follow_read_helix(j,abort_flag)==0)return 0;
    if(abort_flag&&*abort_flag)return -1;
    if(twitch_channel_url(j->login,url,sizeof(url))<0){j->status=9;return -1;}
    snprintf(body,sizeof(body),"{\"query\":\"query { user(login: \\\"%s\\\") { id self { canFollow follower { __typename } } } }\"}",j->login);
    buf_t resp={0};
    if(follow_request("https://gql.twitch.tv/gql",body,j,0,&resp,abort_flag)==0){
        j->state=twitch_parse_follow_state((const char*)resp.data,j->id,sizeof(j->id));
        if(j->state<0)j->status=9;
    }
    buf_free(&resp);return j->state>=0?0:-1;
}
static void follow_metadata(twitch_follow_job_t *j,volatile int *abort_flag)
{
    if(!numeric_id(j->id))return;
    j->followers=-1;j->started_at[0]=j->created_at[0]=j->avatar[0]=0;
    const char *paths[]={"users?id=","channels/followers?first=1&broadcaster_id=","streams?user_id="};
    for(int i=0;i<3&&!(abort_flag&&*abort_flag);i++){
        char url[160];snprintf(url,sizeof(url),"https://api.twitch.tv/helix/%s%s",paths[i],j->id);
        buf_t b={0};if(follow_request(url,NULL,j,1,&b,abort_flag)==0&&b.data){
            json_t *root=json_parse((const char*)b.data);
            if(i==0){copy_str(j->created_at,sizeof(j->created_at),json_str(root,"data.0.created_at",""));copy_str(j->avatar,sizeof(j->avatar),json_str(root,"data.0.profile_image_url",""));}
            else if(i==1){double n=json_num(root,"total",-1);if(n>=0&&n<=2147483647.)j->followers=(int)n;}
            else copy_str(j->started_at,sizeof(j->started_at),json_str(root,"data.0.started_at",""));
            json_free(root);
        }buf_free(&b);
    }
}
int twitch_follow_run(void *arg,volatile int *abort_flag)
{
    twitch_follow_job_t *j=arg;j->status=4;
    if(follow_read(j,abort_flag)<0)return -1;
    if(j->desired<0||j->state==j->desired){if(j->desired<0)follow_metadata(j,abort_flag);j->status=3;return 0;}
    if(j->state==2||j->desired>1||(abort_flag&&*abort_flag)){j->status=10;return -1;}
    char body[512];
    if(j->desired==1)
        snprintf(body,sizeof(body),"{\"query\":\"mutation { followUser(input: {targetID: \\\"%s\\\", disableNotifications: true}) { follow { __typename } } }\"}",j->id);
    else
        snprintf(body,sizeof(body),"{\"query\":\"mutation { unfollowUser(input: {targetID: \\\"%s\\\"}) { follow { __typename } } }\"}",j->id);
    buf_t resp={0};
    int rc=follow_request("https://gql.twitch.tv/gql",body,j,0,&resp,abort_flag);
    int failure=rc<0?j->status:10;
    buf_free(&resp);
    /* A single mutation, followed by independent read-only verification.
       If the state is already correct, trust the verified account state. */
    int verified=follow_read(j,abort_flag);
    if(verified==0&&j->state==j->desired){j->status=3;return 0;}
    j->status=failure;
    LOGW("follow: requested=%d verified=%d category=%d",j->desired,j->state,j->status);
    return -1;
}

int twitch_fetch_channel_bio(const char *login,char *bio,size_t cap,volatile int *abort_flag)
{
    char url[96],body[384],err[128];
    if(!bio||!cap)return -1;
    bio[0]=0;
    if(twitch_channel_url(login,url,sizeof(url))<0)return -1;
    int n=snprintf(body,sizeof(body),
        "{\"query\":\"query { user(login: \\\"%s\\\") { description } }\"}",login);
    if(n<0||(size_t)n>=sizeof(body))return -1;
    buf_t resp={0};
    int rc=twitch_gql(body,NULL,&resp,err,sizeof(err),abort_flag);
    if(rc==0)rc=twitch_parse_channel_bio((const char *)resp.data,bio,cap);
    buf_free(&resp);return rc;
}

int twitch_fetch_categories(const char *oauth,twitch_category_t *out,int max,volatile int *abort_flag)
{
    if(!out||max<=0)return -1;
    buf_t resp={0};int n=-1;
    if(oauth&&*oauth){
        twitch_follow_job_t auth={0};snprintf(auth.oauth,sizeof(auth.oauth),"%s",oauth);
        if(follow_request("https://api.twitch.tv/helix/games/top?first=60",NULL,&auth,1,&resp,abort_flag)==0)
            n=twitch_parse_categories((const char*)resp.data,out,max);
        buf_free(&resp);
    }
    if(n>0)return n;
    /* Public directory, independent of the regional live stream list.
       Read only; no account token in the anonymous compatibility path. */
    const char *bodies[]={
        "{\"query\":\"query { games(first: 60) { edges { node { id name displayName slug boxArtURL(width:144,height:192) } } } }\"}",
        "{\"query\":\"query { games(first: 60) { edges { node { id name displayName slug } } } }\"}"
    };
    char err[160];
    for(int i=0;i<2&&n<=0&&!(abort_flag&&*abort_flag);i++){
        buf_t b={0};
        if(twitch_gql(bodies[i],NULL,&b,err,sizeof(err),abort_flag)==0)
            n=twitch_parse_gql_categories((const char*)b.data,out,max);
        buf_free(&b);
        LOGI("categories: anonymous directory attempt=%d count=%d",i+1,n);
    }
    return n;
}
static int fetch_category_streams(const char *query,const char *oauth,twitch_stream_t *out,int max,volatile int *abort_flag)
{
    if(!query)return -1;
    char id[32],language[16]="",url[192],region[16]="",slug[128]="";
    size_t k=strcspn(query,"|");
    if(!k||k>=sizeof(id))return -1;
    memcpy(id,query,k);id[k]=0;
    if(!numeric_id(id))return -1;
    if(query[k]=='|'){
        const char *s=query+k+1;size_t r=strcspn(s,"|");
        if(r>=sizeof(region))return -1;
        memcpy(region,s,r);region[r]=0;
        if(s[r]=='|')snprintf(slug,sizeof(slug),"%s",s+r+1);
    }
    static const char *const valid[]={"PT","EN","ES","FR","DE","IT","RU","JA","KO","ZH","ID","TL","NL","PL","TR","UK","SV","AR","TH","VI","OTHER","ZH_HK"};
    for(unsigned i=0;i<sizeof(valid)/sizeof(valid[0]);i++)if(!strcmp(region,valid[i])){
        snprintf(language,sizeof(language),"%s",!strcmp(region,"ZH_HK")?"zh-hk":region);
        for(char *p=language;*p;p++)*p=(char)tolower((unsigned char)*p);
        break;
    }
    int n=-1;
    if(oauth&&*oauth){
        snprintf(url,sizeof(url),"https://api.twitch.tv/helix/streams?first=60&game_id=%s%s%s",id,language[0]?"&language=":"",language);
        twitch_follow_job_t auth={0};snprintf(auth.oauth,sizeof(auth.oauth),"%s",oauth);
        buf_t resp={0};
        if(follow_request(url,NULL,&auth,1,&resp,abort_flag)==0)n=twitch_parse_category_streams((const char*)resp.data,out,max);
        buf_free(&resp);
    }
    if(n>=0||(abort_flag&&*abort_flag))return n;
    char body[1024],err[160],escaped[256],filter[64]="";
    if(language[0])snprintf(filter,sizeof(filter),", options: {languages: [%s]}",region);
    if(slug[0]&&twitch_json_escape(slug,escaped,sizeof(escaped))>=0)
        snprintf(body,sizeof(body),"{\"query\":\"query G($slug: String!) { game(slug: $slug) { streams(first: 60, sort: VIEWER_COUNT%s) { edges { node { " STREAM_FIELDS " broadcaster { login displayName profileImageURL(width:70) } } } } } }\",\"variables\":{\"slug\":\"%s\"}}",filter,escaped);
    else
        snprintf(body,sizeof(body),"{\"query\":\"query { game(id: \\\"%s\\\") { streams(first: 60, sort: VIEWER_COUNT%s) { edges { node { " STREAM_FIELDS " broadcaster { login displayName profileImageURL(width:70) } } } } } }\"}",id,filter);
    buf_t resp={0};
    if(twitch_gql(body,NULL,&resp,err,sizeof(err),abort_flag)==0)n=twitch_parse_list((const char*)resp.data,TW_LIST_CATEGORY,out,max,err,sizeof(err));
    buf_free(&resp);return n;
}

int twitch_fetch_list(twitch_list_t kind, const char *query, const char *oauth,
                      twitch_stream_t *out, int max, char *err, size_t errlen,
                      volatile int *abort_flag)
{
    char body[1024];
    buf_t resp = {0};
    int n;
    if(kind==TW_LIST_ALL_FOLLOWS){
        if(!oauth||!*oauth)return -2;
        char cursor[256]="",request[1536];int count=0,more=1,rc=0;
        for(int page=0;page<20&&more&&count<max&&!(abort_flag&&*abort_flag);page++){
            if(twitch_build_follows_body(cursor,request,sizeof(request))<0||twitch_gql(request,oauth,&resp,err,errlen,abort_flag)<0){rc=-1;break;}
            rc=twitch_parse_follows_all((const char*)resp.data,out,max,&count,cursor,sizeof(cursor),&more,err,errlen);
            if(rc<0)break;
        }
        buf_free(&resp);
        if(rc<0&&!count)return rc;
        twitch_sort_by_viewers(out,count);return count;
    }
    if(kind==TW_LIST_CATEGORY){
        int r=fetch_category_streams(query,oauth,out,max,abort_flag);
        if(r<0)snprintf(err,errlen,vt_lang_current()==2?"Não foi possível carregar a categoria.":"Could not load category.");
        return r;
    }
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

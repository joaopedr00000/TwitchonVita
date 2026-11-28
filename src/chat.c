#include "i18n.h"
#include "chat.h"
#include "json.h"
#include "log.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ IRC */

int irc_parse(const char *line, irc_line_t *out)
{
    const char *p = line;
    memset(out, 0, sizeof(*out));

    if (*p == '@') {
        const char *e = strchr(p, ' ');
        if (!e)
            return -1;
        size_t n = (size_t)(e - p - 1);
        if (n >= sizeof(out->tags)) n = sizeof(out->tags) - 1;
        memcpy(out->tags, p + 1, n);
        p = e + 1;
    }
    while (*p == ' ') p++;
    if (*p == ':') {
        const char *e = strchr(p, ' ');
        if (!e)
            return -1;
        size_t n = (size_t)(e - p - 1);
        if (n >= sizeof(out->prefix)) n = sizeof(out->prefix) - 1;
        memcpy(out->prefix, p + 1, n);
        p = e + 1;
    }
    while (*p == ' ') p++;
    size_t n = 0;
    while (*p && *p != ' ' && n + 1 < sizeof(out->command))
        out->command[n++] = *p++;
    out->command[n] = 0;
    if (!n)
        return -1;
    while (*p && *p != ' ') p++;

    n = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (*p == ':') {
            snprintf(out->trailing, sizeof(out->trailing), "%s", p + 1);
            out->has_trailing = 1;
            break;
        }
        while (*p && *p != ' ') {
            if (n + 1 < sizeof(out->params))
                out->params[n++] = *p;
            p++;
        }
        if (*p && n + 1 < sizeof(out->params))
            out->params[n++] = ' ';
    }
    while (n && out->params[n - 1] == ' ') n--;
    out->params[n] = 0;
    return 0;
}

int irc_tag(const irc_line_t *l, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = l->tags;
    if (cap) out[0] = 0;
    while (*p) {
        const char *end = strchr(p, ';');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) >= kl && !strncmp(p, key, kl) && (p[kl] == '=' || p + kl == end)) {
            const char *v = p[kl] == '=' ? p + kl + 1 : end;
            size_t n = 0;
            while (v < end && n + 1 < cap) {
                char c = *v++;
                if (c == '\\' && v < end) {
                    char e = *v++;
                    c = e == 's' ? ' ' : e == ':' ? ';' : e == 'r' ? '\r' : e == 'n' ? '\n' : e;
                }
                out[n++] = c;
            }
            if (cap) out[n] = 0;
            return 0;
        }
        p = *end ? end + 1 : end;
    }
    return -1;
}

/* Chat outgoing validation is kept host-testable (no network dependencies).
 * Reject CR/LF/CTRL framing injection and messages longer than 450 UTF-8 bytes. */
int chat_prepare_outgoing(const char *in,char *out,size_t cap){
    if(!in||!out||cap<2)return -1;
    const unsigned char *p=(const unsigned char*)in;
    while(*p==' '||*p=='\t')p++;
    size_t n=strlen((const char*)p);
    while(n&&((char)p[n-1]==' '||(char)p[n-1]=='\t'))n--;
    if(n==0||n>450||n>=cap)return -1;
    for(size_t i=0;i<n;i++)if(p[i]<0x20||p[i]==0x7f)return -1;
    memcpy(out,p,n);out[n]=0;
    return (int)n;
}

/* Parse the documented Helix response, independent of Vita networking. */
/* A verified /validate HTTP 200 establishes current validity. expires_in
 * is informational; a missing/zero lifetime must not override that status. */
int chat_validate_identity(const char *body,const char *client,const char *sender,char *reason,size_t cap)
{
    json_t *r=json_parse(body);
    const char *cid=json_str(r,"client_id","");
    const char *uid=json_str(r,"user_id","");
    json_t *scopes=json_get(r,"scopes");
    int write=0,client_ok=client&&*client&&!strcmp(cid,client),user_ok=sender&&*sender&&!strcmp(uid,sender);
    for(int i=0;scopes&&scopes->type==JSON_ARRAY&&i<scopes->count;i++)
        if(!strcmp(json_str(json_at(scopes,i),NULL,""),"user:write:chat"))write=1;
    double expiry=json_num(r,"expires_in",-1);
    LOGI("chat: validate 200 client_match=%d user_match=%d write_scope=%d expires_in=%.0f",client_ok,user_ok,write,expiry);
    json_free(r);
    snprintf(reason,cap,"%s",!client_ok?"CLIENT ID":!user_ok?"USER ID":!write?"SCOPE user:write:chat":"");
    return client_ok&&user_ok&&write?0:-1;
}

int chat_parse_send_result(const char *json,char *code,size_t code_cap)
{
    json_t *root=json_parse(json);
    json_t *data=json_get(root,"data");
    json_t *result=data&&data->type==JSON_ARRAY?json_at(data,0):NULL;
    json_t *sent=json_get(result,"is_sent");
    int rc=-1;
    if(code_cap)snprintf(code,code_cap,"INVALID_RESPONSE");
    if(sent&&sent->type==JSON_BOOL){
        if(sent->num!=0){
            const char *id=json_str(result,"message_id","");
            if(*id){rc=0;if(code_cap)code[0]=0;}
        }else{
            rc=1;
            const char *why=json_str(result,"drop_reason.code","");
            /* Log only the protocol's machine-readable code, not message text. */
            if(!*why||strlen(why)>48||strspn(why,"abcdefghijklmnopqrstuvwxyz0123456789_")!=strlen(why))why="unknown_drop";
            if(code_cap)snprintf(code,code_cap,"%s",why);
        }
    }
    json_free(root);
    return rc;
}

/* ============================================================= emotes */

#define HASH_SIZE 8192 /* power of two, > 2 * CHAT_EMOTES_MAX */

static chat_emote_t *g_emotes;
static int g_nemotes;
static int16_t g_hash[HASH_SIZE]; /* emote index + 1, 0 = empty */

static uint32_t hash_word(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

void chat_emotes_reset(void)
{
    if (!g_emotes)
        g_emotes = calloc(CHAT_EMOTES_MAX, sizeof(chat_emote_t));
    g_nemotes = 0;
    memset(g_hash, 0, sizeof(g_hash));
}

int chat_emote_count(void) { return g_nemotes; }

const chat_emote_t *chat_emote(int idx)
{
    return (g_emotes && idx >= 0 && idx < g_nemotes) ? &g_emotes[idx] : NULL;
}

/* slot in the hash table for this key (existing entry or the empty slot to use) */
static int hash_slot(const char *key, size_t len)
{
    uint32_t h = hash_word(key, len) & (HASH_SIZE - 1);
    for (int probe = 0; probe < HASH_SIZE; probe++) {
        int16_t v = g_hash[h];
        if (v == 0)
            return (int)h;
        const chat_emote_t *e = &g_emotes[v - 1];
        if (strlen(e->name) == len && !memcmp(e->name, key, len))
            return (int)h;
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return -1;
}

int chat_find_emote(const char *word, size_t len)
{
    if (!g_emotes || !len || len >= sizeof(g_emotes[0].name))
        return -1;
    int s = hash_slot(word, len);
    return (s >= 0 && g_hash[s]) ? g_hash[s] - 1 : -1;
}

/* adds or replaces (channel emotes override global ones with the same name) */
static int add_emote(const char *key, const char *url, int w, int h)
{
    size_t len = strlen(key);
    if (!g_emotes || !len || len >= sizeof(g_emotes[0].name) || strlen(url) >= sizeof(g_emotes[0].url))
        return -1;
    int s = hash_slot(key, len);
    if (s < 0)
        return -1;
    /* always append (an existing entry may be read by the drawing thread right
     * now); a replaced name just points the hash slot at the new entry */
    if (g_nemotes >= CHAT_EMOTES_MAX)
        return -1;
    int idx = g_nemotes;
    chat_emote_t *e = &g_emotes[idx];
    snprintf(e->name, sizeof(e->name), "%s", key);
    snprintf(e->url, sizeof(e->url), "%s", url);
    e->w = (uint8_t)(w > 0 && w < 255 ? w : 28);
    e->h = (uint8_t)(h > 0 && h < 255 ? h : 28);
    g_nemotes = idx + 1;
    g_hash[s] = (int16_t)(idx + 1);
    return idx;
}

/* Twitch emotes are keyed by "\x01<id>" so they never match a chat word. */
static int twitch_emote(const char *id)
{
    char key[48], url[128];
    if (strlen(id) > 40)
        return -1;
    snprintf(key, sizeof(key), "\x01%s", id);
    int idx = chat_find_emote(key, strlen(key));
    if (idx >= 0)
        return idx;
    snprintf(url, sizeof(url), "https://static-cdn.jtvnw.net/emoticons/v2/%s/static/dark/1.0", id);
    return add_emote(key, url, 28, 28);
}

int chat_load_7tv(const char *json)
{
    json_t *root = json_parse(json);
    if (!root)
        return -1;
    /* /v3/users/twitch/<id> wraps the set in "emote_set"; the global set is bare */
    json_t *list = json_path(root, "emote_set.emotes");
    if (!list)
        list = json_get(root, "emotes");
    int n = 0;
    for (json_t *e = list ? list->child : NULL; e; e = e->next) {
        const char *name = json_str(e, "name", NULL);
        const char *id = json_str(e, "id", NULL);
        if (!name || !id || strlen(id) > 40)
            continue;
        int animated = json_num(e, "data.animated", 0) != 0;
        int w = 32, h = 32;
        json_t *files = json_path(e, "data.host.files");
        for (json_t *f = files ? files->child : NULL; f; f = f->next) {
            const char *fn = json_str(f, "name", "");
            if (!strncmp(fn, "1x.", 3)) {
                w = (int)json_num(f, "width", 32);
                h = (int)json_num(f, "height", 32);
                break;
            }
        }
        char url[128];
        /* the CDN has PNG for static emotes and a PNG first frame for animated ones */
        snprintf(url, sizeof(url), "https://cdn.7tv.app/emote/%s/%s", id, animated ? "1x_static.png" : "1x.png");
        if (add_emote(name, url, w, h) >= 0)
            n++;
    }
    json_free(root);
    return n;
}

int chat_load_badges(const char *json)
{
    json_t *root = json_parse(json);
    if (!root)
        return -1;
    json_t *list = json_path(root, "data.badges");
    if (!list)
        list = json_path(root, "data.user.broadcastBadges");
    int n = 0;
    for (json_t *b = list ? list->child : NULL; b; b = b->next) {
        const char *set = json_str(b, "setID", NULL);
        const char *ver = json_str(b, "version", NULL);
        const char *url = json_str(b, "imageURL", NULL);
        if (!set || !ver || !url)
            continue;
        char key[96];
        if ((size_t)snprintf(key, sizeof(key), "\x02%s/%s", set, ver) >= sizeof(key))
            continue;
        if (add_emote(key, url, 18, 18) >= 0)
            n++;
    }
    json_free(root);
    return n;
}

static int find_badge(const char *set, size_t set_len, const char *ver, size_t ver_len)
{
    char key[96];
    if (set_len + ver_len + 3 > sizeof(key))
        return -1;
    int n = snprintf(key, sizeof(key), "\x02%.*s/%.*s", (int)set_len, set, (int)ver_len, ver);
    return chat_find_emote(key, (size_t)n);
}

int chat_resolve_badges(const char *tag, int16_t *out, int max)
{
    int n = 0;
    const char *p = tag;
    while (p && *p && n < max) {
        const char *end = strchr(p, ',');
        if (!end)
            end = p + strlen(p);
        const char *slash = memchr(p, '/', (size_t)(end - p));
        if (slash) {
            size_t sl = (size_t)(slash - p);
            int idx = find_badge(p, sl, slash + 1, (size_t)(end - slash - 1));
            /* subscriber/bits versions missing from the channel set: use the
             * generic badge of the same kind */
            if (idx < 0)
                idx = find_badge(p, sl, "1", 1);
            if (idx < 0)
                idx = find_badge(p, sl, "0", 1);
            if (idx >= 0)
                out[n++] = (int16_t)idx;
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

/* =========================================================== messages */

static uint32_t name_color(const char *hex, const char *name)
{
    static const uint32_t palette[] = {
        0xFF6B6BFF, 0xFF4FA3FF, 0xFFB86CFF, 0xFF9C7CFF, 0xFFE07BFF, 0xFF6BD1FF, 0xFF8CE38C, 0xFFF0C674,
    };
    unsigned r, g, b;
    if (hex && hex[0] == '#' && sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
        /* keep names readable on the dark panel */
        unsigned lum = (r * 299 + g * 587 + b * 114) / 1000;
        if (lum < 110) {
            r = (r + 255) / 2;
            g = (g + 255) / 2;
            b = (b + 255) / 2;
        }
        return 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    uint32_t h = hash_word(name, strlen(name));
    uint32_t c = palette[h % (sizeof(palette) / sizeof(palette[0]))];
    /* palette written as RRGGBBAA for readability */
    return 0xFF000000u | ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | (c >> 24);
}

typedef struct { int start, end; char id[48]; } erange_t; /* byte offsets */

/* "25:0-4,12-16/1902:6-10" (code point positions) -> byte ranges */
static int parse_emote_tag(const char *tag, const char *text, erange_t *out, int max)
{
    int cp_off[CHAT_TEXT_MAX + 1];
    int ncp = 0, len = (int)strlen(text);
    for (int i = 0; i < len && ncp < CHAT_TEXT_MAX; i++)
        if (((unsigned char)text[i] & 0xC0) != 0x80)
            cp_off[ncp++] = i;
    cp_off[ncp] = len;

    int n = 0;
    const char *p = tag;
    while (*p && n < max) {
        const char *colon = strchr(p, ':');
        if (!colon)
            break;
        char id[48];
        size_t il = (size_t)(colon - p);
        if (il >= sizeof(id)) il = sizeof(id) - 1;
        memcpy(id, p, il);
        id[il] = 0;
        p = colon + 1;
        while (*p && *p != '/' && n < max) {
            int a = 0, b = 0;
            if (sscanf(p, "%d-%d", &a, &b) == 2 && a >= 0 && b >= a && b < ncp) {
                out[n].start = cp_off[a];
                out[n].end = cp_off[b + 1];
                snprintf(out[n].id, sizeof(out[n].id), "%s", id);
                n++;
            }
            while (*p && *p != ',' && *p != '/') p++;
            if (*p == ',') p++;
        }
        if (*p == '/') p++;
    }
    return n;
}

static void tokenize(chat_msg_t *m, const char *raw, const char *emotes_tag)
{
    erange_t ranges[64];
    int nr = emotes_tag && emotes_tag[0] ? parse_emote_tag(emotes_tag, raw, ranges, 64) : 0;
    size_t out = 0;
    int len = (int)strlen(raw);
    int i = 0;

    m->ntok = 0;
    while (i < len && m->ntok < CHAT_TOK_MAX) {
        while (i < len && raw[i] == ' ') i++;
        if (i >= len)
            break;
        int s = i;
        while (i < len && raw[i] != ' ') i++;
        int e = i;
        chat_tok_t *t = &m->tok[m->ntok];

        int emote = -1;
        for (int k = 0; k < nr; k++)
            if (ranges[k].start == s && ranges[k].end == e) {
                emote = twitch_emote(ranges[k].id);
                break;
            }
        if (emote < 0)
            emote = chat_find_emote(raw + s, (size_t)(e - s));
        if (emote >= 0) {
            t->emote = (int16_t)emote;
            t->off = t->len = 0;
            m->ntok++;
            continue;
        }
        /* last token slot: keep the rest of the message as text */
        if (m->ntok == CHAT_TOK_MAX - 1)
            e = i = len;
        char word[CHAT_TEXT_MAX], clean[CHAT_TEXT_MAX];
        int wl = e - s;
        memcpy(word, raw + s, (size_t)wl);
        word[wl] = 0;
        text_clean(word, clean, sizeof(clean));
        size_t cl = strlen(clean);
        if (!cl || out + cl + 1 > sizeof(m->text))
            continue;
        memcpy(m->text + out, clean, cl);
        t->emote = -1;
        t->off = (uint16_t)out;
        t->len = (uint16_t)cl;
        out += cl;
        m->text[out++] = 0; /* each word NUL-terminated: drawn directly */
        m->ntok++;
    }
}

int chat_build_msg(const irc_line_t *l, chat_msg_t *m)
{
    char display[64], color[16], emotes[1024], login[48], sys[512];
    int is_notice = !strcmp(l->command, "USERNOTICE");

    if (strcmp(l->command, "PRIVMSG") && !is_notice)
        return -1;
    memset(m, 0, sizeof(*m));

    /* login from the prefix "nick!nick@nick.tmi.twitch.tv" */
    size_t n = 0;
    while (l->prefix[n] && l->prefix[n] != '!' && n + 1 < sizeof(login)) {
        login[n] = l->prefix[n];
        n++;
    }
    login[n] = 0;
    irc_tag(l, "display-name", display, sizeof(display));
    if (!display[0] || ((unsigned char)display[0] >= 0x80))
        snprintf(display, sizeof(display), "%s", login); /* CJK names can't be drawn */
    if (is_notice) {
        irc_tag(l, "login", login, sizeof(login));
        if (!display[0])
            snprintf(display, sizeof(display), "%s", login);
    }
    text_clean(display, m->name, sizeof(m->name));
    irc_tag(l, "color", color, sizeof(color));
    m->color = name_color(color, m->name);
    {
        char badges[256];
        irc_tag(l, "badges", badges, sizeof(badges));
        m->nbadge = chat_resolve_badges(badges, m->badge, CHAT_BADGES_MAX);
    }
    irc_tag(l, "emotes", emotes, sizeof(emotes));

    char raw[CHAT_TEXT_MAX];
    size_t rl0 = strlen(l->trailing);
    if (rl0 >= sizeof(raw)) {
        rl0 = sizeof(raw) - 1;
        while (rl0 > 0 && ((unsigned char)l->trailing[rl0] & 0xC0) == 0x80)
            rl0--; /* don't cut a UTF-8 character in half */
    }
    memcpy(raw, l->trailing, rl0);
    raw[rl0] = 0;
    /* /me: "\x01ACTION text\x01" */
    if (!strncmp(raw, "\x01" "ACTION ", 8)) {
        size_t rl = strlen(raw);
        if (rl && raw[rl - 1] == '\x01')
            raw[--rl] = 0;
        memmove(raw, raw + 8, strlen(raw + 8) + 1);
        m->action = 1;
        /* emote positions refer to the text without the ACTION wrapper */
    }

    if (is_notice) {
        m->system = 1;
        irc_tag(l, "system-msg", sys, sizeof(sys));
        if (!sys[0] && !raw[0])
            return -1;
        if (raw[0]) {
            /* sub message with text: "<system msg> | <text>" keeps both visible */
            char both[CHAT_TEXT_MAX];
            snprintf(both, sizeof(both), "%s", sys);
            tokenize(m, both, NULL);
            /* append the user's words (emote positions refer to raw only) */
            chat_msg_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            tokenize(&tmp, raw, emotes);
            size_t used = 0;
            for (int i = 0; i < m->ntok; i++)
                if (m->tok[i].emote < 0 && (size_t)(m->tok[i].off + m->tok[i].len + 1) > used)
                    used = (size_t)(m->tok[i].off + m->tok[i].len + 1);
            for (int i = 0; i < tmp.ntok && m->ntok < CHAT_TOK_MAX; i++) {
                chat_tok_t t = tmp.tok[i];
                if (t.emote < 0) {
                    if (used + t.len + 1 > sizeof(m->text))
                        break;
                    memcpy(m->text + used, tmp.text + t.off, t.len + 1u);
                    t.off = (uint16_t)used;
                    used += t.len + 1u;
                }
                m->tok[m->ntok++] = t;
            }
        } else {
            tokenize(m, sys, NULL);
        }
        return m->ntok > 0 ? 0 : -1;
    }

    tokenize(m, raw, emotes);
    return m->ntok > 0 ? 0 : -1;
}

/* ========================================================= live chat */
#ifndef VT_NO_NET

#include "http.h"
#include "tls_trust.h"
#include "twitch.h"
#include <pthread.h>
#include <psp2/net/http.h>
#include <psp2/libssl.h>
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>

static struct {
    pthread_mutex_t lock;
    int lock_ok;
    chat_msg_t *msgs;          /* ring buffer */
    int head, count;
    volatile unsigned version; /* bumped on every new message */
    char status[96];
    char login[32]; /* channel */
    char nick[32],oauth[64],sender_id[24],outgoing[CHAT_TEXT_MAX],pending[CHAT_TEXT_MAX];
    int writable,queued,awaiting,read_only_fallback,retry_auth;
    char fallback_code[24];
    unsigned long long last_send,sent_at;
    volatile int running;
    volatile int stop;         /* curl abort flag */
    volatile int alive;
    pthread_t thread;
    int thread_started;
} C;

static void set_status(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&C.lock);
    if(C.read_only_fallback)snprintf(C.status,sizeof(C.status),"%s [%s]",vt_msg("Secure chat TLS unavailable; anonymous read-only"),C.fallback_code);
    else snprintf(C.status,sizeof(C.status),"%s",buf);
    pthread_mutex_unlock(&C.lock);
}

static void push_msg(const chat_msg_t *m)
{
    pthread_mutex_lock(&C.lock);
    C.msgs[C.head] = *m;
    C.head = (C.head + 1) % CHAT_MSGS;
    if (C.count < CHAT_MSGS)
        C.count++;
    C.version++;
    pthread_mutex_unlock(&C.lock);
}

unsigned chat_version(void) { return C.version; }

static void push_system(const char *text)
{
    static chat_msg_t m;
    memset(&m, 0, sizeof(m));
    m.system = 1;
    tokenize(&m, text, NULL);
    if (m.ntok)
        push_msg(&m);
}

static int irc_send(CURL *c, const char *s)
{
    size_t len = strlen(s), off = 0;
    for (int tries = 0; off < len && tries < 200 && !C.stop; tries++) {
        size_t sent = 0;
        CURLcode r = curl_easy_send(c, s + off, len - off, &sent);
        if (r == CURLE_AGAIN) {
            sceKernelDelayThread(10000);
            continue;
        }
        if (r != CURLE_OK)
            return -1;
        off += sent;
    }
    return off == len ? 0 : -1;
}

static void load_badges(const char *room_id)
{
    static const char global_q[] =
        "{\"query\":\"query { badges { setID version imageURL(size: NORMAL) } }\"}";
    char body[256], err[128];
    buf_t b = {0};
    int g = -1, c = -1;

    for (const char *p = room_id; *p; p++)
        if (*p < '0' || *p > '9')
            return; /* room-id is numeric */
    set_status(vt_msg("Cargando insignias..."));
    if (twitch_gql(global_q, NULL, &b, err, sizeof(err), &C.stop) == 0)
        g = chat_load_badges((const char *)b.data);
    snprintf(body, sizeof(body),
             "{\"query\":\"query { user(id: \\\"%s\\\") { broadcastBadges { setID version imageURL(size: NORMAL) } } }\"}",
             room_id);
    if (!C.stop && twitch_gql(body, NULL, &b, err, sizeof(err), &C.stop) == 0)
        c = chat_load_badges((const char *)b.data);
    LOGI("chat: insignias %d globales, %d del canal", g, c);
    buf_free(&b);
}

static void load_7tv(const char *room_id)
{
    CURL *c = http_new();
    buf_t b = {0};
    long status = 0;
    char url[96];
    int total = 0, chan = -1;

    if (!c)
        return;
    set_status(vt_msg("Cargando emotes de 7TV..."));
    if (http_request(c, "https://7tv.io/v3/emote-sets/global", NULL, NULL, &b, &status, &C.stop) == 0 && status == 200) {
        int n = chat_load_7tv((const char *)b.data);
        if (n > 0) total += n;
        LOGI("chat: 7TV globales %d", n);
    }
    snprintf(url, sizeof(url), "https://7tv.io/v3/users/twitch/%s", room_id);
    if (!C.stop && http_request(c, url, NULL, NULL, &b, &status, &C.stop) == 0) {
        if (status == 200) {
            chan = chat_load_7tv((const char *)b.data);
            if (chan > 0) total += chan;
            LOGI("chat: 7TV del canal %d (%u bytes)", chan, (unsigned)b.size);
        } else {
            LOGI("chat: el canal no usa 7TV (HTTP %ld)", status);
        }
    }
    buf_free(&b);
    http_free(c);
    if (chan > 0)
        set_status(vt_msg("Conectado - 7TV: %d emotes del canal"), chan);
    else
        set_status(total > 0 ? vt_msg("Conectado - 7TV: solo globales") : vt_msg("Conectado"));
}

static int abort_cb(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)p; (void)a; (void)b; (void)c; (void)d;
    return C.stop ? 1 : 0;
}

/* Sony HTTPS is the transport intercepted by vita_tls_fixer's System provider.
 * Never send account credentials through the legacy anonymous IRC connection. */
#define CHAT_VERIFY_FLAGS (SCE_HTTPS_FLAG_SERVER_VERIFY | SCE_HTTPS_FLAG_CN_CHECK | SCE_HTTPS_FLAG_NOT_AFTER_CHECK | SCE_HTTPS_FLAG_NOT_BEFORE_CHECK | SCE_HTTPS_FLAG_KNOWN_CA_CHECK)
static int numeric_id(const char *s){return s&&*s&&strlen(s)<24&&strspn(s,"0123456789")==strlen(s);}
static int native_helix(const char *url,const char *body,char *out,size_t cap,int *status,int validate)
{
    int tpl=-1,conn=-1,req=-1,rc=-1;char auth[96]={0};size_t used=0;
    *status=0;out[0]=0;
    if(C.stop)return -1;
    snprintf(auth,sizeof(auth),"Bearer %s",C.oauth);
#define CHAT_CHECK(expr) do{rc=(expr);if(rc<0)goto done;}while(0)
    CHAT_CHECK(tpl=sceHttpCreateTemplate("VitaTwitch/1.33",SCE_HTTP_VERSION_1_1,0));
    CHAT_CHECK(sceHttpSetResolveTimeOut(tpl,3000000));
    CHAT_CHECK(sceHttpSetResolveRetry(tpl,1));
    CHAT_CHECK(sceHttpSetConnectTimeOut(tpl,5000000));
    CHAT_CHECK(sceHttpSetSendTimeOut(tpl,5000000));
    CHAT_CHECK(sceHttpSetRecvTimeOut(tpl,5000000));
    CHAT_CHECK(sceHttpSetAutoRedirect(tpl,SCE_HTTP_DISABLE));
    CHAT_CHECK(sceHttpSetCookieEnabled(tpl,SCE_HTTP_DISABLE));
    CHAT_CHECK(sceHttpSetResponseHeaderMaxSize(tpl,16384));
    CHAT_CHECK(conn=sceHttpCreateConnectionWithURL(tpl,url,SCE_HTTP_DISABLE));
    CHAT_CHECK(req=sceHttpCreateRequestWithURL(conn,body?SCE_HTTP_METHOD_POST:SCE_HTTP_METHOD_GET,url,body?strlen(body):0));
    CHAT_CHECK(sceHttpAddRequestHeader(req,"Authorization",auth,SCE_HTTP_HEADER_OVERWRITE));
    if(!validate)CHAT_CHECK(sceHttpAddRequestHeader(req,"Client-Id",TWITCH_CLIENT_ID,SCE_HTTP_HEADER_OVERWRITE));
    CHAT_CHECK(sceHttpAddRequestHeader(req,"Accept-Encoding","identity",SCE_HTTP_HEADER_OVERWRITE));
    if(body)CHAT_CHECK(sceHttpAddRequestHeader(req,"Content-Type","application/json",SCE_HTTP_HEADER_OVERWRITE));
    if(C.stop){rc=-1;goto done;}
    CHAT_CHECK(sceHttpSendRequest(req,body,body?(unsigned)strlen(body):0));
    CHAT_CHECK(sceHttpGetStatusCode(req,status));
    while(!C.stop&&used<cap-1){
        int n=sceHttpReadData(req,out+used,(unsigned)(cap-1-used));
        if(n<0){rc=n;goto done;}
        if(!n){rc=0;goto done;}
        used+=(size_t)n;out[used]=0;
    }
    rc=-1; /* truncated or cancelled response: no success assumption */
done:
    if(rc<0&&req>=0){int ssl=0;unsigned detail=0;sceHttpsGetSslError(req,&ssl,&detail);LOGW("chat: sceHttp rc=%08X ssl=%08X detail=%08X",(unsigned)rc,(unsigned)ssl,detail);}
    if(req>=0)sceHttpDeleteRequest(req);
    if(conn>=0)sceHttpDeleteConnection(conn);
    if(tpl>=0)sceHttpDeleteTemplate(tpl);
    memset(auth,0,sizeof(auth));
#undef CHAT_CHECK
    return rc;
}
static int send_native_message(const char *message,char *reason,size_t cap)
{
    /* Run on chat worker, not the player/UI. Do not retry a POST automatically. */
    char url[160],escaped[1024],payload[1200],response[4096],broadcaster[24]="";
    const char *stage="TLS";
    int ssl=0,http=0,rc=-1,status=0,ret=-1;
    if(!numeric_id(C.sender_id)||twitch_json_escape(message,escaped,sizeof(escaped))<0){snprintf(reason,cap,"AUTH");return -1;}
    if(sceSysmoduleIsLoaded(SCE_SYSMODULE_HTTPS)!=0){
        rc=sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
        if(rc<0)goto done;
    }
    rc=sceSslInit(1024*1024);if(rc<0)goto done;ssl=1;
    rc=sceHttpInit(1024*1024);if(rc<0)goto done;http=1;
    rc=sceHttpsEnableOption(CHAT_VERIFY_FLAGS);if(rc<0)goto done;
    /* Verify the actual issued scopes, not just those requested in device flow. */
    stage="VALIDATE";
    rc=native_helix("https://id.twitch.tv/oauth2/validate",NULL,response,sizeof(response),&status,1);
    if(rc<0||status!=200)goto done;
    if(chat_validate_identity(response,TWITCH_CLIENT_ID,C.sender_id,reason,cap)<0)goto cleanup;
    /* Resolve the channel using verified Helix, not unverified IRC ROOMSTATE. */
    stage="USERS";
    snprintf(url,sizeof(url),"https://api.twitch.tv/helix/users?login=%s",C.login);
    rc=native_helix(url,NULL,response,sizeof(response),&status,0);
    if(rc<0||status!=200)goto done;
    json_t *root=json_parse(response);
    json_t *user=json_at(json_get(root,"data"),0);
    const char *id=json_str(user,"id","");
    const char *login=json_str(user,"login","");
    if(numeric_id(id)&&!strcmp(login,C.login))snprintf(broadcaster,sizeof(broadcaster),"%s",id);
    json_free(root);
    if(!broadcaster[0]){snprintf(reason,cap,"CHANNEL");goto cleanup;}
    int n=snprintf(payload,sizeof(payload),"{\"broadcaster_id\":\"%s\",\"sender_id\":\"%s\",\"message\":\"%s\"}",broadcaster,C.sender_id,escaped);
    if(n<0||n>=(int)sizeof(payload)){snprintf(reason,cap,"MESSAGE");goto cleanup;}
    status=0;stage="SEND";
    rc=native_helix("https://api.twitch.tv/helix/chat/messages",payload,response,sizeof(response),&status,0);
    if(rc<0||status!=200)goto done;
    int outcome=chat_parse_send_result(response,reason,cap);
    if(outcome==0){
        LOGI("chat: Helix confirmed message_id and is_sent=true (sceHttp)");ret=0;
    }else if(outcome==1){
        LOGW("chat: Helix HTTP 200 drop_reason=%s",reason);
    }else LOGW("chat: Helix HTTP 200 unexpected response schema");
    goto cleanup;
done:
    if(rc<0)snprintf(reason,cap,"NET %08X",(unsigned)rc);
    else {
        json_t *error=json_parse(response);
        const char *message=json_str(error,"message","");
        const char *kind=strstr(message,"scope")?"SCOPE":strstr(message,"client ID")||strstr(message,"client id")||strstr(message,"Client ID")?"CLIENT":strstr(message,"sender_id")?"SENDER":strstr(message,"token")?"TOKEN":"";
        if(status==401&&!strcmp(stage,"VALIDATE"))snprintf(reason,cap,"TOKEN INVALID - LOGIN");
        else snprintf(reason,cap,"%s HTTP %d %s",stage,status,kind);
        LOGW("chat: rejected stage=%s HTTP=%d class=%s",stage,status,kind);
        json_free(error);
    }
    LOGW("chat: native HTTPS send failed stage=%s rc=%08X HTTP=%d",stage,(unsigned)rc,status);
cleanup:
    if(http)sceHttpTerm();
    if(ssl)sceSslTerm();
    return ret;
}
static void *chat_thread(void *argp)
{
    static char rbuf[16384];
    static irc_line_t line;
    static chat_msg_t msg;
    int backoff=1,emotes_done=0;
    char room_id[32]="";
    (void)argp;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),SCE_KERNEL_CPU_MASK_USER_2);
    LOGI("chat: anonymous IRC reader + native sceHttp Helix sender");
    while(C.running){
        set_status(vt_msg("Conectando al chat..."));
        CURL *c=curl_easy_init();int ok=0;
        if(c){
            curl_easy_setopt(c,CURLOPT_URL,"https://irc.chat.twitch.tv:6697");
            curl_easy_setopt(c,CURLOPT_CONNECT_ONLY,1L);
            curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);
            curl_easy_setopt(c,CURLOPT_TIMEOUT,12L);
            curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
            /* Legacy anonymous reader, no OAuth or other secret. */
            curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,0L);
            curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,0L);
            curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,abort_cb);
            curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
            CURLcode rc=curl_easy_perform(c);
            if(rc==CURLE_OK){
                char hello[160];snprintf(hello,sizeof(hello),"CAP REQ :twitch.tv/tags twitch.tv/commands\r\nPASS SCHMOOPIIE\r\nNICK justinfan%d\r\nJOIN #%s\r\n",10000+rand()%80000,C.login);
                ok=irc_send(c,hello)==0;
            }else LOGW("chat: IRC connect curl=%d",(int)rc);
        }
        size_t have=0;unsigned long long last_rx=sceKernelGetProcessTimeWide();
        int join_reported=0;
        while(ok&&C.running){
            char outgoing[CHAT_TEXT_MAX]="";
            pthread_mutex_lock(&C.lock);
            if(C.queued){snprintf(outgoing,sizeof(outgoing),"%s",C.outgoing);C.queued=0;C.awaiting=1;C.outgoing[0]=0;}
            pthread_mutex_unlock(&C.lock);
            if(outgoing[0]){
                char reason[64]="",notice[160];
                int sent=send_native_message(outgoing,reason,sizeof(reason));
                pthread_mutex_lock(&C.lock);C.awaiting=0;pthread_mutex_unlock(&C.lock);
                if(sent<0&&!C.stop){
                    const int pt=!strcmp(vt_lang_code(vt_lang_current()),"pt-BR");
                    const char *explanation=NULL;
                    if(!strcmp(reason,"automod_held"))explanation=pt?"Mensagem retida pelo AutoMod":"Message held by AutoMod";
                    else if(!strcmp(reason,"msg_duplicate"))explanation=pt?"Mensagem repetida; aguarde 30 s":"Duplicate message; wait 30 s";
                    else if(!strcmp(reason,"followers_only_mode"))explanation=pt?"Chat apenas para seguidores":"Followers-only chat";
                    else if(!strcmp(reason,"subs_only_mode"))explanation=pt?"Chat apenas para inscritos":"Subscribers-only chat";
                    else if(!strcmp(reason,"emote_only_mode"))explanation=pt?"Chat apenas para emotes":"Emote-only chat";
                    else if(!strcmp(reason,"slow_mode"))explanation=pt?"Modo lento; aguarde":"Slow mode; please wait";
                    else if(!strcmp(reason,"verified_account")||!strcmp(reason,"verified_phone_number"))explanation=pt?"Conta precisa de verificacao":"Account verification required";
                    else if(!strcmp(reason,"user_banned")||!strcmp(reason,"user_timed_out")||!strcmp(reason,"user_warned"))explanation=pt?"Restricao de moderacao do canal":"Channel moderation restriction";
                    else if(!strcmp(reason,"INVALID_RESPONSE"))explanation=pt?"Resposta da Twitch nao reconhecida":"Unexpected Twitch response";
                    snprintf(notice,sizeof(notice),"%s [%s]",explanation?explanation:vt_msg("Failed to send chat message"),reason);
                    set_status("%s",reason);push_system(notice);
                }else if(!C.stop)set_status(vt_msg("Conectado"));
                /* Anonymous IRC receives the actual broadcast echo; do not fabricate it. */
            }
            if(room_id[0]&&!emotes_done){emotes_done=1;load_badges(room_id);load_7tv(room_id);}
            size_t got=0;CURLcode rc=curl_easy_recv(c,rbuf+have,sizeof(rbuf)-1-have,&got);
            if(rc==CURLE_AGAIN){
                if(sceKernelGetProcessTimeWide()-last_rx>400000000ULL)break;
                sceKernelDelayThread(30000);continue;
            }
            if(rc!=CURLE_OK||!got)break;
            last_rx=sceKernelGetProcessTimeWide();backoff=1;have+=got;rbuf[have]=0;
            char *start=rbuf,*nl;
            while((nl=strstr(start,"\r\n"))!=NULL){
                *nl=0;
                if(irc_parse(start,&line)==0){
                    if(!strcmp(line.command,"PING")){char pong[513];snprintf(pong,sizeof(pong),"PONG :%.504s\r\n",line.trailing);if(irc_send(c,pong)<0)ok=0;}
                    else if(!strcmp(line.command,"ROOMSTATE")){
                        if(!room_id[0])irc_tag(&line,"room-id",room_id,sizeof(room_id));
                    }else if(!strcmp(line.command,"JOIN")){
                        if(!join_reported){join_reported=1;set_status(vt_msg("Conectado"));push_system(vt_msg("Conectado al chat"));}
                        /* Sending is independent of the anonymous JOIN event. */
                    }else if(!strcmp(line.command,"RECONNECT"))ok=0;
                    else if(!strcmp(line.command,"CLEARCHAT")&&!line.has_trailing)push_system(vt_msg("El chat ha sido borrado por un moderador"));
                    else if(chat_build_msg(&line,&msg)==0)push_msg(&msg);
                }
                start=nl+2;
            }
            have=strlen(start);memmove(rbuf,start,have+1);if(have>=sizeof(rbuf)-1)have=0;
        }
        if(c)curl_easy_cleanup(c);
        pthread_mutex_lock(&C.lock);int lost=C.queued;C.queued=C.awaiting=0;C.outgoing[0]=0;pthread_mutex_unlock(&C.lock);
        if(lost&&!C.stop)push_system(vt_msg("Failed to send chat message"));
        if(!C.running)break;
        set_status(vt_msg("Chat desconectado, reintentando en %d s..."),backoff);
        for(int i=0;i<backoff*10&&C.running;i++)sceKernelDelayThread(100000);
        if(backoff<30)backoff*=2;
    }
    C.alive=0;return NULL;
}

void chat_start(const char *login,const char *nick,const char *oauth,const char *sender_id)
{
    if (C.running || C.thread_started || !login || !*login)
        return;
    for (const char *q = login; *q; ++q)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) return;
    if (!C.lock_ok) {
        if (pthread_mutex_init(&C.lock, NULL) != 0) return;
        C.lock_ok = 1;
    }
    if (!C.msgs)
        C.msgs = calloc(CHAT_MSGS, sizeof(chat_msg_t));
    if (!C.msgs)
        return;
    chat_emotes_reset();
    C.head = C.count = 0;
    C.writable=C.queued=C.awaiting=C.read_only_fallback=C.retry_auth=0;C.fallback_code[0]=0;C.last_send=C.sent_at=0;C.outgoing[0]=C.pending[0]=0;
    C.nick[0]=C.oauth[0]=C.sender_id[0]=0;
    if(nick&&oauth&&*nick&&*oauth&&strlen(nick)<sizeof(C.nick)&&strlen(oauth)<sizeof(C.oauth)&&strspn(nick,"abcdefghijklmnopqrstuvwxyz0123456789_")==strlen(nick)&&strspn(oauth,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==strlen(oauth)){
        snprintf(C.nick,sizeof(C.nick),"%s",nick);snprintf(C.oauth,sizeof(C.oauth),"%s",oauth);if(numeric_id(sender_id))snprintf(C.sender_id,sizeof(C.sender_id),"%s",sender_id);
    }
    C.writable=C.oauth[0]&&numeric_id(C.sender_id);
    C.version++;
    twitch_sanitize_login(login,C.login,sizeof(C.login));
    snprintf(C.status, sizeof(C.status), "%s", vt_msg("Conectando al chat..."));
    C.running = 1;
    C.stop = 0;
    C.alive = 1;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstacksize(&attr, 256 * 1024);
        if (rc == 0) rc = pthread_create(&C.thread, &attr, chat_thread, NULL);
        pthread_attr_destroy(&attr);
    }
    if (rc != 0) {
        LOGE("chat: worker creation failed (%d)", rc);
        C.running = 0;
        C.alive = 0;
        set_status(vt_msg("Chat desconectado, reintentando en %d s..."), 0);
    } else {
        C.thread_started = 1;
    }
}

void chat_stop(void)
{
    C.running = 0;
    C.stop = 1;
    if (C.thread_started) {
        pthread_join(C.thread, NULL);
        C.thread_started = 0;
    }
    memset(C.oauth,0,sizeof(C.oauth));C.nick[0]=0;
}

int chat_retry_auth(void){return 0;}
int chat_can_send(void){if(!C.lock_ok)return 0;pthread_mutex_lock(&C.lock);int ok=C.writable&&C.running;pthread_mutex_unlock(&C.lock);return ok;}
int chat_queue_message(const char *text){
    char buf[CHAT_TEXT_MAX];if(chat_prepare_outgoing(text,buf,sizeof(buf))<0)return -3;
    if(!C.lock_ok)return -1;
    unsigned long long now=sceKernelGetProcessTimeWide();int rc=0;
    pthread_mutex_lock(&C.lock);
    if(!C.writable||!C.running)rc=-1;
    else if(C.queued||C.awaiting||(C.last_send&&now-C.last_send<1500000ULL))rc=-2;
    else {snprintf(C.outgoing,sizeof(C.outgoing),"%s",buf);C.queued=1;C.last_send=now;}
    pthread_mutex_unlock(&C.lock);return rc;
}

int chat_running(void) { return C.running; }

void chat_lock(void) { if (C.lock_ok) pthread_mutex_lock(&C.lock); }
void chat_unlock(void) { if (C.lock_ok) pthread_mutex_unlock(&C.lock); }
int chat_count(void) { return C.count; }

chat_msg_t *chat_recent(int k)
{
    if (k < 0 || k >= C.count)
        return NULL;
    return &C.msgs[(C.head - 1 - k + CHAT_MSGS) % CHAT_MSGS];
}

void chat_status(char *out, size_t cap)
{
    chat_lock();
    snprintf(out, cap, "%s", C.status);
    chat_unlock();
}

#endif

#ifndef VT_CHAT_H
#define VT_CHAT_H

/*
 * Twitch IRC over TLS with anonymous read-only mode and optional authenticated
 * sending (chat:read/chat:edit scopes). Includes Twitch and 7TV emotes.
 *
 * The parsing / emote part is plain C and is tested on the PC
 * (tests/chat_test.c); the network thread only exists on the Vita.
 */

#include <stddef.h>
#include <stdint.h>

#define CHAT_TEXT_MAX   512
#define CHAT_TOK_MAX    64
#define CHAT_PIECE_MAX  96
#define CHAT_MSGS       100
#define CHAT_EMOTES_MAX 5000
#define CHAT_BADGES_MAX 3

typedef struct {
    char name[96];        /* word in chat ("KEKW"), "\x01<id>" Twitch emote, "\x02set/version" badge */
    char url[128];
    uint8_t w, h;         /* 1x size in pixels */
} chat_emote_t;

typedef struct {
    uint16_t off, len;    /* text token: bytes in msg->text */
    int16_t emote;        /* >= 0: emote index instead of text */
} chat_tok_t;

/* layout made by the drawing code (main thread) */
typedef struct {
    int16_t x;
    uint8_t line;
    uint8_t kind;         /* 0 text, 1 emote, 2 badge (off = emote-table index) */
    uint16_t off, len;    /* text piece; for emotes off = emote index */
} chat_piece_t;

typedef struct {
    char name[48];
    uint32_t color;       /* vita2d colour (ABGR) */
    int system;           /* sub / raid notices: drawn differently */
    int action;           /* "/me" message */
    int16_t badge[CHAT_BADGES_MAX]; /* moderator, VIP, subscriber... (emote-table index) */
    int nbadge;
    int16_t name_x;       /* layout: where the name starts (after the badges) */
    char text[CHAT_TEXT_MAX];
    chat_tok_t tok[CHAT_TOK_MAX];
    int ntok;

    int lay_w;            /* width the layout was made for (0 = none yet) */
    int lay_lines;
    int npiece;
    chat_piece_t piece[CHAT_PIECE_MAX];
} chat_msg_t;

/* ---- parsing (pure) ---- */

typedef struct {
    char tags[2048];      /* raw tags, unescaped on lookup */
    char prefix[128];
    char command[32];
    char params[128];     /* middle params, e.g. "#channel" */
    char trailing[1024];
    int has_trailing;
} irc_line_t;

int irc_parse(const char *line, irc_line_t *out);
/* Unescaped tag value into out; returns 0 if present. */
int irc_tag(const irc_line_t *l, const char *key, char *out, size_t cap);

/* Builds a chat message from a PRIVMSG / USERNOTICE. Returns 0 if it should be shown. */
int chat_build_msg(const irc_line_t *l, chat_msg_t *m);

/* ---- emotes ---- */
void chat_emotes_reset(void);
/* Parses a 7TV emote set ("emote_set" of /v3/users/twitch/<id>, or the global set). */
int chat_load_7tv(const char *json);
int chat_emote_count(void);
const chat_emote_t *chat_emote(int idx);
int chat_find_emote(const char *word, size_t len);
/* Twitch chat badges from GQL: "data.badges" (global) or "data.user.broadcastBadges"
 * (channel; loaded after the global ones so they override them). */
int chat_load_badges(const char *json);
/* "moderator/1,subscriber/12" -> badge indexes (falls back to version 1 / 0) */
int chat_resolve_badges(const char *tag, int16_t *out, int max);

/* ---- live chat (Vita only) ---- */
/* Anonymous IRC read; authenticated Helix writes use verified Sony HTTPS. */
void chat_start(const char *login,const char *nick,const char *oauth,const char *sender_id);
int chat_can_send(void);
int chat_retry_auth(void);
/* 0 queued, -1 unavailable, -2 rate limited, -3 invalid text. */
int chat_queue_message(const char *text);
void chat_stop(void);
/* Pure safety parser used by Vita and host regression tests. */
int chat_prepare_outgoing(const char *in,char *out,size_t cap);
/* Helix HTTP 200: 0 confirmed, 1 dropped/held, -1 malformed. */
int chat_validate_identity(const char *body,const char *client,const char *sender,char *reason,size_t cap);
int chat_parse_send_result(const char *json,char *code,size_t code_cap);
int chat_running(void);
/* Locks the message list for reading; recent(0) is the newest. */
void chat_lock(void);
void chat_unlock(void);
int chat_count(void);
unsigned chat_version(void); /* changes whenever a message is added */
chat_msg_t *chat_recent(int k);
/* "Conectando...", "7TV: 812 emotes" etc. (copied under the lock) */
void chat_status(char *out, size_t cap);

#endif

#ifndef VT_TWITCH_H
#define VT_TWITCH_H

#include <stddef.h>
#include "m3u8.h"

/* Twitch's own TV-app client id: it accepts the OAuth device-code flow (needed
 * to log in from a console without a browser) and works with GQL and usher
 * exactly like the web id streamlink uses. Tokens are bound to this id, so it
 * is used for every request. */
#define TWITCH_CLIENT_ID "ue6666qo983tsx6so1t0vnawi233wa"

typedef enum {
    TW_LIST_FOLLOWED = 0,   /* live channels the user follows (needs login) */
    TW_LIST_TOP,            /* most watched live streams */
    TW_LIST_SEARCH          /* channel search (live and offline) */
} twitch_list_t;

typedef struct {
    char login[32];
    char display[64];
    char title[192];
    char game[96];
    char preview[192];   /* 320x180 JPEG thumbnail (empty if offline) */
    char avatar[192];    /* 70x70 profile picture */
    int viewers;
    int live;
} twitch_stream_t;

/* Lowercases and strips anything not valid in a Twitch login. Returns length. */
int twitch_sanitize_login(const char *in, char *out, size_t cap);

/* Escapes s as the contents of a JSON string (no quotes). Returns length or -1. */
int twitch_json_escape(const char *s, char *out, size_t cap);

/* Request bodies (exposed for the host tests). */
int twitch_build_gql_body(const char *login, char *out, size_t cap);
int twitch_build_list_body(twitch_list_t kind, const char *query, char *out, size_t cap);

/* Extracts value/signature from the PlaybackAccessToken response. 0 on success. */
int twitch_parse_token(const char *json, char *token, size_t token_cap,
                       char *sig, size_t sig_cap);

/* Parses a list response. Returns the number of entries, or -1 (bad JSON /
 * GQL error, message in err). For TW_LIST_FOLLOWED, -2 means "not logged in". */
int twitch_parse_list(const char *json, twitch_list_t kind, twitch_stream_t *out, int max,
                      char *err, size_t errlen);

/* Fallback for Siguiendo: the user's full follow list, 100 per page. */
int twitch_build_follows_body(const char *after, char *out, size_t cap);
/* Appends the live channels of one page to out[*n]; cursor/has_next drive paging.
 * Returns 0, -1 on error, -2 when the session is not valid. */
int twitch_parse_follows(const char *json, twitch_stream_t *out, int max, int *n,
                         char *cursor, size_t cursor_cap, int *has_next, char *err, size_t errlen);
void twitch_sort_by_viewers(twitch_stream_t *s, int n);

#ifndef VT_NO_NET
#include "buf.h"

/* POSTs a GQL body. oauth may be NULL/empty. Returns 0 if HTTP 200. */
int twitch_gql(const char *body, const char *oauth, buf_t *resp,
               char *err, size_t errlen, volatile int *abort_flag);

int twitch_fetch_list(twitch_list_t kind, const char *query, const char *oauth,
                      twitch_stream_t *out, int max, char *err, size_t errlen,
                      volatile int *abort_flag);

/* Full flow, like streamlink: access token -> usher master playlist -> variants.
 * Returns the number of variants (>0), or <0 with a message in err. */
int twitch_get_variants(const char *channel, const char *oauth, m3u8_variant_t *out, int max,
                        char *err, size_t errlen, volatile int *abort_flag);
int twitch_find_clean_variant(const char *channel,const char *oauth,const char *current,
                              int width,int height,float fps,char *out,
                              volatile int *abort_flag);
#endif

#endif

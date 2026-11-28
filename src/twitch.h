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
    TW_LIST_SEARCH,         /* channel search (live and offline) */
    TW_LIST_CATEGORY,       /* authenticated category-specific live list */
    TW_LIST_ALL_FOLLOWS,    /* full account list, including offline */
    TW_LIST_SAVED          /* local favorites; never account follows */
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

typedef struct {char id[32],name[96],art[192],slug[128];} twitch_category_t;
void twitch_category_cover(const char *name,char *out,size_t cap);
int twitch_parse_gql_categories(const char *json,twitch_category_t *out,int max);
int twitch_parse_categories(const char *json,twitch_category_t *out,int max);
int twitch_parse_category_streams(const char *json,twitch_stream_t *out,int max);

/* Lowercases and strips anything not valid in a Twitch login. Returns length. */
int twitch_sanitize_login(const char *in, char *out, size_t cap);

/* Escapes s as the contents of a JSON string (no quotes). Returns length or -1. */
int twitch_json_escape(const char *s, char *out, size_t cap);

/* Request bodies (exposed for the host tests). */
int twitch_build_gql_body(const char *login, char *out, size_t cap);
int twitch_build_list_body(twitch_list_t kind, const char *query, char *out, size_t cap);

int twitch_channel_url(const char *login, char *out, size_t cap);
int twitch_build_vod_url(const char *id,const char *token,const char *sig,unsigned nonce,char *out,size_t cap);
const char *twitch_vod_rejection(const char *body);
const char *twitch_vod_access_message(const char *reason);
int twitch_video_subscriber_type(const char *type);
int twitch_parse_clip_source(const char *json,char *vod,size_t cap,int *offset);
int twitch_parse_channel_bio(const char *json, char *bio, size_t cap);
/* Unknown=-1; 0=not followed; 1=followed; 2=not eligible. */
int twitch_parse_follow_state(const char *json,char *id,size_t cap);
int twitch_parse_helix_follow(const char *json,const char *target);
int twitch_parse_follows_all(const char *json,twitch_stream_t *out,int max,int *n,char *cursor,size_t cursor_cap,int *has_next,char *err,size_t errlen);
const char *twitch_follow_label(int state,int saved);
const char *twitch_follow_notice_detail(int status);
/* Local fallback UI states: never imply an account mutation. */
enum { TW_FOLLOW_LOCAL_SAVED=11, TW_FOLLOW_LOCAL_ERROR=12, TW_FOLLOW_ALREADY=13, TW_FOLLOW_LOCAL_REMOVED=14 };
const char *twitch_follow_notice(int status);
typedef struct {
    char login[32],oauth[128],id[32],user_id[32];
    int status; /* diagnostic code, never credentials */
    int followers; char started_at[32],created_at[32],avatar[192];
    int state; /* read-only account relationship */
} twitch_follow_job_t;

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

typedef struct {int followers;char created[32],bio[512],links[384];} twitch_about_t;
#define VT_MEDIA_MAX 30
typedef struct {char id[80],title[192],thumb[192],date[32],broadcaster[32],channel[64],type[16],url[192];int seconds,offset,subscriber_only;char vod_id[32];} twitch_media_t;
typedef struct {char login[32],oauth[128],user_id[32],vod_id[32],broadcaster[32],title[96],id[80],url[192],error[192];int duration,offset,confirmed;} twitch_clip_job_t;
#ifndef VT_NO_NET
#include "buf.h"
int twitch_fetch_about(const char *login,const char *oauth,twitch_about_t *out,volatile int *abort_flag);
int twitch_fetch_media(const char *login,const char *oauth,int kind,twitch_media_t *out,int max,char *err,size_t errlen,volatile int *abort_flag);
int twitch_vod_variants(const char *id,const char *oauth,m3u8_variant_t *out,int max,char *err,size_t errlen,volatile int *abort_flag);
int twitch_create_clip(void *arg,volatile int *abort_flag);
int twitch_clip_mp4(const char *id,char *url,size_t cap,int *height,char *err,size_t errlen,char *vod,size_t vod_cap,int *offset,volatile int *abort_flag);

/* POSTs a GQL body. oauth may be NULL/empty. Returns 0 if HTTP 200. */
int twitch_gql(const char *body, const char *oauth, buf_t *resp,
               char *err, size_t errlen, volatile int *abort_flag);

int twitch_follow_run(void *arg,volatile int *abort_flag);
void twitch_refresh_saved(twitch_stream_t *items,int count,volatile int *abort_flag);
int twitch_fetch_categories(const char *oauth,twitch_category_t *out,int max,volatile int *abort_flag);
int twitch_fetch_channel_bio(const char *login, char *bio, size_t cap, volatile int *abort_flag);
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

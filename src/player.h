#ifndef VT_PLAYER_H
#define VT_PLAYER_H

#include <vita2d.h>
#include "m3u8.h"

typedef struct {
    const char *url;        /* media playlist of the chosen quality */
    const char *channel;    /* display name */
    const char *title;      /* stream title (may be empty) */
    const char *profile_avatar,*profile_game,*profile_label,*follow_label;
    int (*saved_has)(const char *login);
    int (*saved_toggle)(const char *login,const char *display,const char *avatar);
    int (*saved_ensure)(const char *login,const char *display,const char *avatar);
    int account_follow_known;
    const char *(*saved_label)(int saved);
    const char *quality;    /* label shown in the OSD */
    int width, height;      /* from the master playlist (0 = unknown) */
    float fps;              /* from the master playlist (0 = unknown) */
    vita2d_pgf *font;
    unsigned btn_back;      /* SCE_CTRL_* used to leave the player */
    const char *login;      /* channel login (chat room) */
    const char *chat_nick;  /* logged-in Twitch account (nullable) */
    const char *chat_oauth; /* OAuth token with user:write:chat (nullable) */
    const char *chat_sender_id;
    int profile;
    int recorded,start_seconds,duration_seconds;
    int preview_end;
    const char *preview_playlist;
    m3u8_preview_t *live_capture;
    int *position_seconds,*seek_requested;
    int auto_quality;
    int has_lower;
    int diagnostics;
    char *ad_backup_url; /* optional M3U8_URL_MAX output */
    int *volume;
    m3u8_variant_t *const *variants; /* existing compatible live variants */
    int variant_count, variant_current;
    int *variant_requested; /* output index on PLAYER_CHANGE_QUALITY */
    int *viewer_count;      /* last snapshot, -1 when not available */
    int *chat_on;           /* in/out: chat panel visible (remembered by the caller) */
} player_params_t;

/* Plays the stream until the user leaves or it ends. Blocks.
 * Returns 0 when the user exited, <0 on error (err filled). */
#define PLAYER_RECONNECT 1
#define PLAYER_LOWER_QUALITY 2
#define PLAYER_AD_SWITCH 3
#define PLAYER_CHANGE_QUALITY 4
#define PLAYER_SEEK 5
#define PLAYER_RECORDING_END 6
#define PLAYER_CREATE_CLIP 7
int player_run(const player_params_t *pp, char *err, int errlen);

#endif

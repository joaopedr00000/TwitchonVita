#ifndef VT_PLAYER_H
#define VT_PLAYER_H

#include <vita2d.h>

typedef struct {
    const char *url;        /* media playlist of the chosen quality */
    const char *channel;    /* display name */
    const char *title;      /* stream title (may be empty) */
    const char *quality;    /* label shown in the OSD */
    int width, height;      /* from the master playlist (0 = unknown) */
    float fps;              /* from the master playlist (0 = unknown) */
    vita2d_pgf *font;
    unsigned btn_back;      /* SCE_CTRL_* used to leave the player */
    const char *login;      /* channel login (chat room) */
    const char *chat_nick;  /* logged-in Twitch account (nullable) */
    const char *chat_oauth; /* OAuth token with chat:read,chat:edit (nullable) */
    int profile;
    int auto_quality;
    int has_lower;
    int diagnostics;
    char *ad_backup_url; /* optional M3U8_URL_MAX output */
    int *volume;
    int *viewer_count;      /* last snapshot, -1 when not available */
    int *chat_on;           /* in/out: chat panel visible (remembered by the caller) */
} player_params_t;

/* Plays the stream until the user leaves or it ends. Blocks.
 * Returns 0 when the user exited, <0 on error (err filled). */
#define PLAYER_RECONNECT 1
#define PLAYER_LOWER_QUALITY 2
#define PLAYER_AD_SWITCH 3
int player_run(const player_params_t *pp, char *err, int errlen);

#endif

#ifndef VT_AUTH_H
#define VT_AUTH_H

#include <stddef.h>

/* Twitch login with the OAuth 2.0 device code flow: the Vita shows a short
 * code, the user enters it at twitch.tv/activate on a phone or PC, and the
 * Vita polls until the token is issued. No password is ever typed on the
 * console. */

#define AUTH_SCOPES "user:read:follows chat:read chat:edit user:write:chat clips:edit channel:manage:clips editor:manage:clips"

typedef struct {
    char access[64];
    char refresh[128];
    char user_id[24];
    char login[32];
    char display[64];
    char avatar[192];
} auth_t;

typedef struct {
    char device_code[96];
    char user_code[16];
    char uri[128];
    int interval;     /* seconds between polls */
    int expires_in;   /* seconds */
} auth_device_t;

enum { AUTH_PENDING = 0, AUTH_OK = 1, AUTH_SLOW_DOWN = 2 };

/* Pure parsers (host-tested). Return 0 on success, -1 on error (msg in err). */
int auth_parse_device(const char *json, auth_device_t *d, char *err, size_t errlen);
/* Returns AUTH_OK, AUTH_PENDING, AUTH_SLOW_DOWN or -1. */
int auth_parse_token(const char *json, auth_t *a, char *err, size_t errlen);
int auth_parse_validate(const char *json, auth_t *a);

int auth_load(auth_t *a, const char *path);
int auth_save(const auth_t *a, const char *path);

#ifndef VT_NO_NET
int auth_device_start(auth_device_t *d, char *err, size_t errlen, volatile int *abort_flag);
int auth_device_poll(const auth_device_t *d, auth_t *a, char *err, size_t errlen,
                     volatile int *abort_flag);
/* Checks the token; refreshes it if expired. Fills login/user_id and the
 * display name/avatar. Returns 0 if the session is usable, -1 if the user must
 * log in again, -2 on network errors (keep the stored session). */
int auth_ensure_valid(auth_t *a, char *err, size_t errlen, volatile int *abort_flag);
#endif

#endif

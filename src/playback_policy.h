#ifndef VT_PLAYBACK_POLICY_H
#define VT_PLAYBACK_POLICY_H
#include "m3u8.h"
typedef enum { VT_PROFILE_ECO, VT_PROFILE_BALANCED, VT_PROFILE_LOW_LATENCY, VT_PROFILE_QUALITY, VT_PROFILE_COUNT } vt_profile;
int vt_profile_edge(int profile);
int vt_profile_buffer_ms(int profile, int rebuffers);
int vt_profile_clock(int profile, int height, float fps);
int vt_profile_pick(m3u8_variant_t *const *list, int n, int profile);
int vt_profile_lower(m3u8_variant_t *const *list, int n, const m3u8_variant_t *current);
#endif

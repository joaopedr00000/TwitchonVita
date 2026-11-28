#ifndef VT_I18N_H
#define VT_I18N_H
#include <stddef.h>
#define VT_LANG_COUNT 19
typedef enum {
  VT_I_FOLLOWING,
  VT_I_EXPLORE,
  VT_I_SEARCH,
  VT_I_LIVE,
  VT_I_OFFLINE,
  VT_I_MENU,
  VT_I_ACCOUNT,
  VT_I_LANGUAGE,
  VT_I_TOUCH,
  VT_I_CHAT,
  VT_I_WRITE,
  VT_I_LOGIN_TO_CHAT,
  VT_I_ON,
  VT_I_OFF,
  VT_I_REFRESH,
  VT_I_CLOSE,
  VT_I_PLAY,
  VT_I_QUALITY,
  VT_I_LOADING,
  VT_I_BACK,
  VT_I_EXIT,
  VT_I_SETTINGS,
  VT_I_CLEAR_CACHE,
  VT_I_ABOUT,
  VT_I_LOGIN,
  VT_I_LOGOUT,
  VT_I_TAB,
  VT_I_SEND_MESSAGE,
  VT_I_SHOW_INFO,
  VT_I_HIDE_INFO,
  VT_I_COUNT
} vt_text_id;
int vt_lang_current(void);
int vt_lang_set(int index);
int vt_lang_set_code(const char *code);
int vt_lang_auto(int system_language);
const char *vt_lang_code(int index);
const char *vt_lang_name(int index);
const char *vt_tr(vt_text_id key);
size_t vt_msg_count(void);
const char *vt_msg_key(size_t i);
const char *vt_msg(const char *source);
typedef enum { VT_O_PROFILE,VT_O_ECONOMY,VT_O_BALANCED,VT_O_LOW_LATENCY,VT_O_QUALITY,VT_O_AUTO,VT_O_DIAG,VT_O_VOLUME,VT_O_RECONNECT,VT_O_LOWER,VT_O_REMEMBER,VT_O_FPS,VT_O_RATE,VT_O_CPU,VT_O_REBUFFERS,VT_O_BUFFER,VT_O_DROPPED,VT_O_COUNT } vt_option_id;
const char *vt_opt(vt_option_id key);
#endif

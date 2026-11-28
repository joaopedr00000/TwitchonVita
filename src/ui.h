#ifndef VT_UI_H
#define VT_UI_H

#include <stddef.h>
#include <vita2d.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* Twitch-like dark palette */
#define C_BG       RGBA8(12, 10, 19, 255)
#define C_SURFACE  RGBA8(23, 20, 33, 255)
#define C_PANEL    RGBA8(34, 29, 46, 255)
#define C_PANEL2   RGBA8(48, 40, 66, 255)
#define C_PURPLE   RGBA8(145, 70, 255, 255)
#define C_PURPLE_D RGBA8(119, 44, 232, 255)
#define C_WHITE    RGBA8(239, 239, 241, 255)
#define C_GREY     RGBA8(173, 173, 184, 255)
#define C_DIM      RGBA8(110, 110, 120, 255)
#define C_RED      RGBA8(235, 4, 0, 255)
#define C_GREEN    RGBA8(0, 200, 120, 255)
#define C_SHADE    RGBA8(0, 0, 0, 170)
#define C_SCRIM    RGBA8(0, 0, 0, 200)

extern vita2d_pgf *ui_font;

/* Removes characters the system font cannot draw (emoji, joiners, variation
 * selectors) so titles don't fill up with boxes. */
void ui_clean_text(const char *in, char *out, size_t cap);

int ui_text_w(float scale, const char *s);
void ui_text(int x, int y, unsigned color, float scale, const char *s);
/* Draws s cut with "..." to fit max_w pixels. */
void ui_text_fit(int x, int y, int max_w, unsigned color, float scale, const char *s);
void ui_text_center(int cx, int y, unsigned color, float scale, const char *s);

void ui_round_rect(float x, float y, float w, float h, float r, unsigned color);
void ui_frame(float x, float y, float w, float h, float t, unsigned color);
void ui_spinner(float cx, float cy, float r, unsigned color);
/* Draws a texture scaled to fill w x h. */
void ui_image(vita2d_texture *t, float x, float y, float w, float h);
/* Small rounded "button" glyph with a label, returns the width used. */
int ui_hint(int x, int y, const char *btn, const char *label);
void ui_hint_bounded(int x,int y,int width,const char *btn,const char *label);

/* "1.234" / "12,3 mil" style viewer counts. */
void ui_fmt_viewers(int v, char *out, size_t cap);

/* Controller with key repeat for the d-pad. */
typedef struct {
    unsigned held, pressed;
    unsigned prev;
    unsigned long long repeat_at;
    /* Front touch coordinates are normalized to 960 x 544. Tap is delivered
     * only on release and never after a drag (prevents accidental stream opens). */
    int touch_down, touch_prev_down, touch_tap, touch_dragged;
    int touch_x, touch_y, touch_start_x, touch_start_y, touch_last_y, touch_dy;
} ui_input_t;
void ui_touch_enable(int enabled);
int ui_touch_enabled(void);
int ui_touch_hit(const ui_input_t *in,int x,int y,int w,int h);
void ui_input_poll(ui_input_t *in);

#endif

#include "i18n.h"
/* Chat panel drawing (main thread). */
#include "chat_view.h"
#include "chat.h"
#include "images.h"
#include "log.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>

#define FONT_SCALE 1.04f
#define LINE_H     31
#define EMOTE_H    25
#define BADGE_H    20  /* Twitch badges are 18x18 at 1x */
#define PAD        10
#define HEADER     34

#define COL_BG      0xFF141014u
#define COL_HEADER  0xFF1F1A1Fu
#define COL_TEXT    0xFFEFEFEFu
#define COL_GREY    0xFFA0A0A0u
#define COL_SYSTEM  0xFFFF8CBFu /* light purple */
#define COL_SYS_BG  0x40FF4691u

static int text_w(vita2d_pgf *f, const char *s)
{
    return vita2d_pgf_text_width(f, FONT_SCALE, s);
}

static int emote_w(const chat_emote_t *e)
{
    int w = e && e->h ? e->w * EMOTE_H / e->h : EMOTE_H;
    return w < 8 ? 8 : w > 3 * EMOTE_H ? 3 * EMOTE_H : w;
}

static void add_piece(chat_msg_t *m, int x, int line, int kind, int off, int len)
{
    if (m->npiece >= CHAT_PIECE_MAX)
        return;
    chat_piece_t *p = &m->piece[m->npiece++];
    p->x = (int16_t)x;
    p->line = (uint8_t)(line > 255 ? 255 : line);
    p->kind = (uint8_t)kind;
    p->off = (uint16_t)off;
    p->len = (uint16_t)len;
}

static void layout(chat_msg_t *m, int w, vita2d_pgf *f)
{
    int x = 0, line = 0;
    int space = text_w(f, " ");
    if (space < 4) space = 4;

    m->npiece = 0;
    m->name_x = 0;
    if (m->name[0] && !m->system) {
        char nm[64];
        /* badges (moderator, VIP, subscriber...) before the name */
        for (int i = 0; i < m->nbadge; i++) {
            add_piece(m, x, 0, 2, m->badge[i], 0);
            x += BADGE_H + 3;
        }
        m->name_x = (int16_t)x;
        snprintf(nm, sizeof(nm), m->action ? "%s " : "%s: ", m->name);
        x += text_w(f, nm);
    }
    for (int i = 0; i < m->ntok; i++) {
        const chat_tok_t *t = &m->tok[i];
        if (t->emote >= 0) {
            int ew = emote_w(chat_emote(t->emote));
            if (x > 0 && x + ew > w) { line++; x = 0; }
            add_piece(m, x, line, 1, t->emote, 0);
            x += ew + 3;
        } else {
            const char *word = m->text + t->off;
            int ww = text_w(f, word);
            if (x > 0 && x + ww > w) { line++; x = 0; }
            add_piece(m, x, line, 0, t->off, t->len);
            x += ww + space;
        }
    }
    m->lay_lines = line + 1;
    m->lay_w = w;
}

static void draw_msg(chat_msg_t *m, int x0, int y0, int w, vita2d_pgf *f)
{
    if (m->system) {
        vita2d_draw_rectangle(x0 - PAD + 2, y0, w + 2 * PAD - 4, m->lay_lines * LINE_H + 2, COL_SYS_BG);
        vita2d_draw_rectangle(x0 - PAD + 2, y0, 3, m->lay_lines * LINE_H + 2, COL_SYSTEM);
    }
    if (m->name[0] && !m->system) {
        char nm[64];
        snprintf(nm, sizeof(nm), m->action ? "%s " : "%s:", m->name);
        vita2d_pgf_draw_text(f, x0 + m->name_x, y0 + LINE_H - 7, m->color, FONT_SCALE, nm);
    }
    unsigned tcol = m->system ? COL_SYSTEM : m->action ? m->color : COL_TEXT;
    for (int i = 0; i < m->npiece; i++) {
        const chat_piece_t *p = &m->piece[i];
        int px = x0 + p->x, py = y0 + p->line * LINE_H;
        if (p->kind == 0) {
            vita2d_pgf_draw_text(f, px, py + LINE_H - 7, tcol, FONT_SCALE, m->text + p->off);
        } else if (p->kind == 2) {
            const chat_emote_t *e = chat_emote(p->off);
            vita2d_texture *tex = e ? img_get(e->url) : NULL;
            int by = py + (LINE_H - BADGE_H) / 2 - 1;
            if (tex) {
                float s = (float)BADGE_H / vita2d_texture_get_height(tex);
                vita2d_draw_texture_scale(tex, px, by, s, s);
            } else {
                vita2d_draw_rectangle(px, by, BADGE_H, BADGE_H, 0xFF2E2A2Eu);
            }
        } else {
            const chat_emote_t *e = chat_emote(p->off);
            int ew = emote_w(e);
            vita2d_texture *tex = e ? img_get(e->url) : NULL;
            int ey = py + (LINE_H - EMOTE_H) / 2;
            if (tex) {
                float sx = (float)ew / vita2d_texture_get_width(tex);
                float sy = (float)EMOTE_H / vita2d_texture_get_height(tex);
                vita2d_draw_texture_scale(tex, px, ey, sx, sy);
            } else {
                vita2d_draw_rectangle(px, ey, ew, EMOTE_H, 0xFF2E2A2Eu);
            }
        }
    }
}

static void render_panel(int x, int y, int w, int h, vita2d_pgf *f, const char *channel)
{
    char status[96], title[96];
    int inner = w - 2 * PAD;

    vita2d_draw_rectangle(x, y, w, h, COL_BG);
    vita2d_draw_rectangle(x, y, w, HEADER, COL_HEADER);
    snprintf(title, sizeof(title), vt_msg("Chat de %s"), channel ? channel : "");
    ui_text_fit(x+PAD,y+24,inner,COL_TEXT,1.0f,title);
    chat_status(status, sizeof(status));
    ui_text_fit(x+PAD,y+HEADER+21,inner,COL_GREY,0.85f,status);

    int top = y + HEADER + 30;
    int bottom = y + h - 44;
    chat_lock();
    int n = chat_count();
    for (int k = 0; k < n; k++) {
        chat_msg_t *m = chat_recent(k);
        if (m->lay_w != inner)
            layout(m, inner, f);
        int mh = m->lay_lines * LINE_H + 4;
        if (bottom - mh < top)
            break;
        bottom -= mh;
        draw_msg(m, x + PAD, bottom, inner, f);
    }
    chat_unlock();
}

/* ------------------------------------------------- cached panel texture */

#define MIN_INTERVAL_US 160000ULL /* re-render at most ~6 times per second */

static vita2d_texture *g_rt;      /* full-screen render target (vita2d's projection is 960x544) */
static int g_rt_failed;
static unsigned g_seen_chat = ~0u, g_seen_img = ~0u;
static char g_seen_status[96];
static unsigned long long g_last_render;
static int g_w, g_h;
static vita2d_pgf *g_font;
static const char *g_channel;
static int g_renders;
static unsigned long long g_render_us;

void chat_view_update(int w, int h, vita2d_pgf *f, const char *channel)
{
    char status[96];
    unsigned long long now = sceKernelGetProcessTimeWide();

    g_w = w; g_h = h; g_font = f; g_channel = channel;
    if (g_rt_failed)
        return;
    if (!g_rt) {
        g_rt = vita2d_create_empty_texture_rendertarget(960, 544, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        if (!g_rt) {
            LOGE("chat: no se pudo crear la textura del panel, se dibuja directo");
            g_rt_failed = 1;
            return;
        }
        g_seen_chat = ~0u;
    }
    chat_status(status, sizeof(status));
    int dirty = g_seen_chat != chat_version() || g_seen_img != img_version() || strcmp(status, g_seen_status);
    if (!dirty || (now - g_last_render < MIN_INTERVAL_US && g_seen_chat != ~0u))
        return;

    g_seen_chat = chat_version();
    g_seen_img = img_version();
    snprintf(g_seen_status, sizeof(g_seen_status), "%s", status);
    g_last_render = now;

    vita2d_start_drawing_advanced(g_rt, 0);
    render_panel(0, 0, w, h, f, channel);
    vita2d_end_drawing();
    /* the frame's scene resets vita2d's vertex pool: the GPU must be done */
    vita2d_wait_rendering_done();

    g_renders++;
    g_render_us += sceKernelGetProcessTimeWide() - now;
}

void chat_view_draw(int x, int y)
{
    if (g_rt && !g_rt_failed)
        vita2d_draw_texture_part(g_rt, x, y, 0, 0, g_w, g_h);
    else if (g_font)
        render_panel(x, y, g_w, g_h, g_font, g_channel); /* fallback */
}

void chat_view_free(void)
{
    if (g_rt) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(g_rt);
        g_rt = NULL;
    }
    g_seen_chat = ~0u;
}

int chat_view_renders(void) { return g_renders; }
int chat_view_avg_ms_x10(void) { return g_renders ? (int)(g_render_us / (unsigned long long)g_renders / 100ULL) : 0; }

#include "ui.h"
#include "text.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>

vita2d_pgf *ui_font;

void ui_clean_text(const char *in, char *out, size_t cap)
{
    text_clean(in, out, cap);
}

int ui_text_w(float scale, const char *s)
{
    return vita2d_pgf_text_width(ui_font, scale, s);
}

void ui_text(int x, int y, unsigned color, float scale, const char *s)
{
    vita2d_pgf_draw_text(ui_font, x, y, color, scale, s);
}

void ui_text_fit(int x, int y, int max_w, unsigned color, float scale, const char *s)
{
    if(!s||max_w<=0||scale<=0)return;
    char buf[256];
    size_t len=strlen(s);
    size_t n=len<sizeof(buf)-1?len:sizeof(buf)-1;
    if(n<len)while(n>0&&((unsigned char)s[n]&0xC0)==0x80)n--;
    memcpy(buf,s,n);buf[n]=0;
    if(n==len&&ui_text_w(scale,buf)<=max_w){ui_text(x,y,color,scale,buf);return;}
    if(ui_text_w(scale,"...")>max_w)return;
    if(n>sizeof(buf)-4)n=sizeof(buf)-4;
    while(n>0&&((unsigned char)s[n]&0xC0)==0x80)n--;
    for(;;){
        memcpy(buf+n,"...",4);
        if(ui_text_w(scale,buf)<=max_w){ui_text(x,y,color,scale,buf);return;}
        if(n==0)return;
        do{n--;}while(n>0&&((unsigned char)s[n]&0xC0)==0x80);
    }
}

void ui_text_center(int cx, int y, unsigned color, float scale, const char *s)
{
    ui_text(cx - ui_text_w(scale, s) / 2, y, color, scale, s);
}

void ui_round_rect(float x, float y, float w, float h, float r, unsigned color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    vita2d_draw_rectangle(x + r, y, w - 2 * r, h, color);
    vita2d_draw_rectangle(x, y + r, r, h - 2 * r, color);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, color);
    vita2d_draw_fill_circle(x + r, y + r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + r, r, color);
    vita2d_draw_fill_circle(x + r, y + h - r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, color);
}

void ui_frame(float x, float y, float w, float h, float t, unsigned color)
{
    vita2d_draw_rectangle(x - t, y - t, w + 2 * t, t, color);
    vita2d_draw_rectangle(x - t, y + h, w + 2 * t, t, color);
    vita2d_draw_rectangle(x - t, y, t, h, color);
    vita2d_draw_rectangle(x + w, y, t, h, color);
}

void ui_spinner(float cx, float cy, float r, unsigned color)
{
    float t = (float)(sceKernelGetProcessTimeWide() % 1000000ULL) / 1000000.0f;
    int head = (int)(t * 8);
    for (int i = 0; i < 8; i++) {
        float a = (float)i / 8.0f * 6.2831853f;
        int age = (head - i + 8) % 8;
        unsigned alpha = (unsigned)(255 - age * 28);
        unsigned c = (color & 0x00FFFFFFu) | (alpha << 24);
        vita2d_draw_fill_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.22f, c);
    }
}

void ui_image(vita2d_texture *t, float x, float y, float w, float h)
{
    float tw = (float)vita2d_texture_get_width(t), th = (float)vita2d_texture_get_height(t);
    if (tw > 0 && th > 0)
        vita2d_draw_texture_scale(t, x, y, w / tw, h / th);
}

/* Native PlayStation glyphs: no dependency on optional PGF characters. */
static int ps_kind(const char *btn)
{
    if(!strcmp(btn,"X")||!strcmp(btn,"×"))return 1;
    if(!strcmp(btn,"O")||!strcmp(btn,"○"))return 2;
    if(!strcmp(btn,"□"))return 3;
    if(!strcmp(btn,"△"))return 4;
    return 0;
}
static void ps_line(float x,float y,float x2,float y2,unsigned c)
{
    vita2d_draw_line(x,y,x2,y2,c);
    vita2d_draw_line(x+1,y,x2+1,y2,c);
    vita2d_draw_line(x,y+1,x2,y2+1,c);
}
static int hint_button(int x,int y,const char *btn,int draw)
{
    int k=ps_kind(btn),bw=k?24:ui_text_w(0.72f,btn)+12;
    if(bw<24)bw=24;
    if(!draw)return bw;
    if(!k){ui_round_rect(x,y-17,bw,24,6,C_PANEL2);ui_text_center(x+bw/2,y,C_WHITE,0.72f,btn);return bw;}
    unsigned colors[]={0,RGBA8(104,174,255,255),RGBA8(255,112,129,255),RGBA8(245,140,222,255),RGBA8(90,221,168,255)};
    unsigned c=colors[k];float cx=x+12,cy=y-5;
    vita2d_draw_fill_circle(cx,cy,12,c);
    vita2d_draw_fill_circle(cx,cy,10, C_PANEL2);
    if(k==1){ps_line(cx-5,cy-5,cx+4,cy+4,c);ps_line(cx+4,cy-5,cx-5,cy+4,c);}
    else if(k==2){vita2d_draw_fill_circle(cx,cy,6,c);vita2d_draw_fill_circle(cx,cy,4,C_PANEL2);}
    else if(k==3){vita2d_draw_rectangle(cx-6,cy-6,12,12,c);vita2d_draw_rectangle(cx-4,cy-4,8,8,C_PANEL2);}
    else {ps_line(cx,cy-6,cx-6,cy+5,c);ps_line(cx,cy-6,cx+6,cy+5,c);ps_line(cx-6,cy+5,cx+6,cy+5,c);}
    return bw;
}
void ui_hint_bounded(int x,int y,int width,const char *btn,const char *label)
{
    int bw=hint_button(x,y,btn,0);if(width<bw+6)return;
    hint_button(x,y,btn,1);
    ui_text_fit(x+bw+7,y,width-bw-9,C_WHITE,0.88f,label);
}
int ui_hint(int x, int y, const char *btn, const char *label)
{
    int bw=hint_button(x,y,btn,0),w=bw+7+ui_text_w(0.88f,label)+10;
    ui_hint_bounded(x,y,w,btn,label);
    return w+10;
}

void ui_fmt_viewers(int v, char *out, size_t cap)
{
    if(v>=1000000)snprintf(out,cap,"%.1f M",v/1e6);
    else if(v>=10000)snprintf(out,cap,"%.1f K",v/1e3);
    else snprintf(out,cap,"%d",v);
}

#define DPAD (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)
static int g_touch_enabled = 1;
void ui_touch_enable(int enabled) { g_touch_enabled = enabled != 0; }
int ui_touch_enabled(void) {return g_touch_enabled;}
int ui_touch_hit(const ui_input_t *in,int x,int y,int w,int h) {
    return in && in->touch_tap && in->touch_x >= x && in->touch_x < x+w &&
           in->touch_y >= y && in->touch_y < y+h;
}


void ui_input_poll(ui_input_t *in)
{
    SceCtrlData pad;
    memset(&pad,0,sizeof(pad));
    pad.lx=pad.ly=128;
    unsigned long long now = sceKernelGetProcessTimeWide();
    int sampled=sceCtrlPeekBufferPositive(0,&pad,1);
    if(sampled<=0){
        /* No fresh controller data: retain held state and still poll touch.
         * Never interpret uninitialized stack data as button presses. */
        pad.buttons=in->prev;
        pad.lx=pad.ly=128;
    }

    /* left stick acts as a d-pad too */
    unsigned b = pad.buttons;
    if (pad.ly < 40) b |= SCE_CTRL_UP;
    if (pad.ly > 215) b |= SCE_CTRL_DOWN;
    if (pad.lx < 40) b |= SCE_CTRL_LEFT;
    if (pad.lx > 215) b |= SCE_CTRL_RIGHT;

    in->pressed = b & ~in->prev;
    if (in->pressed & DPAD) {
        in->repeat_at = now + 380000;
    } else if ((b & DPAD) && now >= in->repeat_at) {
        in->pressed |= b & DPAD;
        in->repeat_at = now + 90000;
    }
    in->held = b;
    in->prev = b;
    in->touch_tap = 0;
    in->touch_dy = 0;
    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    int down = g_touch_enabled && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 &&
               touch.reportNum > 0;
    if (down) {
        int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
        if (x < 0) x = 0;
        if (x >= SCREEN_W) x = SCREEN_W-1;
        if (y < 0) y = 0;
        if (y >= SCREEN_H) y = SCREEN_H-1;
        if (!in->touch_prev_down) {
            in->touch_start_x = x;
            in->touch_start_y = y;
            in->touch_last_y = y;
            in->touch_dragged = 0;
        } else {
            in->touch_dy = y - in->touch_last_y;
            int dx=x-in->touch_start_x, dy=y-in->touch_start_y;
            if (dx*dx+dy*dy > 16*16) in->touch_dragged = 1;
        }
        in->touch_x = x;
        in->touch_y = y;
        in->touch_last_y = y;
    } else if (in->touch_prev_down) {
        in->touch_tap = !in->touch_dragged;
        in->touch_dragged = 0;
    }
    in->touch_down = down;
    in->touch_prev_down = down;
}

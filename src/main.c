/*
 * VitaTwitch
 * Home with Siguiendo / Explorar / Buscar tabs, Twitch login with a device
 * code, and live playback with the hardware decoders.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <vita2d.h>

#include "auth.h"
#include "http.h"
#include "images.h"
#include "log.h"
#include "player.h"
#include "task.h"
#include "twitch.h"
#include "ui.h"
#include "i18n.h"
#include "ime_utf8.h"
#include "playback_policy.h"

int _newlib_heap_size_user = 96 * 1024 * 1024;

#define DATA_DIR      "ux0:data/VitaTwitch"
#define AUTH_FILE     DATA_DIR "/auth.txt"
#define SETTINGS_FILE DATA_DIR "/settings.txt"
#define LOG_FILE      DATA_DIR "/log.txt"
#define LOG_PREV_FILE DATA_DIR "/log_prev.txt"
#define APP_VERSION   "1.33-classic-uifix1"

#define MAX_ITEMS 100

/* Classic home: fixed sidebar, six-tile mosaic in each scrollable block. */
#define GRID_X 224
#define HEADER_H 58
#define FOOTER_Y 506
#define SEARCH_H 44
#define SIDE_X 22
#define SIDE_W 188
#define SIDE_Y 103
#define SIDE_STEP 62
#define SIDE_H 54
#define SIDE_COUNT 6
#define BLOCK_H 410
#define CLASSIC_BG RGBA8(25,18,34,255)
#define CLASSIC_TILE RGBA8(53,38,73,255)
#define CLASSIC_PURPLE RGBA8(100,65,165,255)

static unsigned g_btn_ok = SCE_CTRL_CROSS, g_btn_back = SCE_CTRL_CIRCLE;
static const char *g_ok_name = "X", *g_back_name = "O";

static auth_t g_auth;
static int g_logged;
static task_t g_auth_task;
static int g_auth_checking;
static char g_pref_quality[32];
static char g_search[64];
static int g_chat_on = 1;
/* 0 = automatic UI language, 1 = all, then explicit broadcast languages. */
static const char *const region_codes[]={"AUTO","","PT","EN","ES","FR","DE","IT","RU","JA","KO","ZH","ZH_HK","ID","TL","NL","PL","TR","UK","SV","AR","TH","VI","OTHER"};
static const char *const region_names[]={"Auto","ALL","Português","English","Español","Français","Deutsch","Italiano","Русский","日本語","한국어","中文","中文 (香港)","Bahasa Indonesia","Filipino","Nederlands","Polski","Türkçe","Українська","Svenska","العربية","ไทย","Tiếng Việt","OTHER"};
#define REGION_COUNT ((int)(sizeof(region_codes)/sizeof(region_codes[0])))
static int g_region=0;
static const char *region_code(void){
    static const char *const by_ui[]={"EN","ES","PT","FR","IT","DE","RU","ID","TL","TL","NL","PL","TR","UK","SV","JA","KO","ZH","ZH"};
    int lang=vt_lang_current();
    return g_region?region_codes[g_region]:(lang>=0&&lang<19?by_ui[lang]:"EN");
}
static const char *region_label(int i){
    if(i==0)return vt_lang_current()==2?"Automático":"Auto";
    if(i==1)return vt_lang_current()==2?"Todos os idiomas":"All languages";
    if(i==REGION_COUNT-1)return vt_lang_current()==2?"Outros":"Other";
    return region_names[i];
}
static const char *region_title(void){
    static const char *const labels[]={"Live languages","Idioma de directos","Idioma das lives","Langue des lives","Lingua delle live","Stream-Sprache","Язык трансляций","Bahasa siaran","Wika ng live","Pinulongan sa live","Streamtaal","Język transmisji","Yayın dili","Мова трансляцій","Streams språk","配信言語","방송 언어","直播语言","直播語言"};
    int i=vt_lang_current();return i>=0&&i<19?labels[i]:labels[0];
}
static int g_profile=VT_PROFILE_BALANCED, g_auto_quality=0, g_diag=0, g_volume=100, g_remember=1;
static unsigned long long g_auth_last_check;
static int g_touch_manual_scroll = 0;

static ui_input_t g_in;
static char g_toast[160];
static unsigned long long g_toast_until;

/* ------------------------------------------------------------- tabs */

enum { TAB_FOLLOWED, TAB_EXPLORE, TAB_SEARCH, TAB_COUNT };
static const char *tab_name(int t) {return vt_tr((vt_text_id)(VT_I_FOLLOWING+t));}

typedef enum { LS_EMPTY, LS_LOADING, LS_READY, LS_ERROR } list_state_t;

typedef struct {
    twitch_list_t kind;
    char query[64];
    char oauth[64];
    twitch_stream_t out[MAX_ITEMS];
    int n;
    char err[160];
} list_job_t;

typedef struct {
    list_state_t state;
    twitch_stream_t items[MAX_ITEMS];
    int count;
    int sel;            /* -1 = search bar (Buscar tab) */
    float scroll;
    char err[160];
    int need_login;
    list_job_t job;
    task_t task;
    int busy;
} tab_t;

static tab_t g_tabs[TAB_COUNT];
static int g_tab = TAB_EXPLORE;
static int g_side_focus = -1;
static char g_game_filter[96];

/* ---------------------------------------------------------- helpers */

static void toast(const char *msg)
{
    LOGI("aviso: %s", msg);
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toast_until = sceKernelGetProcessTimeWide() + 3000000ULL;
}

static int net_connected(void)
{
    int st = 0;
    if (sceNetCtlInetGetState(&st) < 0)
        return 1; /* unknown: let the request decide */
    return st == SCE_NETCTL_STATE_CONNECTED;
}

static void ensure_data_dir(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
}

static void settings_load(void)
{
    char line[160];
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f)
        return;
    int quality_flow=0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!strncmp(line, "quality=", 8))
            snprintf(g_pref_quality, sizeof(g_pref_quality), "%s", line + 8);
        else if (!strncmp(line, "search=", 7))
            snprintf(g_search, sizeof(g_search), "%s", line + 7);
        else if (!strncmp(line, "chat=", 5))
            g_chat_on = atoi(line + 5) != 0;
        else if (!strncmp(line, "lang=", 5))
            vt_lang_set_code(line + 5);
        else if (!strncmp(line,"region=",7)) {for(int i=0;i<REGION_COUNT;i++)if(!strcmp(line+7,region_codes[i])){g_region=i;break;}}
        else if (!strncmp(line, "profile=", 8)) {int p=atoi(line+8);if(p>=0&&p<VT_PROFILE_COUNT)g_profile=p;}
        else if (!strncmp(line,"quality_flow=",13)) quality_flow=atoi(line+13);
        else if (!strncmp(line, "auto_quality=", 13)) g_auto_quality=atoi(line+13)!=0;
        else if (!strncmp(line, "diag=", 5)) g_diag=atoi(line+5)!=0;
        else if (!strncmp(line, "volume=", 7)) {int v=atoi(line+7);if(v>=0&&v<=100)g_volume=v;}
        else if (!strncmp(line, "remember=", 9)) g_remember=atoi(line+9)!=0;
        else if (!strncmp(line, "touch=", 6))
            ui_touch_enable(atoi(line + 6) != 0);
    }
    fclose(f);
    if(quality_flow<3)g_auto_quality=0;
}

static void settings_save(void)
{
    ensure_data_dir();
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "quality=%s\nsearch=%s\nchat=%d\nlang=%s\ntouch=%d\n",
            g_pref_quality, g_search, g_chat_on,
            vt_lang_code(vt_lang_current()), ui_touch_enabled());
    fprintf(f,"profile=%d\nauto_quality=%d\ndiag=%d\nvolume=%d\nremember=%d\n",g_profile,g_auto_quality,g_diag,g_volume,g_remember);
    fprintf(f,"region=%s\nquality_flow=3\n",region_codes[g_region]);
    fclose(f);
}

static void frame_begin(void)
{
    img_frame();
    vita2d_start_drawing();
    vita2d_clear_screen();
}

static void frame_end(void)
{
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

/* ------------------------------------------------------- list loads */

static int list_job_run(void *arg, volatile int *abort_flag)
{
    list_job_t *j = arg;
    j->err[0] = 0;
    j->n = twitch_fetch_list(j->kind, j->query, j->oauth, j->out, MAX_ITEMS,
                             j->err, sizeof(j->err), abort_flag);
    return j->n;
}

static void tab_load(int t)
{
    tab_t *tb = &g_tabs[t];
    if (tb->busy)
        return;
    tb->need_login = 0;
    if (t == TAB_FOLLOWED && !g_logged) {
        tb->state = LS_READY;
        tb->count = 0;
        tb->need_login = 1;
        return;
    }
    if (t == TAB_FOLLOWED && g_auth_checking) {
        tb->state = LS_LOADING; /* started once the session check finishes */
        return;
    }
    if (t == TAB_SEARCH && !g_search[0]) {
        tb->state = LS_EMPTY;
        tb->count = 0;
        return;
    }
    if (!net_connected()) {
        tb->state = LS_ERROR;
        snprintf(tb->err, sizeof(tb->err), vt_msg("Sin conexion a Internet. Conectate a una red Wi-Fi."));
        LOGW("%s: sin Wi-Fi", tab_name(t));
        return;
    }
    list_job_t *j = &tb->job;
    j->kind = t == TAB_FOLLOWED ? TW_LIST_FOLLOWED : t == TAB_EXPLORE ? TW_LIST_TOP : TW_LIST_SEARCH;
    snprintf(j->query, sizeof(j->query), "%s", t==TAB_EXPLORE?region_code():g_search);
    snprintf(j->oauth, sizeof(j->oauth), "%s", g_logged ? g_auth.access : "");
    if (task_start(&tb->task, list_job_run, j) == 0) {
        tb->busy = 1;
        if (tb->state != LS_READY)
            tb->state = LS_LOADING;
    }
}

static void tabs_update(void)
{
    for (int t = 0; t < TAB_COUNT; t++) {
        tab_t *tb = &g_tabs[t];
        if (!tb->busy || !task_done(&tb->task))
            continue;
        tb->busy = 0;
        if(t==TAB_EXPLORE && strcmp(tb->job.query,region_code())){tb->state=LS_EMPTY;tb->count=0;tab_load(t);continue;}
        if (tb->job.n >= 0)
            LOGI("%s: %d canales", tab_name(t), tb->job.n);
        else if (tb->job.n == -2)
            LOGI("%s: hace falta iniciar sesion", tab_name(t));
        else if (!tb->task.abort_flag)
            LOGE("%s: %s", tab_name(t), tb->job.err);
        if (tb->job.n >= 0) {
            /* keep the selection on the same channel if it is still there */
            char cur[32] = "";
            if (tb->sel >= 0 && tb->sel < tb->count)
                snprintf(cur, sizeof(cur), "%s", tb->items[tb->sel].login);
            memcpy(tb->items, tb->job.out, sizeof(twitch_stream_t) * (size_t)tb->job.n);
            tb->count = tb->job.n;
            int sel = t == TAB_SEARCH ? tb->sel : 0;
            for (int i = 0; i < tb->count && cur[0]; i++)
                if (!strcmp(tb->items[i].login, cur))
                    sel = i;
            if (sel >= tb->count)
                sel = tb->count - 1;
            if (sel < 0 && t != TAB_SEARCH)
                sel = 0;
            tb->sel = sel;
            tb->state = LS_READY;
        } else if (tb->job.n == -2) {
            tb->state = LS_READY;
            tb->count = 0;
            tb->need_login = 1;
        } else {
            snprintf(tb->err, sizeof(tb->err), "%s", tb->job.err);
            if (tb->state == LS_READY && tb->count > 0)
                toast(tb->err); /* keep showing the old list */
            else
                tb->state = LS_ERROR;
        }
    }
}

/* ----------------------------------------------------- auth checking */

typedef struct {
    auth_t a;
    char err[160];
} auth_job_t;
static auth_job_t g_auth_job;

static int auth_check_run(void *arg, volatile int *abort_flag)
{
    auth_job_t *j = arg;
    return auth_ensure_valid(&j->a, j->err, sizeof(j->err), abort_flag);
}

static void auth_update(void)
{
    if (!g_auth_checking || !task_done(&g_auth_task))
        return;
    g_auth_checking = 0;
    g_auth_last_check=sceKernelGetProcessTimeWide();
    int r = g_auth_task.result;
    if (r == 0)
        LOGI("sesion valida: @%s", g_auth_job.a.login);
    else
        LOGE("comprobacion de sesion fallida (%d): %s", r, g_auth_job.err);
    if (r == 0) {
        g_auth = g_auth_job.a;
        if(g_remember)auth_save(&g_auth, AUTH_FILE);
    } else if (r == -1) {
        g_logged = 0;
        memset(&g_auth, 0, sizeof(g_auth));
        sceIoRemove(AUTH_FILE);
        toast(g_auth_job.err);
    }
    if (g_tabs[TAB_FOLLOWED].state == LS_LOADING || g_tab == TAB_FOLLOWED)
        tab_load(TAB_FOLLOWED);
}

/* ---------------------------------------------------------- drawing */

static int grid_top(int t)
{
    return t == TAB_SEARCH ? HEADER_H + SEARCH_H + 20 : HEADER_H + 10;
}

typedef struct { int x,y,w,h; } home_rect_t;
static home_rect_t home_rect(int i)
{
    static const home_rect_t tiles[6] = {
        {224,0,476,266}, {708,0,236,129}, {708,137,236,129},
        {224,274,236,128}, {468,274,232,128}, {708,274,236,128}
    };
    home_rect_t r=tiles[i%6]; r.y+=(i/6)*BLOCK_H; return r;
}

static int home_count(const tab_t *tb)
{
    if(tb->state!=LS_READY)return 0;
    if(g_tab!=TAB_EXPLORE||!g_game_filter[0])return tb->count;
    int n=0;for(int i=0;i<tb->count;i++)if(!strcmp(tb->items[i].game,g_game_filter))n++;
    return n;
}
static int home_index(const tab_t *tb,int pos)
{
    if(pos<0)return -1;
    if(g_tab!=TAB_EXPLORE||!g_game_filter[0])return pos<tb->count?pos:-1;
    for(int i=0;i<tb->count;i++)if(!strcmp(tb->items[i].game,g_game_filter)&&pos--==0)return i;
    return -1;
}
static int home_pos(const tab_t *tb)
{
    int n=home_count(tb);for(int i=0;i<n;i++)if(home_index(tb,i)==tb->sel)return i;
    return -1;
}
static int home_move(int pos,int n,int dx,int dy)
{
    if(pos<0)return n?0:-1;
    home_rect_t a=home_rect(pos);
    int ax=a.x+a.w/2,ay=a.y+a.h/2,best=-1,score=0x7fffffff;
    for(int i=0;i<n;i++){
        if(i==pos)continue;
        home_rect_t b=home_rect(i);
        int x=b.x+b.w/2-ax,y=b.y+b.h/2-ay;
        int primary=dx?x*dx:y*dy, cross=dx?abs(y):abs(x);
        if(primary<=0)continue;
        /* Prefer an overlapping row/column, then proximity. */
        int overlap=dx?(a.y<b.y+b.h&&b.y<a.y+a.h):(a.x<b.x+b.w&&b.x<a.x+a.w);
        if(dx&&!overlap)continue;
        int s=primary+cross*3+(overlap?0:1200);
        if(s<score){score=s;best=i;}
    }
    return best;
}
static const char *games_label(void)
{
    static const char *const names[]={"Games","Juegos","Jogos","Jeux","Giochi","Spiele","Игры","Gim","Mga laro","Mga dula","Games","Gry","Oyunlar","Ігри","Spel","ゲーム","게임","游戏","遊戲"};
    int l=vt_lang_current();return names[l>=0&&l<VT_LANG_COUNT?l:0];
}
static const char *channels_label(void)
{
    static const char *const names[]={"Channels","Canales","Canais","Chaînes","Canali","Kanäle","Каналы","Kanal","Mga channel","Mga channel","Kanalen","Kanały","Kanallar","Канали","Kanaler","チャンネル","채널","频道","頻道"};
    int l=vt_lang_current();return names[l>=0&&l<VT_LANG_COUNT?l:0];
}
static int side_active(void)
{
    return g_tab==TAB_SEARCH?1:g_tab==TAB_FOLLOWED?2:g_game_filter[0]?3:4;
}
static const char *side_label(int i)
{
    switch(i){
    case 0:return vt_tr(VT_I_ACCOUNT);
    case 1:return vt_tr(VT_I_SEARCH);
    case 2:return vt_tr(VT_I_FOLLOWING);
    case 3:return games_label();
    case 4:return channels_label();
    default:return vt_tr(VT_I_SETTINGS);
    }
}

static void draw_avatar(const char *url, const char *name, float x, float y, float size)
{
    vita2d_texture *t = img_get(url);
    if (t) {
        ui_image(t, x, y, size, size);
        return;
    }
    vita2d_draw_fill_circle(x + size / 2, y + size / 2, size / 2, C_PANEL2);
    char ini[5]="?";
    if(name&&name[0]){
        unsigned char first=(unsigned char)name[0];
        int len=first<128?1:first>=0xC2&&first<=0xDF?2:first>=0xE0&&first<=0xEF?3:first>=0xF0&&first<=0xF4?4:0;
        if(len&&(int)strlen(name)>=len){memcpy(ini,name,(size_t)len);ini[len]=0;if(len==1&&ini[0]>='a'&&ini[0]<='z')ini[0]-=32;}
    }
    ui_text_center((int)(x + size / 2), (int)(y + size / 2 + size * 0.17f), C_GREY, size / 40.0f, ini);
}

static void draw_card(const twitch_stream_t *s, home_rect_t r, int selected)
{
    int x=r.x,y=r.y,w=r.w,h=r.h;
    char line[256],v[32],second[180];
    vita2d_draw_rectangle(x,y,w,h,CLASSIC_TILE);
    vita2d_texture *tex=s->live?img_get(s->preview):NULL;
    if(tex){
        float tw=vita2d_texture_get_width(tex),th=vita2d_texture_get_height(tex);
        if(tw>0&&th>0){
            float scale=w/tw;if(h/th>scale)scale=h/th;
            float sw=w/scale,sh=h/scale;
            vita2d_draw_texture_part_scale(tex,x,y,(tw-sw)/2,(th-sh)/2,sw,sh,scale,scale);
        }
    }else{
        draw_avatar(s->avatar,s->display,x+w/2-27,y+(h-(w>300?64:48))/2-27,54);
    }
    int strip=w>300?64:48;
    vita2d_draw_rectangle(x,y+h-strip,w,strip,RGBA8(15,9,24,220));
    ui_clean_text(s->display,line,sizeof(line));
    ui_text_fit(x+10,y+h-strip+(w>300?26:19),w-20,C_WHITE,w>300?1.08f:0.83f,line);
    if(s->live){
        ui_fmt_viewers(s->viewers,v,sizeof(v));
        snprintf(second,sizeof(second),"%s%s%s",v,s->game[0]?"  /  ":"",s->game);
    }else snprintf(second,sizeof(second),"%s",vt_tr(VT_I_OFFLINE));
    ui_clean_text(second,line,sizeof(line));
    ui_text_fit(x+10,y+h-9,w-20,RGBA8(203,184,235,255),w>300?0.84f:0.72f,line);
    if(s->live){
        int bw=ui_text_w(0.7f,vt_tr(VT_I_LIVE))+28;if(bw>w-16)bw=w-16;
        vita2d_draw_rectangle(x+8,y+5,bw,22,C_SHADE);
        vita2d_draw_rectangle(x+13,y+12,8,8,C_RED);
        ui_text_fit(x+26,y+21,bw-21,C_WHITE,0.7f,vt_tr(VT_I_LIVE));
    }
    if(selected)ui_frame(x,y,w,h,2,C_WHITE);
}

static void draw_header(void)
{
    /* Draw sidebar after cards so offscreen tiles cannot overlap it. */
    vita2d_draw_rectangle(0,0,GRID_X-5,SCREEN_H,CLASSIC_BG);
    vita2d_draw_rectangle(GRID_X-5,0,SCREEN_W-GRID_X+5,HEADER_H,CLASSIC_BG);
    /* Existing native Twitch Glitch silhouette, no extra GPU texture. */
    static const unsigned int glitch[24]={
        0x3fffe0u,0x3ffff0u,0x200078u,0x20007cu,0x20007cu,0x23107cu,0x23107cu,0x23107cu,0x23107cu,0x23107cu,0x20007cu,0x30007cu,0x38007cu,0x1c007cu,0xe007cu,0x7e7fcu,0x3f7fcu,0x1fffcu,0xfffcu,0x7ffcu,0x780u,0x380u,0x180u,0x80u
    };
    for(int gy=0;gy<24;gy++)for(int gx=0;gx<24;){
        unsigned mask=glitch[gy];
        if(!(mask&(1u<<gx))){gx++;continue;}
        int end=gx+1;while(end<24&&(mask&(1u<<end)))end++;
        vita2d_draw_rectangle(28+gx*1.45f,22+gy*1.45f,(end-gx)*1.45f,1.45f,C_WHITE);gx=end;
    }
    ui_text(70,52,C_WHITE,1.65f,"Twitch");
    ui_text_fit(SIDE_X,78,SIDE_W,C_GREY,0.62f,"TwitchonVita");
    int active=side_active();
    for(int i=0;i<SIDE_COUNT;i++){
        int y=SIDE_Y+i*SIDE_STEP;
        vita2d_draw_rectangle(SIDE_X,y,SIDE_W,SIDE_H,i==active?RGBA8(120,80,189,255):CLASSIC_PURPLE);
        if(g_side_focus==i)ui_frame(SIDE_X,y,SIDE_W,SIDE_H,2,C_WHITE);
        int label_w=SIDE_W-24;
        if(i==2&&g_logged&&g_tabs[TAB_FOLLOWED].state==LS_READY){
            char n[16];snprintf(n,sizeof(n),"%d",g_tabs[TAB_FOLLOWED].count);
            int nw=ui_text_w(0.8f,n);
            ui_text(SIDE_X+SIDE_W-nw-10,y+33,C_WHITE,0.8f,n);label_w-=nw+12;
        }
        ui_text_fit(SIDE_X+12,y+33,label_w,C_WHITE,0.96f,side_label(i));
    }
    const char *heading=g_tab==TAB_EXPLORE&&g_game_filter[0]?g_game_filter:side_label(active);
    ui_text_fit(GRID_X,40,(g_logged?780:SCREEN_W-16)-GRID_X,C_WHITE,1.32f,heading);
    if(g_logged){
        char name[80];ui_clean_text(g_auth.display[0]?g_auth.display:g_auth.login,name,sizeof(name));
        ui_text_fit(796,39,102,C_GREY,0.72f,name);
        draw_avatar(g_auth.avatar,name,906,17,28);
    }
}

/* Fixed footer cells keep long localized labels inside the 960px display. */
static void footer_hint(int x,int width,int y,const char *btn,const char *label)
{
    int bw=ui_text_w(0.70f,btn)+10;if(bw<22)bw=22;
    if(bw>width-12)bw=width-12;
    ui_round_rect(x,y-16,bw,22,4,C_PANEL2);
    ui_text_fit(x+5,y,bw-10,C_WHITE,0.70f,btn);
    ui_text_fit(x+bw+5,y,width-bw-10,C_GREY,0.76f,label);
}
static void draw_footer(void)
{
    tab_t *tb=&g_tabs[g_tab];
    vita2d_draw_rectangle(0,FOOTER_Y,SCREEN_W,SCREEN_H-FOOTER_Y,C_SURFACE);
    vita2d_draw_rectangle(0,FOOTER_Y,SCREEN_W,1,C_PANEL2);
    const char *ok=g_side_focus>=0?side_label(g_side_focus):
        g_tab==TAB_SEARCH&&tb->sel<0?vt_tr(VT_I_SEARCH):vt_tr(VT_I_PLAY);
    const char *btns[]={g_ok_name,"△","□","L/R","SELECT","START"};
    const char *labels[]={ok,vt_tr(VT_I_REFRESH),vt_msg("Ir al canal exacto"),vt_tr(VT_I_TAB),vt_tr(VT_I_MENU),vt_tr(VT_I_EXIT)};
    int count=g_tab==TAB_SEARCH?6:5,cell=(SCREEN_W-24)/count,pos=0;
    for(int i=0;i<6;i++){if(i==2&&g_tab!=TAB_SEARCH)continue;footer_hint(12+pos++*cell,cell,FOOTER_Y+25,btns[i],labels[i]);}
}

static void draw_center_msg(const char *title, const char *msg, int top)
{
    int cx=(GRID_X+SCREEN_W)/2,cy=top+(FOOTER_Y-top)/2;
    int w=SCREEN_W-GRID_X-36;
    int tw=ui_text_w(1.02f,title);if(tw>w)tw=w;
    ui_text_fit(cx-tw/2,cy-10,tw,C_WHITE,1.02f,title);
    if(msg){int mw=ui_text_w(0.82f,msg);if(mw>w)mw=w;ui_text_fit(cx-mw/2,cy+22,mw,C_GREY,0.82f,msg);}
}

static void draw_home(void)
{
    tab_t *tb=&g_tabs[g_tab];
    int top=grid_top(g_tab),n=home_count(tb),pos=home_pos(tb);
    char buf[160];
    vita2d_draw_rectangle(0,0,SCREEN_W,SCREEN_H,CLASSIC_BG);
    float max=0;
    if(n){int bottom=0;for(int i=(n-1)/6*6;i<n;i++){home_rect_t r=home_rect(i);if(r.y+r.h>bottom)bottom=r.y+r.h;}
        max=bottom-(FOOTER_Y-top-8);if(max<0)max=0;}
    if(!g_touch_manual_scroll&&g_side_focus<0){
        float target=tb->scroll;
        if(pos>=0){home_rect_t r=home_rect(pos);if(r.y<target)target=r.y;if(r.y+r.h>target+FOOTER_Y-top-8)target=r.y+r.h-(FOOTER_Y-top-8);}
        else target=0;
        if(target<0)target=0;
        if(target>max)target=max;
        tb->scroll+=(target-tb->scroll)*0.35f;
        if(tb->scroll-target<0.5f&&target-tb->scroll<0.5f)tb->scroll=target;
    }
    if(tb->scroll<0)tb->scroll=0;
    if(tb->scroll>max)tb->scroll=max;
    if(n){
        for(int i=0;i<n;i++){
            int idx=home_index(tb,i);if(idx<0)continue;
            home_rect_t r=home_rect(i);r.y+=top-(int)tb->scroll;
            if(r.y+r.h<=top||r.y>=FOOTER_Y){
                if(r.y+r.h>=top-BLOCK_H&&r.y<=FOOTER_Y+BLOCK_H&&tb->items[idx].live)img_prefetch(tb->items[idx].preview);
                continue;
            }
            draw_card(&tb->items[idx],r,g_side_focus<0&&idx==tb->sel);
        }
    }else if(tb->need_login){
        draw_center_msg(vt_msg("Inicia sesion para ver los canales que sigues"),vt_tr(VT_I_ACCOUNT),top);
    }else if(tb->state==LS_LOADING){
        ui_spinner((GRID_X+SCREEN_W)/2,top+(FOOTER_Y-top)/2,16,C_PURPLE);
    }else if(tb->state==LS_ERROR){
        draw_center_msg(vt_msg("No se pudo cargar"),tb->err,top);
    }else if(g_tab==TAB_SEARCH&&!g_search[0]){
        snprintf(buf,sizeof(buf),vt_msg("Pulsa %s para buscar un canal por nombre"),g_ok_name);
        draw_center_msg(vt_msg("Busca canales de Twitch"),buf,top);
    }else if(tb->state==LS_READY){
        draw_center_msg(g_tab==TAB_FOLLOWED?vt_msg("Ninguno de tus canales esta en directo"):g_tab==TAB_SEARCH?vt_msg("Sin resultados"):vt_msg("No hay directos"),vt_msg("Pulsa Triangulo para actualizar"),top);
    }
    /* Opaque header and footer mask scrolled cards at both edges. */
    draw_header();
    if(g_tab==TAB_SEARCH){
        int y=HEADER_H+5,w=SCREEN_W-GRID_X-16;
        vita2d_draw_rectangle(GRID_X-5,HEADER_H,SCREEN_W-GRID_X+5,top-HEADER_H-2,CLASSIC_BG);
        vita2d_draw_rectangle(GRID_X,y,w,SEARCH_H,CLASSIC_TILE);
        if(tb->sel<0&&g_side_focus<0)ui_frame(GRID_X,y,w,SEARCH_H,2,C_WHITE);
        ui_text_fit(GRID_X+15,y+29,w-30,g_search[0]?C_WHITE:C_GREY,1.0f,g_search[0]?g_search:vt_tr(VT_I_SEARCH));
    }else vita2d_draw_rectangle(GRID_X-4,HEADER_H,SCREEN_W-GRID_X+4,top-HEADER_H-3,CLASSIC_BG);
    if(tb->busy&&n)ui_spinner(SCREEN_W-14,HEADER_H+3,5,C_PURPLE);
    if(max>0){
        float track=FOOTER_Y-top-4,thumb=track*(FOOTER_Y-top-8)/(max+FOOTER_Y-top-8);if(thumb<24)thumb=24;
        vita2d_draw_rectangle(951,top+2,3,track,C_PANEL2);
        vita2d_draw_rectangle(951,top+2+(track-thumb)*tb->scroll/max,3,thumb,CLASSIC_PURPLE);
    }
    draw_footer();
    if(g_toast[0]&&sceKernelGetProcessTimeWide()<g_toast_until){
        int w=ui_text_w(0.9f,g_toast)+40;if(w>SCREEN_W-40)w=SCREEN_W-40;
        ui_round_rect((SCREEN_W-w)/2,FOOTER_Y-56,w,40,8,C_PANEL2);
        ui_text_fit((SCREEN_W-w)/2+20,FOOTER_Y-30,w-40,C_WHITE,0.9f,g_toast);
    }
}

/* Dark box in the middle of the screen; returns its top-left. */
static void draw_modal_box(int w, int h, int *ox, int *oy)
{
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, C_SCRIM);
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    ui_round_rect(x, y, w, h, 12, C_SURFACE);
    vita2d_draw_rectangle(x + 12, y, w - 24, 3, C_PURPLE);
    *ox = x;
    *oy = y;
}

/* ----------------------------------------------------------- modals */

static void wrap_text(int x, int y, int max_w, unsigned color, float scale, const char *s, int max_lines)
{
    if(!s||max_w<=0)return;
    char line[256];const char *p=s;
    for(int ln=0;ln<max_lines&&*p;ln++){
        while(*p==' ')p++;
        size_t n=0,best=0,space=0;
        while(p[n]&&p[n]!='\n'){
            size_t next=n+1;
            while(p[next]&&((unsigned char)p[next]&0xC0)==0x80)next++;
            if(next>=sizeof(line))break;
            memcpy(line,p,next);line[next]=0;
            if(ui_text_w(scale,line)>max_w)break;
            if(p[n]==' ')space=n;
            best=next;n=next;
        }
        if(!best&&*p&&*p!='\n'){best=1;while(p[best]&&((unsigned char)p[best]&0xC0)==0x80)best++;}
        if(p[best]&&p[best]!='\n'&&space)best=space;
        if(ln==max_lines-1&&p[best]){ui_text_fit(x,y+ln*(int)(28*scale),max_w,color,scale,p);return;}
        memcpy(line,p,best);line[best]=0;
        ui_text_fit(x,y+ln*(int)(28*scale),max_w,color,scale,line);
        p+=best;if(*p=='\n')p++;
    }
}

static void modal_message(const char *title, const char *msg)
{
    char hint[32];
    snprintf(hint, sizeof(hint), vt_msg("Aceptar"));
    LOGE("mensaje en pantalla: %s: %s", title, msg);
    for (;;) {
        ui_input_poll(&g_in);
        if ((g_in.pressed & (g_btn_ok | g_btn_back)) || ui_touch_hit(&g_in,590,353,155,31))
            return;
        frame_begin();
        draw_home();
        int x, y;
        draw_modal_box(560, 210, &x, &y);
        ui_text_fit(x + 28, y + 48, 504, C_WHITE, 1.2f, title);
        wrap_text(x + 28, y + 90, 504, C_GREY, 0.95f, msg, 3);
        ui_hint(x + 560 - 140, y + 210 - 22, g_ok_name, hint);
        frame_end();
    }
}

/* Runs a task while showing a spinner over the home screen.
 * Returns 0 when it finished, -1 if the user cancelled. */
static int run_with_spinner(task_t *t, task_fn fn, void *arg, const char *msg)
{
    if (task_start(t, fn, arg) < 0)
        return -1;
    for (;;) {
        ui_input_poll(&g_in);
        if (g_in.pressed & g_btn_back) {
            task_cancel(t);
            /* keep drawing while the worker notices the cancellation */
            while (!task_done(t)) {
                frame_begin(); draw_home(); frame_end();
            }
            return -1;
        }
        if (task_done(t))
            return 0;
        tabs_update();
        frame_begin();
        draw_home();
        int x, y;
        draw_modal_box(460, 150, &x, &y);
        ui_spinner(x + 50, y + 62, 14, C_PURPLE);
        ui_text_fit(x + 84, y + 70, 460 - 110, C_WHITE, 1.0f, msg);
        ui_hint(x + 460 - 130, y + 150 - 20, g_back_name, vt_msg("Cancelar"));
        frame_end();
    }
}

static int ime_input(const char *title, const char *initial, char *out, size_t cap, int max_len)
{
    static SceWChar16 title16[64], init16[128], buf16[128];
    vt_utf8_to_utf16((uint16_t*)title16,64,title);
    vt_utf8_to_utf16((uint16_t*)init16,128,initial);
    memset(buf16, 0, sizeof(buf16));

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages =
        SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_SPANISH |
        SCE_IME_LANGUAGE_PORTUGUESE_BR | SCE_IME_LANGUAGE_FRENCH |
        SCE_IME_LANGUAGE_GERMAN | SCE_IME_LANGUAGE_ITALIAN |
        SCE_IME_LANGUAGE_RUSSIAN | SCE_IME_LANGUAGE_JAPANESE |
        SCE_IME_LANGUAGE_KOREAN | SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE |
        SCE_IME_LANGUAGE_TRADITIONAL_CHINESE | SCE_IME_LANGUAGE_TURKISH;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION | SCE_IME_OPTION_NO_ASSISTANCE;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = title16;
    param.maxTextLength = (SceUInt32)max_len;
    param.initialText = init16;
    param.inputTextBuffer = buf16;

    if (sceImeDialogInit(&param) < 0)
        return 0;

    for (;;) {
        frame_begin();
        draw_home();
        frame_end();
        if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED)
            break;
    }

    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ui_input_poll(&g_in); /* swallow the button that closed the dialog */

    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER)
        return 0;
    size_t n = vt_utf16_to_utf8(out,cap,(const uint16_t*)buf16,128);
    /* trim */
    while (n && out[n - 1] == ' ')
        out[--n] = 0;
    size_t s = 0;
    while (out[s] == ' ')
        s++;
    memmove(out, out + s, n - s + 1);
    return 1;
}

/* ---------------------------------------------------------- quality */

/* ------------------------------------------------------------- play */

typedef struct {
    char login[32];
    char oauth[64];
    m3u8_variant_t v[M3U8_MAX_VARIANTS];
    int n;
    char err[256];
} variants_job_t;

static int variants_run(void *arg, volatile int *abort_flag)
{
    variants_job_t *j = arg;
    j->err[0] = 0;
    j->n = twitch_get_variants(j->login, j->oauth, j->v, M3U8_MAX_VARIANTS, j->err, sizeof(j->err), abort_flag);
    return j->n;
}

/* Channel opening and manual quality selection are states in main(), never
 * nested UI loops. The player is entered only after a separate input frame. */
static variants_job_t play_job;
static task_t play_task;
static m3u8_variant_t *play_list[M3U8_MAX_VARIANTS];
static int play_count,play_sel,play_top;
static char play_login[64],play_display[96],play_title[256];
enum { PLAY_IDLE, PLAY_FETCH, PLAY_CANCEL, PLAY_CHOOSE, PLAY_LAUNCH };
static int play_state;
static int play_armed;

static void play_channel(const char *login,const char *display,const char *title)
{
    if(play_state!=PLAY_IDLE)return;
    if(!net_connected()){
        modal_message(vt_msg("Sin conexion"),vt_msg("Conecta la Vita a una red Wi-Fi e intentalo de nuevo."));
        return;
    }
    snprintf(play_login,sizeof(play_login),"%s",login?login:"");
    ui_clean_text(display&&display[0]?display:play_login,play_display,sizeof(play_display));
    ui_clean_text(title?title:"",play_title,sizeof(play_title));
    memset(&play_job,0,sizeof(play_job));
    snprintf(play_job.login,sizeof(play_job.login),"%s",play_login);
    snprintf(play_job.oauth,sizeof(play_job.oauth),"%s",g_logged?g_auth.access:"");
    if(task_start(&play_task,variants_run,&play_job)<0){
        toast(vt_tr(VT_I_BACK));return;
    }
    play_state=PLAY_FETCH;play_armed=0;
    LOGI("quality flow: fetching %s",play_login);
}

static void play_selected(void)
{
    static char err[256];
    /* Keep player/reconnect working data off the small UI stack. */




    int sel=play_sel;
    const char *login=play_login,*display=play_display,*title=play_title;
    if(sel<0||sel>=play_count){play_state=PLAY_IDLE;return;}
    play_state=PLAY_IDLE;
    snprintf(g_pref_quality, sizeof(g_pref_quality), "%s", play_list[sel]->name);
    settings_save();

    char clean_title[200];
    ui_clean_text(title ? title : "", clean_title, sizeof(clean_title));
    player_params_t pp;
    int retries=0, reductions=0, r=0, ad_switched=0;
    static char ad_backup[M3U8_URL_MAX];
    ad_backup[0]=0;
    char wanted[48];
    static m3u8_variant_t previous;
    for(;;){
    memset(&pp, 0, sizeof(pp));
    pp.url = play_list[sel]->url;
    pp.channel = display && display[0] ? display : login;
    pp.title = clean_title;
    pp.quality = play_list[sel]->name;
    pp.width = play_list[sel]->width;
    pp.height = play_list[sel]->height;
    pp.fps = (float)play_list[sel]->fps;
    pp.font = ui_font;
    pp.btn_back = g_btn_back;
    pp.login = play_job.login;
    pp.chat_nick = g_logged ? g_auth.login : NULL;
    pp.chat_oauth = g_logged ? g_auth.access : NULL;
    pp.chat_on = &g_chat_on;
    pp.profile=g_profile;pp.auto_quality=g_auto_quality;
    pp.has_lower=vt_profile_lower(play_list,play_count,play_list[sel])>=0;
    pp.diagnostics=g_diag;pp.volume=&g_volume;
    pp.ad_backup_url=ad_switched?NULL:ad_backup;
    LOGI("startup: selected %s %s %dx%d %.0f fps", login, pp.quality, pp.width, pp.height, pp.fps);
    scePowerSetArmClockFrequency(vt_profile_clock(g_profile,pp.height,pp.fps));
    LOGI("startup: entering player");
    r = player_run(&pp, err, sizeof(err));
    LOGI("quality flow: player returned %d",r);
    scePowerSetArmClockFrequency(333);
    settings_save();
    if(r==PLAYER_AD_SWITCH && !ad_switched && ad_backup[0]){
        ad_switched=1;
        snprintf(play_list[sel]->url,sizeof(play_list[sel]->url),"%s",ad_backup);
        LOGI("player: reopening on verified alternate HLS session");
        continue;
    }
    if(r!=PLAYER_RECONNECT&&r!=PLAYER_LOWER_QUALITY)break;
    if(r==PLAYER_RECONNECT&&++retries>2){r=-1;break;}
    if(r==PLAYER_LOWER_QUALITY&&++reductions>4){r=-1;break;}
    previous=*play_list[sel];
    snprintf(wanted,sizeof(wanted),"%s",previous.name);
    const int lower=(r==PLAYER_LOWER_QUALITY);
    memset(&play_job,0,sizeof(play_job));
    snprintf(play_job.login,sizeof(play_job.login),"%s",login);
    snprintf(play_job.oauth,sizeof(play_job.oauth),"%s",g_logged?g_auth.access:"");
    if(run_with_spinner(&play_task,variants_run,&play_job,vt_opt(lower?VT_O_LOWER:VT_O_RECONNECT))<0){r=0;break;}
    if(play_job.n<=0){snprintf(err,sizeof(err),"%s",play_job.err);r=-1;break;}
    play_count=0;
    for(int i=0;i<play_job.n;i++)if(!play_job.v[i].audio_only&&play_job.v[i].height>0&&play_job.v[i].height<=720)play_list[play_count++]=&play_job.v[i];
    if(!play_count)for(int i=0;i<play_job.n;i++)if(!play_job.v[i].audio_only&&play_job.v[i].height>0&&play_job.v[i].height<=1080)play_list[play_count++]=&play_job.v[i];
    if(!play_count){snprintf(err,sizeof(err),"%s",vt_msg("El stream no tiene calidades de video compatibles."));r=-1;break;}
    sel=-1;
    if(lower)sel=vt_profile_lower(play_list,play_count,&previous);
    else for(int i=0;i<play_count;i++)if(!strcmp(wanted,play_list[i]->name)){sel=i;break;}
    if(sel<0&&!lower)sel=vt_profile_pick(play_list,play_count,g_profile);
    if(sel<0){snprintf(err,sizeof(err),"%s",vt_msg("Reproduccion detenida"));r=-1;break;}
    }
    ui_input_poll(&g_in);
    if (r < 0)
        modal_message(vt_msg("Reproduccion detenida"), err);
    /* viewer counts / thumbnails are stale after watching */
    tab_load(g_tab);




}

static void quality_draw(void)
{
    const int x=210,w=540,row=48,vis=play_count<6?play_count:6;
    const int h=150+vis*row,y=(SCREEN_H-h)/2,ly=y+78;
    vita2d_draw_rectangle(x,y,w,h,C_SURFACE);
    vita2d_draw_rectangle(x,y,w,3,C_PURPLE);
    ui_text_fit(x+24,y+33,w-48,C_WHITE,1.12f,play_display);
    ui_text(x+24,y+60,C_GREY,0.87f,vt_tr(VT_I_QUALITY));
    for(int k=0;k<vis&&play_top+k<play_count;k++){
        int i=play_top+k,ry=ly+k*row;
        char info[96];
        vita2d_draw_rectangle(x+16,ry,w-32,row-4,i==play_sel?C_PURPLE:C_PANEL);
        ui_text_fit(x+28,ry+29,136,C_WHITE,0.98f,play_list[i]->name);
        snprintf(info,sizeof(info),"%dx%d / %.0f fps",play_list[i]->width,play_list[i]->height,play_list[i]->fps);
        ui_text_fit(x+176,ry+29,w-200,C_WHITE,0.82f,info);
    }
    vita2d_draw_rectangle(x+16,y+h-53,w/2-24,43,C_PANEL2);
    vita2d_draw_rectangle(x+w/2+8,y+h-53,w/2-24,43,C_PANEL2);
    ui_text(x+30,y+h-25,C_WHITE,0.9f,vt_tr(VT_I_PLAY));
    ui_text(x+w/2+20,y+h-25,C_WHITE,0.9f,vt_tr(VT_I_BACK));
}

static void play_tick(void)
{
    unsigned p=g_in.pressed;
    if(play_state==PLAY_LAUNCH){
        if(p&(g_btn_back|SCE_CTRL_START|SCE_CTRL_SELECT)){
            play_state=PLAY_IDLE;return;
        }
        /* Present loading before calling the blocking player startup. */
        vita2d_start_drawing();vita2d_clear_screen();
        ui_spinner(SCREEN_W/2,SCREEN_H/2-32,14,C_PURPLE);
        ui_text_center(SCREEN_W/2,SCREEN_H/2+12,C_WHITE,1.0f,vt_tr(VT_I_LOADING));
        {int tw=ui_text_w(0.85f,play_display);if(tw>SCREEN_W-80)tw=SCREEN_W-80;ui_text_fit((SCREEN_W-tw)/2,SCREEN_H/2+42,tw,C_GREY,0.85f,play_display);}
        frame_end();
        play_selected();
        return;
    }
    if(play_state==PLAY_FETCH||play_state==PLAY_CANCEL){
        if(play_state==PLAY_FETCH && (p&(g_btn_back|SCE_CTRL_START))){
            task_cancel(&play_task);play_state=PLAY_CANCEL;
        }
        if(task_done(&play_task)){
            if(play_state==PLAY_CANCEL){play_state=PLAY_IDLE;return;}
            if(play_job.n<=0||play_job.n>M3U8_MAX_VARIANTS){
                play_state=PLAY_IDLE;toast(play_job.err[0]?play_job.err:vt_msg("No se encontraron calidades en la playlist"));return;
            }
            play_count=0;
            for(int i=0;i<play_job.n;i++)
                if(!play_job.v[i].audio_only&&play_job.v[i].height>0&&play_job.v[i].height<=720)
                    play_list[play_count++]=&play_job.v[i];
            if(!play_count){
                for(int i=0;i<play_job.n;i++)
                    if(!play_job.v[i].audio_only&&play_job.v[i].height>0&&play_job.v[i].height<=1080)
                        play_list[play_count++]=&play_job.v[i];
            }
            if(!play_count){play_state=PLAY_IDLE;toast(vt_msg("El stream no tiene calidades de video compatibles."));return;}
            play_sel=vt_profile_pick(play_list,play_count,g_profile);
            if(play_sel<0||play_sel>=play_count)play_sel=0;
            if(!g_auto_quality)
                for(int i=0;i<play_count;i++)
                    if(g_pref_quality[0]&&!strcmp(g_pref_quality,play_list[i]->name)){play_sel=i;break;}
            play_top=play_sel>=6?play_sel-5:0;
            play_armed=0;
            play_state=g_auto_quality?PLAY_LAUNCH:PLAY_CHOOSE;
            LOGI("startup: %d variants; state %d; auto=%d",play_count,play_state,g_auto_quality);
            return;
        }
    }else if(play_state==PLAY_CHOOSE){
        const int x=210,w=540,row=48,vis=play_count<6?play_count:6;
        const int h=150+vis*row,y=(SCREEN_H-h)/2,ly=y+78;
        if(!(g_in.held&(g_btn_ok|g_btn_back))&&!g_in.touch_down)play_armed=1;
        if(p&(g_btn_back|SCE_CTRL_START|SCE_CTRL_SELECT)){
            play_state=PLAY_IDLE;LOGI("quality flow: cancelled");return;
        }
        if(p&(SCE_CTRL_UP|SCE_CTRL_LEFT))play_sel=(play_sel+play_count-1)%play_count;
        if(p&(SCE_CTRL_DOWN|SCE_CTRL_RIGHT))play_sel=(play_sel+1)%play_count;
        if(play_sel<play_top)play_top=play_sel;
        if(play_sel>=play_top+vis)play_top=play_sel-vis+1;
        if(play_armed&&(p&g_btn_ok)){play_state=PLAY_LAUNCH;return;}
        if(play_armed&&g_in.touch_tap){
            if(ui_touch_hit(&g_in,x+w/2+8,y+h-53,w/2-24,43)){play_state=PLAY_IDLE;return;}
            if(ui_touch_hit(&g_in,x+16,y+h-53,w/2-24,43)){play_state=PLAY_LAUNCH;return;}
            for(int k=0;k<vis&&play_top+k<play_count;k++)
                if(ui_touch_hit(&g_in,x+16,ly+k*row,w-32,row-4)){play_sel=play_top+k;play_state=PLAY_LAUNCH;return;}
        }
    }
    /* Same rendering/swap path as the working main screen, no image-cache
     * locks, explicit GPU waits or background rendering while choosing. */
    vita2d_start_drawing();vita2d_clear_screen();
    if(play_state==PLAY_CHOOSE)quality_draw();
    else{
        ui_spinner(SCREEN_W/2,SCREEN_H/2-32,14,C_PURPLE);
        ui_text_center(SCREEN_W/2,SCREEN_H/2+12,C_WHITE,1.0f,vt_tr(VT_I_LOADING));
        {int tw=ui_text_w(0.85f,play_display);if(tw>SCREEN_W-80)tw=SCREEN_W-80;ui_text_fit((SCREEN_W-tw)/2,SCREEN_H/2+42,tw,C_GREY,0.85f,play_display);}
    }
    frame_end();
}


/* ---------------------------------------------------------- account */

typedef struct {
    auth_device_t dev;
    auth_t a;
    char err[160];
} login_job_t;

static int login_start_run(void *arg, volatile int *abort_flag)
{
    login_job_t *j = arg;
    return auth_device_start(&j->dev, j->err, sizeof(j->err), abort_flag);
}

static int login_poll_run(void *arg, volatile int *abort_flag)
{
    login_job_t *j = arg;
    int r = auth_device_poll(&j->dev, &j->a, j->err, sizeof(j->err), abort_flag);
    if (r == AUTH_OK) {
        /* fill login / display name / avatar */
        if (auth_ensure_valid(&j->a, j->err, sizeof(j->err), abort_flag) != 0)
            return -1;
    }
    return r;
}

static void account_logged_in(void)
{
    char name[80];
    ui_clean_text(g_auth.display[0] ? g_auth.display : g_auth.login, name, sizeof(name));
    for (;;) {
        ui_input_poll(&g_in);
        if (g_in.pressed & g_btn_back)
            return;
        if (g_in.pressed & SCE_CTRL_TRIANGLE) {
            LOGI("sesion cerrada por el usuario");
            if (g_auth_checking) {
                task_cancel(&g_auth_task);
                task_wait(&g_auth_task);
                g_auth_checking = 0;
                memset(&g_auth_job, 0, sizeof(g_auth_job));
            }
            g_logged = 0;
            memset(&g_auth, 0, sizeof(g_auth));
            sceIoRemove(AUTH_FILE);
            g_tabs[TAB_FOLLOWED].count = 0;
            tab_load(TAB_FOLLOWED);
            toast(vt_msg("Has cerrado sesion"));
            return;
        }
        frame_begin();
        draw_home();
        int x, y;
        draw_modal_box(500, 220, &x, &y);
        draw_avatar(g_auth.avatar, name, x + 28, y + 34, 72);
        ui_text_fit(x + 120, y + 64, 350, C_WHITE, 1.3f, name);
        char at[48];
        snprintf(at, sizeof(at), "@%s", g_auth.login);
        ui_text(x + 120, y + 94, C_GREY, 0.9f, at);
        ui_text(x + 28, y + 150, C_GREY, 0.85f, vt_msg("Sesion iniciada: Siguiendo y anuncios segun tu cuenta."));
        ui_hint(x + 28, y + 196, "\xE2\x96\xB3", vt_msg("Cerrar sesion"));
        ui_hint(x + 260, y + 196, g_back_name, vt_msg("Volver"));
        frame_end();
    }
}

static void account_login(void)
{
    static login_job_t job;
    static task_t task;

    if (!net_connected()) {
        modal_message(vt_msg("Sin conexion"), vt_msg("Conecta la Vita a una red Wi-Fi para iniciar sesion."));
        return;
    }
    memset(&job, 0, sizeof(job));
    LOGI("login: pidiendo codigo de dispositivo");
    if (run_with_spinner(&task, login_start_run, &job, vt_msg("Pidiendo un codigo a Twitch...")) < 0)
        return;
    if (task.result >= 0)
        LOGI("login: codigo recibido, caduca en %d s, consulta cada %d s", job.dev.expires_in, job.dev.interval);
    if (task.result < 0) {
        modal_message(vt_msg("No se pudo iniciar sesion"), job.err);
        return;
    }

    unsigned long long now = sceKernelGetProcessTimeWide();
    unsigned long long expires = now + (unsigned long long)job.dev.expires_in * 1000000ULL;
    unsigned long long next_poll = now + (unsigned long long)job.dev.interval * 1000000ULL;
    int polling = 0;

    /* show the code with spaced letters: "ABCD EFGH" */
    char code[32];
    size_t cl = strlen(job.dev.user_code);
    snprintf(code, sizeof(code), "%.*s %s", (int)(cl / 2), job.dev.user_code, job.dev.user_code + cl / 2);

    for (;;) {
        ui_input_poll(&g_in);
        now = sceKernelGetProcessTimeWide();
        if (g_in.pressed & g_btn_back) {
            if (polling) {
                task_cancel(&task);
                task_wait(&task);
            }
            return;
        }
        if (polling && task_done(&task)) {
            polling = 0;
            int r = task.result;
            if (r == AUTH_SLOW_DOWN)
                LOGW("login: Twitch pide consultar mas despacio");
            if (r == AUTH_OK) {
                LOGI("login: completado como @%s", job.a.login);
                g_auth = job.a;
                g_logged = 1;
                ensure_data_dir();
                if(g_remember)auth_save(&g_auth, AUTH_FILE);
                char hi[96], name[80];
                ui_clean_text(g_auth.display, name, sizeof(name));
                snprintf(hi, sizeof(hi), vt_msg("Hola, %s"), name);
                toast(hi);
                g_tab = TAB_FOLLOWED;
                tab_load(TAB_FOLLOWED);
                return;
            }
            if (r < 0) {
                modal_message(vt_msg("No se pudo iniciar sesion"), job.err);
                return;
            }
            if (r == AUTH_SLOW_DOWN)
                job.dev.interval += 5;
            next_poll = now + (unsigned long long)job.dev.interval * 1000000ULL;
        }
        if (!polling && now >= next_poll) {
            if (now >= expires) {
                modal_message(vt_msg("Codigo caducado"), vt_msg("El codigo ha caducado. Pulsa SELECT para pedir otro."));
                return;
            }
            if (task_start(&task, login_poll_run, &job) == 0)
                polling = 1;
        }

        frame_begin();
        draw_home();
        int x, y, w = 640, h = 330;
        draw_modal_box(w, h, &x, &y);
        ui_text(x + 32, y + 50, C_WHITE, 1.3f, vt_msg("Iniciar sesion en Twitch"));
        ui_text(x + 32, y + 92, C_GREY, 0.95f, vt_msg("1. En tu movil o PC abre:"));
        ui_text(x + 60, y + 128, C_PURPLE, 1.4f, "twitch.tv/activate");
        ui_text(x + 32, y + 168, C_GREY, 0.95f, vt_msg("2. Inicia sesion e introduce este codigo:"));
        ui_round_rect(x + 60, y + 184, 300, 64, 10, C_PANEL2);
        ui_text_center(x + 210, y + 230, C_WHITE, 2.0f, code);
        ui_spinner(x + 400, y + 216, 10, C_PURPLE);
        ui_text(x + 422, y + 222, C_GREY, 0.85f, vt_msg("Esperando..."));
        unsigned long long left = expires > now ? (expires - now) / 1000000ULL : 0;
        char t[64];
        snprintf(t, sizeof(t), vt_msg("El codigo caduca en %llu:%02llu"), left / 60, left % 60);
        ui_text(x + 32, y + h - 50, C_DIM, 0.8f, t);
        ui_hint(x + w - 150, y + h - 22, g_back_name, vt_msg("Cancelar"));
        frame_end();
    }
}

static void account_screen(void)
{
    if (g_logged)
        account_logged_in();
    else
        account_login();
}


/* ----------------------------------------------------- SELECT extras menu */
static void language_screen(void)
{
    int sel=vt_lang_current(), first=sel>7?sel-7:0;
    const int x=198,y=52,w=564,h=440,row=43,visible=8;
    for(;;) {
        ui_input_poll(&g_in);
        if(g_in.pressed&(g_btn_back|SCE_CTRL_SELECT)) return;
        if(g_in.pressed&SCE_CTRL_UP) sel=(sel+VT_LANG_COUNT-1)%VT_LANG_COUNT;
        if(g_in.pressed&SCE_CTRL_DOWN) sel=(sel+1)%VT_LANG_COUNT;
        if(g_in.touch_dy && g_in.touch_dragged) {
            if(g_in.touch_dy>16 && first>0) {first--;sel=first;}
            else if(g_in.touch_dy< -16 && first<VT_LANG_COUNT-visible) {first++;sel=first;}
        }
        int choose=!!(g_in.pressed&g_btn_ok);
        if(g_in.touch_tap) {
            for(int k=0;k<visible;k++)
                if(first+k<VT_LANG_COUNT && ui_touch_hit(&g_in,x+12,y+60+k*row,w-24,row-3)) {
                    sel=first+k;choose=1;break;
                }
        }
        if(choose) {
            vt_lang_set(sel);g_game_filter[0]=0;
            if(g_region==0){g_tabs[TAB_EXPLORE].count=0;g_tabs[TAB_EXPLORE].state=LS_EMPTY;tab_load(TAB_EXPLORE);}
            settings_save();
            LOGI("ui language selected: %s",vt_lang_code(sel));
            return;
        }
        if(sel<first) first=sel;
        if(sel>=first+visible) first=sel-visible+1;
        frame_begin();
        draw_home();
        int bx,by; draw_modal_box(w,h,&bx,&by);
        ui_text(x+25,y+42,C_WHITE,1.25f,vt_tr(VT_I_LANGUAGE));
        for(int k=0;k<visible;k++){
            int i=first+k; if(i>=VT_LANG_COUNT)break;
            if(i==sel) ui_round_rect(x+12,y+60+k*row,w-24,row-3,6,C_PURPLE);
            ui_text_fit(x+30,y+90+k*row,w-60,C_WHITE,0.94f,vt_lang_name(i));
        }
        ui_text_fit(x+24,y+h-13,w-48,C_GREY,0.7f,vt_msg("Up/Down  |  X  |  Back"));
        frame_end();
    }
}
static void region_screen(void)
{
    int sel=g_region,top=sel>7?sel-7:0;
    const int x=210,y=34,w=540,h=476,row=44,vis=8;
    int dirty=1;
    ui_input_poll(&g_in);
    for(;;){
        ui_input_poll(&g_in);
        if(g_in.pressed&(g_btn_back|SCE_CTRL_START|SCE_CTRL_SELECT))return;
        int before=sel,choose=!!(g_in.pressed&g_btn_ok);
        if(g_in.pressed&SCE_CTRL_UP)sel=(sel+REGION_COUNT-1)%REGION_COUNT;
        if(g_in.pressed&SCE_CTRL_DOWN)sel=(sel+1)%REGION_COUNT;
        if(g_in.pressed&SCE_CTRL_LEFT)sel=sel>=vis?sel-vis:0;
        if(g_in.pressed&SCE_CTRL_RIGHT)sel=sel+vis<REGION_COUNT?sel+vis:REGION_COUNT-1;
        if(g_in.touch_tap){
            if(!ui_touch_hit(&g_in,x,y,w,h))return;
            for(int k=0;k<vis && top+k<REGION_COUNT;k++)
                if(ui_touch_hit(&g_in,x+16,y+62+k*row,w-32,row-4)){sel=top+k;choose=1;break;}
        }
        if(g_in.touch_dragged && g_in.touch_dy>18 && sel>0)sel--;
        if(g_in.touch_dragged && g_in.touch_dy<-18 && sel<REGION_COUNT-1)sel++;
        if(choose){
            g_region=sel;g_game_filter[0]=0;settings_save();
            tab_t *tb=&g_tabs[TAB_EXPLORE];
            tb->count=0;tb->sel=0;tb->scroll=0;tb->state=LS_EMPTY;
            tab_load(TAB_EXPLORE);
            LOGI("Explore language: %s",region_code());
            return;
        }
        if(sel<top)top=sel;
        if(sel>=top+vis)top=sel-vis+1;
        if(before!=sel)dirty=1;
        if(!dirty){sceKernelDelayThread(8000);continue;}
        vita2d_start_drawing();vita2d_clear_screen();
        vita2d_draw_rectangle(x,y,w,h,C_SURFACE);
        vita2d_draw_rectangle(x,y,w,3,C_PURPLE);
        ui_text_fit(x+24,y+40,w-48,C_WHITE,1.15f,region_title());
        for(int k=0;k<vis&&top+k<REGION_COUNT;k++){
            int i=top+k,ry=y+62+k*row;
            vita2d_draw_rectangle(x+16,ry,w-32,row-4,i==sel?C_PURPLE:C_PANEL);
            ui_text_fit(x+28,ry+28,w-140,C_WHITE,0.95f,region_label(i));
            if(i==g_region)ui_text_fit(x+w-105,ry+28,75,C_WHITE,0.8f,vt_tr(VT_I_ON));
        }
        ui_hint(x+24,y+h-22,g_ok_name,vt_tr(VT_I_PLAY));
        ui_hint(x+w/2,y+h-22,g_back_name,vt_tr(VT_I_BACK));
        vita2d_end_drawing();vita2d_swap_buffers();dirty=0;
        sceKernelDelayThread(8000);
    }
}
static void extras_menu(void)
{
    /* All entries share the same hit boxes for touch and controller use. */
    const int x=145,y=20,w=670,h=504,row=35,count=12;
    int sel=0;
    for(;;) {
        ui_input_poll(&g_in);
        if(g_in.pressed&(SCE_CTRL_SELECT|g_btn_back))return;
        if(g_in.pressed&SCE_CTRL_UP)sel=(sel+count-1)%count;
        if(g_in.pressed&SCE_CTRL_DOWN)sel=(sel+1)%count;
        int choose=!!(g_in.pressed&g_btn_ok);
        if(g_in.touch_tap){
            for(int i=0;i<count;i++)
                if(ui_touch_hit(&g_in,x+14,y+68+i*row,w-28,row-4)){sel=i;choose=1;break;}
            if(!choose && !ui_touch_hit(&g_in,x,y,w,h))return;
        }
        if(choose){
            switch(sel){
                case 0: account_screen();break;
                case 1: language_screen();break;
                case 2: ui_touch_enable(!ui_touch_enabled());settings_save();break;
                case 3: g_chat_on=!g_chat_on;settings_save();break;
                case 4: img_clear();toast(vt_msg("Thumbnail cache cleared"));break;
                case 5: modal_message("VitaTwitch", vt_msg("19 languages, touchscreen, Twitch chat, hardware video. Independent VitaTwitch fork."));break;
                case 6:g_profile=(g_profile+1)%VT_PROFILE_COUNT;settings_save();break;
                case 7:g_auto_quality=!g_auto_quality;settings_save();break;
                case 8:g_diag=!g_diag;settings_save();break;
                case 9:g_remember=!g_remember;if(g_remember&&g_logged)auth_save(&g_auth,AUTH_FILE);else sceIoRemove(AUTH_FILE);settings_save();break;
                case 10:region_screen();break;
                case 11:return;
            }
        }
        frame_begin();draw_home();
        int bx,by;draw_modal_box(w,h,&bx,&by);
        ui_text(x+26,y+41,C_WHITE,1.2f,vt_tr(VT_I_SETTINGS));
        for(int i=0;i<count;i++){
            int yy=y+68+i*row;
            if(sel==i)ui_round_rect(x+14,yy,w-28,row-4,7,C_PURPLE);
            const char *label=i==0?vt_tr(VT_I_ACCOUNT):i==1?vt_tr(VT_I_LANGUAGE):
                i==2?vt_tr(VT_I_TOUCH):i==3?vt_tr(VT_I_CHAT):
                i==4?vt_tr(VT_I_CLEAR_CACHE):i==5?vt_tr(VT_I_ABOUT):i==6?vt_opt(VT_O_PROFILE):i==7?vt_opt(VT_O_AUTO):i==8?vt_opt(VT_O_DIAG):i==9?vt_opt(VT_O_REMEMBER):i==10?region_title():vt_tr(VT_I_CLOSE);
            ui_text_fit(x+32,yy+31,w-220,C_WHITE,0.97f,label);
            if(i==1) ui_text_fit(x+w-182,yy+31,157,C_GREY,0.76f,vt_lang_code(vt_lang_current()));
            if(i==10)ui_text_fit(x+w-215,yy+27,190,C_WHITE,0.83f,region_label(g_region));
            if(i==6)ui_text_fit(x+w-215,yy+31,190,C_WHITE,0.83f,vt_opt((vt_option_id)(VT_O_ECONOMY+g_profile)));
            if(i==2||i==3||i==7||i==8||i==9)ui_text_fit(x+w-215,yy+31,190,C_WHITE,0.83f,
                (i==2?ui_touch_enabled():i==3?g_chat_on:i==7?g_auto_quality:i==8?g_diag:g_remember)?vt_tr(VT_I_ON):vt_tr(VT_I_OFF));
        }
        frame_end();
    }
}

/* ------------------------------------------------------------- init */

static void log_header(void)
{
    SceDateTime dt;
    memset(&dt, 0, sizeof(dt));
    sceRtcGetCurrentClockLocalTime(&dt);
    SceKernelFwInfo fw;
    memset(&fw, 0, sizeof(fw));
    fw.size = sizeof(fw);
    sceKernelGetSystemSwVersion(&fw);
    LOGI("VitaTwitch %s - %04d-%02d-%02d %02d:%02d:%02d - firmware %s", APP_VERSION, dt.year, dt.month, dt.day,
         dt.hour, dt.minute, dt.second, fw.versionString);
}

static void init_system(void)
{
    ensure_data_dir();
    log_open(LOG_FILE, LOG_PREV_FILE);
    log_header();

    scePowerSetArmClockFrequency(333);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    /* main thread (H.264 decode + drawing) gets core 0; downloads use cores 1-2 */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);

    /* (the H.264 decoder needs no sysmodule: SceVideodec is linked directly;
     *  loading SCE_SYSMODULE_AVCDEC only returned an error) */
    int r1 = sceSysmoduleLoadModule(SCE_SYSMODULE_NET), r2;
    if (r1 < 0)
        LOGE("init: carga del modulo NET 0x%08X", (unsigned)r1);

    static char net_mem[1 * 1024 * 1024];
    SceNetInitParam np;
    np.memory = net_mem;
    np.size = sizeof(net_mem);
    np.flags = 0;
    r1 = sceNetInit(&np);
    r2 = sceNetCtlInit();
    if (r1 < 0 || r2 < 0)
        LOGE("init: sceNetInit 0x%08X, sceNetCtlInit 0x%08X", (unsigned)r1, (unsigned)r2);

    SceAppUtilInitParam ai;
    SceAppUtilBootParam bi;
    memset(&ai, 0, sizeof(ai));
    memset(&bi, 0, sizeof(bi));
    sceAppUtilInit(&ai, &bi);

    SceCommonDialogConfigParam cfg;
    sceCommonDialogConfigParamInit(&cfg);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cfg.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cfg.enterButtonAssign);
    sceCommonDialogSetConfigParam(&cfg);
    vt_lang_auto((int)cfg.language); /* saved lang= still overrides this */
    if (cfg.enterButtonAssign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
        g_btn_ok = SCE_CTRL_CIRCLE;
        g_btn_back = SCE_CTRL_CROSS;
        g_ok_name = "O";
        g_back_name = "X";
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    if (sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,SCE_TOUCH_SAMPLING_STATE_START)<0)
        LOGW("touch: failed to enable front panel sampling");

    SceRtcTick tick;
    sceRtcGetCurrentTick(&tick);
    srand((unsigned)tick.tick);

    if (http_global_init() < 0)
        LOGE("init: curl_global_init fallo");

    vita2d_init();
    vita2d_set_clear_color(C_BG);
    ui_font = vita2d_load_default_pgf();
    if (!ui_font)
        LOGE("init: no se pudo cargar la fuente del sistema");
    img_init();

    int st = 0;
    sceNetCtlInetGetState(&st);
    LOGI("init: listo, boton aceptar %s, Wi-Fi %s", g_ok_name, st == SCE_NETCTL_STATE_CONNECTED ? "conectado" : "desconectado");
}

/* ------------------------------------------------------------- main */

static void open_selected(void)
{
    tab_t *tb = &g_tabs[g_tab];
    if (g_tab == TAB_SEARCH && tb->sel < 0) {
        char q[64];
        if (ime_input(vt_msg("Buscar canal"), g_search, q, sizeof(q), 40) && q[0]) {
            snprintf(g_search, sizeof(g_search), "%s", q);
            settings_save();
            tb->count = 0;
            tb->state = LS_LOADING;
            tab_load(TAB_SEARCH);
        }
        return;
    }
    if (tb->state != LS_READY || tb->sel < 0 || tb->sel >= tb->count || home_pos(tb)<0)
        return;
    twitch_stream_t s = tb->items[tb->sel]; /* copy: the list may refresh meanwhile */
    play_channel(s.login, s.display, s.title);
}

static void home_switch(int t)
{
    g_tab=t;g_side_focus=-1;g_touch_manual_scroll=0;
    if(t!=TAB_EXPLORE)g_game_filter[0]=0;
    tab_t *tb=&g_tabs[t];tb->scroll=0;tb->sel=t==TAB_SEARCH?-1:home_index(tb,0);
    if(tb->state==LS_EMPTY||tb->state==LS_ERROR)tab_load(t);
}

/* Browse games from the current Explore results, without adding API calls. */
static void games_screen(void)
{
    tab_t *tb=&g_tabs[TAB_EXPLORE];
    if(tb->state!=LS_READY){g_game_filter[0]=0;home_switch(TAB_EXPLORE);return;}
    int ids[MAX_ITEMS],n=0,sel=0;
    for(int i=0;i<tb->count;i++){
        if(!tb->items[i].game[0])continue;
        int seen=0;for(int j=0;j<n;j++)if(!strcmp(tb->items[ids[j]].game,tb->items[i].game)){seen=1;break;}
        if(!seen)ids[n++]=i;
    }
    if(!n){g_game_filter[0]=0;home_switch(TAB_EXPLORE);return;}
    const int x=180,y=52,w=600,h=440,row=42,vis=8;
    for(;;){
        ui_input_poll(&g_in);
        if(g_in.pressed&(g_btn_back|SCE_CTRL_SELECT))return;
        if(g_in.pressed&SCE_CTRL_UP)sel=(sel+n-1)%n;
        if(g_in.pressed&SCE_CTRL_DOWN)sel=(sel+1)%n;
        if(g_in.touch_dragged&&g_in.touch_dy>12&&sel>0)sel--;
        if(g_in.touch_dragged&&g_in.touch_dy< -12&&sel<n-1)sel++;
        if(g_in.touch_tap&&!ui_touch_hit(&g_in,x,y,w,h))return;
        int first=sel>=vis?sel-vis+1:0,choose=!!(g_in.pressed&g_btn_ok);
        for(int j=0;j<vis&&first+j<n;j++)if(ui_touch_hit(&g_in,x+18,y+55+j*row,w-36,row-3)){sel=first+j;choose=1;break;}
        if(choose){
            snprintf(g_game_filter,sizeof(g_game_filter),"%s",tb->items[ids[sel]].game);
            home_switch(TAB_EXPLORE);return;
        }
        frame_begin();draw_home();
        int bx,by;draw_modal_box(w,h,&bx,&by);
        ui_text(x+22,y+37,C_WHITE,1.15f,games_label());
        for(int j=0;j<vis&&first+j<n;j++){
            int k=first+j,yy=y+55+j*row;
            vita2d_draw_rectangle(x+18,yy,w-36,row-3,k==sel?CLASSIC_PURPLE:CLASSIC_TILE);
            ui_text_fit(x+30,yy+27,w-60,C_WHITE,0.95f,tb->items[ids[k]].game);
        }
        ui_hint(x+25,y+h-16,g_back_name,vt_tr(VT_I_BACK));
        frame_end();
    }
}
static void side_open(int i)
{
    switch(i){
    case 0:account_screen();break;
    case 1:home_switch(TAB_SEARCH);break;
    case 2:home_switch(TAB_FOLLOWED);break;
    case 3:games_screen();break;
    case 4:g_game_filter[0]=0;home_switch(TAB_EXPLORE);break;
    case 5:extras_menu();break;
    }
}

int main(void)
{
    init_system();
    settings_load();
    for (int t = 0; t < TAB_COUNT; t++)
        g_tabs[t].sel = t == TAB_SEARCH ? -1 : 0;

    if(!g_remember)sceIoRemove(AUTH_FILE);
    if (g_remember && auth_load(&g_auth, AUTH_FILE) == 0) {
        LOGI("sesion guardada de @%s, comprobando", g_auth.login);
        g_logged = 1;
        g_tab = TAB_FOLLOWED;
        g_auth_job.a = g_auth;
        if (net_connected() && task_start(&g_auth_task, auth_check_run, &g_auth_job) == 0)
            g_auth_checking = 1;
    }
    tab_load(g_tab);
    if (g_tab != TAB_EXPLORE)
        tab_load(TAB_EXPLORE);
    if (g_search[0])
        tab_load(TAB_SEARCH);

    unsigned long long last_tick = 0;
    for (;;) {
        ui_input_poll(&g_in);
        if(play_state!=PLAY_IDLE){
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            play_tick();
            continue;
        }
        auth_update();
        if(g_logged&&!g_auth_checking&&net_connected()&&sceKernelGetProcessTimeWide()-g_auth_last_check>3600000000ULL){
            g_auth_job.a=g_auth;
            if(task_start(&g_auth_task,auth_check_run,&g_auth_job)==0)g_auth_checking=1;
            g_auth_last_check=sceKernelGetProcessTimeWide();
        }
        tabs_update();
        tab_t *tb = &g_tabs[g_tab];
        unsigned p = g_in.pressed;

        if (p & SCE_CTRL_START)
            break;
        if (p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
            g_game_filter[0]=0;
            home_switch((g_tab+(p&SCE_CTRL_RTRIGGER?1:TAB_COUNT-1))%TAB_COUNT);
            continue;
        }
        if(p&SCE_CTRL_SELECT){extras_menu();continue;}
        if(g_in.touch_tap&&g_in.touch_x<GRID_X){
            for(int i=0;i<SIDE_COUNT;i++)if(ui_touch_hit(&g_in,SIDE_X,SIDE_Y+i*SIDE_STEP,SIDE_W,SIDE_H)){g_side_focus=i;side_open(i);break;}
            continue;
        }
        if(g_in.touch_down&&g_in.touch_dragged&&g_in.touch_start_x>=GRID_X&&g_in.touch_start_y>=grid_top(g_tab)&&g_in.touch_start_y<FOOTER_Y){
            g_side_focus=-1;g_touch_manual_scroll=1;tb->scroll-=g_in.touch_dy;
        }
        if(g_in.touch_tap&&g_in.touch_x>=GRID_X&&g_in.touch_y>=HEADER_H&&g_in.touch_y<FOOTER_Y){
            g_side_focus=-1;
            int top=grid_top(g_tab);
            if(g_tab==TAB_SEARCH&&g_in.touch_y<top){tb->sel=-1;g_touch_manual_scroll=0;open_selected();continue;}
            for(int i=0;i<home_count(tb);i++){
                home_rect_t r=home_rect(i);r.y+=top-(int)tb->scroll;
                if(g_in.touch_y>=top&&ui_touch_hit(&g_in,r.x,r.y,r.w,r.h)){
                    tb->sel=home_index(tb,i);g_touch_manual_scroll=0;open_selected();break;
                }
            }
            continue;
        }
        if (p & SCE_CTRL_TRIANGLE) {
            img_clear(); /* thumbnails are refreshed by Twitch every few minutes */
            tab_load(g_tab);
        }
        if (g_tab == TAB_SEARCH && (p & SCE_CTRL_SQUARE) && g_search[0]) {
            char login[32];
            if (twitch_sanitize_login(g_search, login, sizeof(login)) >= 3)
                play_channel(login, login, NULL);
            else
                toast(vt_msg("Ese nombre no es un canal valido"));
            continue;
        }

        /* Sidebar and mosaic share the same geometry as touch. */
        if(p&(SCE_CTRL_UP|SCE_CTRL_DOWN|SCE_CTRL_LEFT|SCE_CTRL_RIGHT))g_touch_manual_scroll=0;
        int n=home_count(tb),pos=home_pos(tb);
        if(g_side_focus>=0){
            if(p&SCE_CTRL_UP)g_side_focus=(g_side_focus+SIDE_COUNT-1)%SIDE_COUNT;
            else if(p&SCE_CTRL_DOWN)g_side_focus=(g_side_focus+1)%SIDE_COUNT;
            else if(p&SCE_CTRL_RIGHT){g_side_focus=-1;if(pos<0&&g_tab!=TAB_SEARCH)tb->sel=home_index(tb,0);}
            else if(p&g_btn_ok){side_open(g_side_focus);continue;}
        }else{
            int next=-1;
            if(p&SCE_CTRL_LEFT){
                if(pos>=0)next=home_move(pos,n,-1,0);
                if(next<0)g_side_focus=side_active();
            }else if(p&SCE_CTRL_RIGHT){if(pos>=0)next=home_move(pos,n,1,0);else if(n)next=0;}
            else if(p&SCE_CTRL_DOWN){if(pos<0&&n)next=0;else if(pos>=0)next=home_move(pos,n,0,1);}
            else if(p&SCE_CTRL_UP){if(pos>=0)next=home_move(pos,n,0,-1);if(next<0&&g_tab==TAB_SEARCH)tb->sel=-1;}
            if(next>=0)tb->sel=home_index(tb,next);
            if(p&g_btn_back)g_side_focus=side_active();
            if(g_tab!=TAB_SEARCH&&home_pos(tb)<0&&n)tb->sel=home_index(tb,0);
            if(g_side_focus<0&&(p&g_btn_ok)){
                if(tb->need_login)account_screen();
                else open_selected();
            }
        }

        unsigned long long now = sceKernelGetProcessTimeWide();
        if (now - last_tick > 1000000ULL) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            last_tick = now;
        }

        frame_begin();
        draw_home();
        frame_end();
    }

    if(play_state==PLAY_FETCH||play_state==PLAY_CANCEL){task_cancel(&play_task);task_wait(&play_task);}
    for (int t = 0; t < TAB_COUNT; t++)
        if (g_tabs[t].busy) {
            task_cancel(&g_tabs[t].task);
            task_wait(&g_tabs[t].task);
        }
    if (g_auth_checking) {
        task_cancel(&g_auth_task);
        task_wait(&g_auth_task);
    }
    img_shutdown();
    vita2d_fini();
    vita2d_free_pgf(ui_font);
    http_global_term();
    sceNetCtlTerm();
    sceNetTerm();
    LOGI("salida normal");
    log_close();
    sceKernelExitProcess(0);
    return 0;
}

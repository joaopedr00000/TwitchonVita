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
#define APP_VERSION   "1.34-media-playback-test29"

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
#define SIDE_COUNT 5
#define PROFILE_FOCUS 5
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
static int g_tv_layout=0,g_follow_offline=0;
static int g_tv_home=1,g_tv_nav=0,g_tv_nav_focus=0;

static ui_input_t g_in;
static char g_toast[160];
static unsigned long long g_toast_until;

/* ------------------------------------------------------------- tabs */

enum { TAB_FOLLOWED, TAB_EXPLORE, TAB_SEARCH, TAB_COUNT };
static const char *tab_name(int t) {return vt_tr((vt_text_id)(VT_I_FOLLOWING+t));}

typedef enum { LS_EMPTY, LS_LOADING, LS_READY, LS_ERROR } list_state_t;

typedef struct {
    twitch_list_t kind;
    char query[256];
    char oauth[64];
    twitch_stream_t out[MAX_ITEMS];
    int n;
    twitch_stream_t local[MAX_ITEMS];
    int local_n;
    char err[160];
} list_job_t;

typedef struct {
    list_state_t state;
    twitch_stream_t items[MAX_ITEMS*2];
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
static char g_game_filter[96],g_game_id[32],g_game_slug[128];
static void explore_query(char *out,size_t cap){
    if(g_game_id[0])snprintf(out,cap,"%s|%s|%s",g_game_id,region_code(),g_game_slug);
    else snprintf(out,cap,"%s",region_code());
}

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

/* Device-only favorites, never presented as a Twitch account follow. */
#define SAVED_FILE DATA_DIR "/saved-channels.txt"
static twitch_stream_t saved_channels[MAX_ITEMS],saved_work[MAX_ITEMS];
static int saved_n;
static twitch_stream_t account_channels[MAX_ITEMS], local_live[MAX_ITEMS];
static int account_n, local_live_n;
static char account_oauth[sizeof(g_auth.access)];
static int following_dirty;
static void following_rebuild(void);
static const char *saved_label(int saved){
    int l=vt_lang_current();return saved?(l==2?"Remover do Vita":l==1?"Quitar de Vita":"Remove from Vita"):(l==2?"Salvar no Vita":l==1?"Guardar en Vita":"Save on Vita");
}
static const char *saved_title(void){return vt_lang_current()==2?"Salvos no Vita":vt_lang_current()==1?"Guardados en Vita":"Saved on Vita";}
static int saved_has(const char *login){for(int i=0;login&&*login&&i<saved_n;i++)if(!strcmp(saved_channels[i].login,login))return 1;return 0;}
static void saved_field(char *out,size_t cap,const char *in){
    size_t n=0;if(in)while(*in&&n+1<cap){char c=*in++;out[n++]=(c=='\t'||c=='\r'||c=='\n')?' ':c;}out[n]=0;
}
static void saved_load(void){
    saved_n=0;FILE *fp=fopen(SAVED_FILE,"r");if(!fp)fp=fopen(SAVED_FILE ".bak","r");if(!fp)return;
    char line[512];while(saved_n<MAX_ITEMS&&fgets(line,sizeof(line),fp)){
        char *a=strchr(line,'\t');if(!a)continue;*a++=0;char *b=strchr(a,'\t');if(!b)continue;*b++=0;b[strcspn(b,"\r\n")]=0;
        char url[96];if(twitch_channel_url(line,url,sizeof(url))<0||saved_has(line))continue;
        twitch_stream_t *s=&saved_channels[saved_n++];memset(s,0,sizeof(*s));
        saved_field(s->login,sizeof(s->login),line);saved_field(s->display,sizeof(s->display),a);saved_field(s->avatar,sizeof(s->avatar),b);s->viewers=-1;
    }fclose(fp);
}
static int saved_toggle(const char *login,const char *display,const char *avatar){
    char url[96];if(twitch_channel_url(login,url,sizeof(url))<0)return -1;
    int n=saved_n,at=-1;memcpy(saved_work,saved_channels,(size_t)n*sizeof(saved_work[0]));
    for(int i=0;i<n;i++)if(!strcmp(saved_work[i].login,login))at=i;
    if(at>=0){memmove(saved_work+at,saved_work+at+1,(size_t)(n-at-1)*sizeof(saved_work[0]));n--;}
    else{if(n>=MAX_ITEMS)return -1;twitch_stream_t *s=&saved_work[n++];memset(s,0,sizeof(*s));saved_field(s->login,sizeof(s->login),login);saved_field(s->display,sizeof(s->display),display&&*display?display:login);saved_field(s->avatar,sizeof(s->avatar),avatar);s->viewers=-1;}
    ensure_data_dir();FILE *fp=fopen(SAVED_FILE ".tmp","w");if(!fp)return -1;
    int rc=0;for(int i=0;i<n;i++)if(fprintf(fp,"%s\t%s\t%s\n",saved_work[i].login,saved_work[i].display,saved_work[i].avatar)<0)rc=-1;
    if(fclose(fp)!=0)rc=-1;
    if(rc<0){sceIoRemove(SAVED_FILE ".tmp");return -1;}
    /* Preserve the previous file even when the filesystem cannot replace it. */
    sceIoRemove(SAVED_FILE ".bak");int backed=sceIoRename(SAVED_FILE,SAVED_FILE ".bak")>=0;
    if(sceIoRename(SAVED_FILE ".tmp",SAVED_FILE)<0){if(backed)sceIoRename(SAVED_FILE ".bak",SAVED_FILE);sceIoRemove(SAVED_FILE ".tmp");return -1;}
    sceIoRemove(SAVED_FILE ".bak");memcpy(saved_channels,saved_work,(size_t)n*sizeof(saved_work[0]));saved_n=n;following_dirty=1;following_rebuild();
    return at<0?1:0;
}
static int saved_ensure(const char *login,const char *display,const char *avatar){
    if(saved_has(login))return 1; /* idempotent; never toggle a saved channel off */
    return saved_toggle(login,display,avatar);
}
static int stream_find(const twitch_stream_t *items,int n,const char *login){
    for(int i=0;i<n;i++)if(!strcmp(items[i].login,login))return i;
    return -1;
}
static int account_valid(void){return g_logged&&account_oauth[0]&&!strcmp(account_oauth,g_auth.access);}
static int account_follow_known(const char *login){
    return login&&*login&&account_valid()&&stream_find(account_channels,account_n,login)>=0;
}
static const char *following_mode(void){
    int l=vt_lang_current();
    return g_follow_offline==2?saved_title():g_follow_offline==1?vt_tr(VT_I_LIVE):(l==2?"Todos os seguidos":l==1?"Todos los seguidos":"All follows");
}
/* The account cache is never populated from local bookmarks. Rebuild the visible
 * union from the current saved file and authenticated account data. */
static void following_rebuild(void){
    tab_t *tb=&g_tabs[TAB_FOLLOWED];char cur[32]="";
    if(tb->sel>=0&&tb->sel<tb->count)snprintf(cur,sizeof(cur),"%s",tb->items[tb->sel].login);
    int n=0,remote=account_valid()?account_n:0;
    if(g_follow_offline!=2)for(int i=0;i<remote;i++){
        const twitch_stream_t *s=&account_channels[i];
        int k=stream_find(local_live,local_live_n,s->login);
        if(k>=0&&local_live[k].live&&!s->live)s=&local_live[k];
        if(g_follow_offline==1&&!s->live)continue;
        if(stream_find(tb->items,n,s->login)<0)tb->items[n++]=*s;
    }
    for(int i=0;i<saved_n;i++){
        const twitch_stream_t *s=&saved_channels[i];
        int k=stream_find(local_live,local_live_n,s->login);
        int a=stream_find(account_channels,remote,s->login);
        if(k>=0)s=&local_live[k];
        if(a>=0&&(account_channels[a].live||k<0))s=&account_channels[a];
        if(g_follow_offline==1&&!s->live)continue;
        if(stream_find(tb->items,n,s->login)<0)tb->items[n++]=*s;
    }
    twitch_sort_by_viewers(tb->items,n);
    tb->count=n;tb->sel=cur[0]?stream_find(tb->items,n,cur):-1;
    if(tb->sel<0)tb->sel=0;
    tb->need_login=!g_logged&&!saved_n;
    tb->state=LS_READY;
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
            g_search[0]=0;
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
        else if (!strncmp(line,"tv_layout=",10))g_tv_layout=atoi(line+10)!=0;
        else if (!strncmp(line,"following_filter=",17)){int mode=atoi(line+17);g_follow_offline=mode>=0&&mode<=2?mode:0;}
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
            g_pref_quality, "", g_chat_on,
            vt_lang_code(vt_lang_current()), ui_touch_enabled());
    fprintf(f,"profile=%d\nauto_quality=%d\ndiag=%d\nvolume=%d\nremember=%d\n",g_profile,g_auto_quality,g_diag,g_volume,g_remember);
    fprintf(f,"region=%s\nquality_flow=3\ntv_layout=%d\nfollowing_filter=%d\n",region_codes[g_region],g_tv_layout,g_follow_offline);
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
    if(j->kind==TW_LIST_SAVED){twitch_refresh_saved(j->local,j->local_n,abort_flag);j->n=0;return 0;}
    if(j->kind==TW_LIST_ALL_FOLLOWS)twitch_refresh_saved(j->local,j->local_n,abort_flag);
    j->n = twitch_fetch_list(j->kind, j->query, j->oauth, j->out, MAX_ITEMS,
                             j->err, sizeof(j->err), abort_flag);
    return j->n;
}

static void tab_load(int t)
{
    tab_t *tb = &g_tabs[t];
    if(t==TAB_FOLLOWED){
        following_rebuild();
        if(tb->busy||g_auth_checking||!net_connected())return;
        if(!g_logged&&!saved_n)return;
        list_job_t *j=&tb->job;
        j->kind=g_logged?TW_LIST_ALL_FOLLOWS:TW_LIST_SAVED;
        j->local_n=saved_n;memcpy(j->local,saved_channels,(size_t)saved_n*sizeof(j->local[0]));
        snprintf(j->oauth,sizeof(j->oauth),"%s",g_logged?g_auth.access:"");
        if(task_start(&tb->task,list_job_run,j)==0){tb->busy=1;following_dirty=0;}
        return;
    }
    if(tb->busy)return;
    tb->need_login=0;
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
    j->kind = t == TAB_EXPLORE ? (g_game_id[0]?TW_LIST_CATEGORY:TW_LIST_TOP) : TW_LIST_SEARCH;
    if(t==TAB_EXPLORE)explore_query(j->query,sizeof(j->query));else snprintf(j->query,sizeof(j->query),"%s",g_search);
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
        if(t==TAB_FOLLOWED){
            list_job_t *j=&tb->job;
            if((j->kind==TW_LIST_ALL_FOLLOWS&&(!g_logged||strcmp(j->oauth,g_auth.access)))||
               (j->kind==TW_LIST_SAVED&&g_logged)){tab_load(t);continue;}
            if(j->kind==TW_LIST_ALL_FOLLOWS&&j->n>=0){
                account_n=j->n;memcpy(account_channels,j->out,(size_t)account_n*sizeof(account_channels[0]));
                snprintf(account_oauth,sizeof(account_oauth),"%s",j->oauth);
            }else if(j->kind==TW_LIST_ALL_FOLLOWS&&j->n<0&&!tb->task.abort_flag){
                LOGW("Following account refresh failed: %s",j->err);
                if(!account_valid()&&!saved_n){tb->count=0;tb->state=LS_ERROR;snprintf(tb->err,sizeof(tb->err),"%s",j->err);continue;}
                toast(j->err);
            }
            local_live_n=0;
            for(int i=0;i<j->local_n;i++)if(saved_has(j->local[i].login))local_live[local_live_n++]=j->local[i];
            following_rebuild();
            if(following_dirty)tab_load(t);
            continue;
        }
        if(t==TAB_SEARCH&&strcmp(tb->job.query,g_search)){tb->state=LS_EMPTY;tb->count=0;if(g_search[0])tab_load(t);continue;}
        if(t==TAB_EXPLORE){char expected[256];explore_query(expected,sizeof(expected));if(strcmp(tb->job.query,expected)){tb->state=LS_EMPTY;tb->count=0;tab_load(t);continue;}}
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
    if(tb!=&g_tabs[TAB_EXPLORE]||!g_game_filter[0]||g_game_id[0])return tb->count;
    int n=0;for(int i=0;i<tb->count;i++)if(!strcmp(tb->items[i].game,g_game_filter))n++;
    return n;
}
static int home_index(const tab_t *tb,int pos)
{
    if(pos<0)return -1;
    if(tb!=&g_tabs[TAB_EXPLORE]||!g_game_filter[0]||g_game_id[0])return pos<tb->count?pos:-1;
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
    static const char *const names[]={"Categories","Categorías","Categorias","Catégories","Categorie","Kategorien","Категории","Kategori","Mga kategorya","Mga kategorya","Categorieën","Kategorie","Kategoriler","Категорії","Kategorier","カテゴリー","카테고리","分类","分類"};
    int l=vt_lang_current();return names[l>=0&&l<VT_LANG_COUNT?l:0];
}
static const char *channels_label(void)
{
    static const char *const names[]={"Explore","Explorar","Explorar","Explorer","Esplora","Entdecken","Обзор","Jelajahi","Tuklasin","Susihon","Ontdekken","Odkrywaj","Keşfet","Огляд","Utforska","探索","탐색","探索","探索"};
    int l=vt_lang_current();return names[l>=0&&l<VT_LANG_COUNT?l:0];
}
static int side_active(void)
{
    return g_tab==TAB_SEARCH?0:g_tab==TAB_FOLLOWED?1:g_game_filter[0]?3:2;
}
static const char *side_label(int i)
{
    switch(i){
    case 0:return vt_tr(VT_I_SEARCH);
    case 1:return vt_tr(VT_I_FOLLOWING);
    case 2:return channels_label();
    case 3:return games_label();
    case PROFILE_FOCUS:return vt_lang_current()==2?"Meu perfil":vt_lang_current()==1?"Mi perfil":"My profile";
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
        draw_avatar(s->avatar,s->display,x+w/2-27,y+(h-(w>300?68:54))/2-27,54);
    }
    int strip=w>300?68:54;
    vita2d_draw_rectangle(x,y+h-strip,w,strip,RGBA8(15,9,24,220));
    ui_clean_text(s->display,line,sizeof(line));
    ui_text_fit(x+10,y+h-strip+(w>300?28:23),w-20,C_WHITE,w>300?1.16f:0.98f,line);
    if(s->live){
        ui_fmt_viewers(s->viewers,v,sizeof(v));
        snprintf(second,sizeof(second),"%s%s%s",v,s->game[0]?"  /  ":"",s->game);
    }else snprintf(second,sizeof(second),"%s",vt_tr(VT_I_OFFLINE));
    if(g_tab==TAB_FOLLOWED&&saved_has(s->login)&&!account_follow_known(s->login)){
        const char *tag=twitch_follow_label(0,1);size_t used=strlen(second);snprintf(second+used,sizeof(second)-used," | %s",tag);
    }
    ui_clean_text(second,line,sizeof(line));
    ui_text_fit(x+10,y+h-9,w-20,RGBA8(203,184,235,255),w>300?0.96f:0.86f,line);
    if(s->live){
        int bw=ui_text_w(0.83f,vt_tr(VT_I_LIVE))+28;if(bw>w-16)bw=w-16;
        vita2d_draw_rectangle(x+8,y+5,bw,22,C_SHADE);
        vita2d_draw_rectangle(x+13,y+12,8,8,C_RED);
        ui_text_fit(x+26,y+21,bw-21,C_WHITE,0.83f,vt_tr(VT_I_LIVE));
    }
    if(selected)ui_frame(x,y,w,h,2,C_WHITE);
}

static void draw_glitch(float x,float y,float scale,unsigned int color)
{
    /* Existing native Twitch Glitch silhouette, no extra GPU texture. */
    static const unsigned int glitch[24]={
        0x3fffe0u,0x3ffff0u,0x200078u,0x20007cu,0x20007cu,0x23107cu,0x23107cu,0x23107cu,0x23107cu,0x23107cu,0x20007cu,0x30007cu,0x38007cu,0x1c007cu,0xe007cu,0x7e7fcu,0x3f7fcu,0x1fffcu,0xfffcu,0x7ffcu,0x780u,0x380u,0x180u,0x80u
    };
    for(int gy=0;gy<24;gy++)for(int gx=0;gx<24;){
        unsigned mask=glitch[gy];
        if(!(mask&(1u<<gx))){gx++;continue;}
        int end=gx+1;while(end<24&&(mask&(1u<<end)))end++;
        vita2d_draw_rectangle(x+gx*scale,y+gy*scale,(end-gx)*scale,scale,color);gx=end;
    }
}
/* Outline silhouettes sampled from the supplied navigation reference.
 * Tiny 24x120 RGBA atlas: no network, font dependency or per-frame allocation. */
static vita2d_texture *side_icon_atlas;
static void side_icons_init(void)
{
    static const char masks[5][24*24+1]={
        "000000000000000000000000"
        "000000000000000000000000"
        "0000000057AA862000000000"
        "0000004DFFFFFFE800000000"
        "000005FFD7445BFFA0000000"
        "00003EF80000004EF7000000"
        "0000CF9000000004FF200000"
        "0004FE00000000009F800000"
        "0008F700000000002FC00000"
        "0009F400000000000EE00000"
        "0009F300000000000DF00000"
        "0008F500000000000ED00000"
        "0004FA00000000005F900000"
        "0000DF3000000000DF400000"
        "00006FD10000000AFA000000"
        "000009FD610004CFFC100000"
        "0000007FFFDCFFF9AFC10000"
        "000000029EFEB8300BFC1000"
        "000000000000000001CFC000"
        "0000000000000000001DF100"
        "000000000000000000013000"
        "000000000000000000000000"
        "000000000000000000000000"
        "000000000000000000000000",
        "000000000000000000000000"
        "000000000000000000000000"
        "000000000000000000000000"
        "000000014200001330000000"
        "0000019EFFE62AFFFC600000"
        "00004EFCBCFFFFEBBEFA0000"
        "0002EF50004DF91001BF8000"
        "000AF60000013000001DF200"
        "000EE000000000000007F600"
        "001FC000000000000005F800"
        "000FD000000000000006F700"
        "000CF100000000000009F400"
        "0007F80000000000001EE000"
        "0001EE1000000000009F8000"
        "00006FC10000000006FC0000"
        "00000AFC100000006FE20000"
        "000000CFC2000008FE400000"
        "0000001BFE5001BFE4000000"
        "000000008FFB6EFC20000000"
        "0000000003BFFE6000000000"
        "000000000005810000000000"
        "000000000000000000000000"
        "000000000000000000000000"
        "000000000000000000000000",
        "000000000000000000000000"
        "000000000000000000000000"
        "0000000038BDECA500000000"
        "0000002BFFFDCEFFE6000000"
        "000003EFB4000027EF800000"
        "00001DF7000000002DF60000"
        "0000AF600000000002DE2000"
        "0004FA00000037BF404FA000"
        "000AF300019DFFFF500CF100"
        "000EE00009FFC7EF1008F500"
        "001FC0000DF202FC0005F800"
        "001FC0002FC006F80004F900"
        "000FD0007F727DF30006F700"
        "000DF100BFDFFFD00009F500"
        "0008F700EFFC8400001EF100"
        "0002FE1056200000009F9000"
        "00008FC10000000008FD1000"
        "00000CFD300000019FF30000"
        "000001AFFB85479EFD400000"
        "00000005DFFFFFFF91000000"
        "00000000048AB96100000000"
        "000000000000000000000000"
        "000000000000000000000000"
        "000000000000000000000000",
        "000000000000000000000000"
        "000000000000000000000000"
        "000000000000000000000000"
        "0000049B81000006AA810000"
        "00006FFFFE20009FFFFC1000"
        "0003FE85AFC005FD75BF9000"
        "0008F6000CF20AF4000EE000"
        "0009F40009F40BF2000CF100"
        "0006FA002EE108F8003FD000"
        "0000CFC8EF7001EFB9EF5000"
        "00001CFFF700002DFFE50000"
        "000000231000000023000000"
        "0000049B81000006AA810000"
        "00006FFFFE20009FFFFC1000"
        "0003FC416FC005FB318F9000"
        "0008F5000BF20AF3000DE000"
        "0009F40009F40BF2000CF100"
        "0006FA002EE108F8003FD000"
        "0000CFCBEF7001EFCBEF5000"
        "00001CFFF700002DFFE50000"
        "000000231000000023000000"
        "000000000000000000000000"
        "000000000000000000000000"
        "000000000000000000000000",
        "000000000000000000000000"
        "000000000000000000000000"
        "000000000037761000000000"
        "0000000005FFFFC000000000"
        "000000001EF54BF800000000"
        "00000577DF8002EF97710000"
        "0000BFFFFB00005FFFFE3000"
        "0005FE874000000177BFC000"
        "000BF30000000000000BF300"
        "000CF300006EFB20000AF400"
        "0007FB0006FDBFD0003FE000"
        "0000DF200DE209F5009F6000"
        "0000AF300EE006F700BF3000"
        "0003FE000BFA6EF3007FA000"
        "000AF70002DFFF70001DF200"
        "000CF100001573000009F400"
        "0008F90000000000003EE100"
        "0001EFFFF700002AFFFF7000"
        "00002AFFFF4000CFFFD70000"
        "000000007FD106FC10000000"
        "000000000BFDCFF300000000"
        "0000000001BFFE5000000000"
        "000000000000000000000000"
        "000000000000000000000000"
    };
    side_icon_atlas=vita2d_create_empty_texture_format(24,120,SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if(!side_icon_atlas)return;
    unsigned char *pixels=vita2d_texture_get_datap(side_icon_atlas);
    unsigned stride=vita2d_texture_get_stride(side_icon_atlas);
    if(!pixels||stride<24*4){vita2d_free_texture(side_icon_atlas);side_icon_atlas=NULL;return;}
    memset(pixels,0,(size_t)stride*120);
    for(int i=0;i<5;i++)for(int y=0;y<24;y++)for(int x=0;x<24;x++){
        char c=masks[i][y*24+x];
        unsigned a=(unsigned)(c<='9'?c-'0':c-'A'+10)*17;
        unsigned char *p=pixels+(size_t)(i*24+y)*stride+x*4;
        p[0]=239;p[1]=239;p[2]=241;p[3]=(unsigned char)a;
    }
    vita2d_texture_set_filters(side_icon_atlas,SCE_GXM_TEXTURE_FILTER_LINEAR,SCE_GXM_TEXTURE_FILTER_LINEAR);
}
static void side_icon_draw(int i,int x,int y)
{
    if(side_icon_atlas&&i>=0&&i<5)
        vita2d_draw_texture_part_scale(side_icon_atlas,x,y,0,i*24,24,24,1.0f,1.0f);
}

static void draw_header(void)
{
    /* Draw sidebar after cards so offscreen tiles cannot overlap it. */
    vita2d_draw_rectangle(0,0,GRID_X-5,SCREEN_H,CLASSIC_BG);
    vita2d_draw_rectangle(GRID_X-5,0,SCREEN_W-GRID_X+5,HEADER_H,CLASSIC_BG);
    draw_glitch(SIDE_X+(SIDE_W-60)/2,21,2.5f,C_WHITE);
    int active=side_active();
    for(int i=0;i<SIDE_COUNT;i++){
        int y=SIDE_Y+i*SIDE_STEP;
        vita2d_draw_rectangle(SIDE_X,y,SIDE_W,SIDE_H,i==active?RGBA8(120,80,189,255):CLASSIC_PURPLE);
        if(g_side_focus==i)ui_frame(SIDE_X,y,SIDE_W,SIDE_H,2,C_WHITE);
        side_icon_draw(i,SIDE_X+9,y+(SIDE_H-24)/2);
        int label_w=SIDE_W-45;
        if(i==1&&g_tabs[TAB_FOLLOWED].state==LS_READY){
            char n[16];snprintf(n,sizeof(n),"%d",g_tabs[TAB_FOLLOWED].count);
            int nw=ui_text_w(0.8f,n);
            ui_text(SIDE_X+SIDE_W-nw-10,y+33,C_WHITE,0.8f,n);label_w-=nw+12;
        }
        ui_text_fit(SIDE_X+35,y+33,label_w,C_WHITE,1.0f,side_label(i));
    }
    const char *heading=g_tab==TAB_FOLLOWED&&g_follow_offline==2?saved_title():g_tab==TAB_EXPLORE&&g_game_filter[0]?g_game_filter:side_label(active);
    ui_text_fit(GRID_X,40,770-GRID_X,C_WHITE,1.32f,heading);
    if(g_side_focus==PROFILE_FOCUS)ui_round_rect(778,7,170,44,6,CLASSIC_PURPLE);
    char name[80];ui_clean_text(g_logged?(g_auth.display[0]?g_auth.display:g_auth.login):(vt_lang_current()==2?"Entrar":vt_lang_current()==1?"Iniciar sesión":"Sign in"),name,sizeof(name));
    ui_text_fit(788,39,112,C_GREY,.86f,name);
    if(g_logged)draw_avatar(g_auth.avatar,name,906,14,32);
    else {ui_round_rect(906,14,32,32,16,C_PANEL);ui_text_center(922,37,C_WHITE,1.0f,"+");}
}

/* Fixed footer cells keep long localized labels inside the 960px display. */
static const char *profile_label(int action)
{
    static const char *const labels[19][2]={
        {"Profile","Follow / unfollow"},{"Perfil","Seguir / dejar de seguir"},{"Perfil","Seguir / deixar de seguir"},
        {"Profil","Suivre / ne plus suivre"},{"Profilo","Segui / non seguire"},{"Profil","Folgen / entfolgen"},
        {"Профиль","Подписаться / отписаться"},{"Profil","Ikuti / berhenti mengikuti"},{"Profile","Sundan / i-unfollow"},
        {"Profile","Sunda / undang sa pagsunod"},{"Profiel","Volgen / ontvolgen"},{"Profil","Obserwuj / przestań"},
        {"Profil","Takip et / bırak"},{"Профіль","Стежити / не стежити"},{"Profil","Följ / avfölj"},
        {"プロフィール","フォロー / 解除"},{"프로필","팔로우 / 취소"},{"个人资料","关注 / 取消关注"},{"個人資料","追蹤 / 取消追蹤"}
    };
    int lang=vt_lang_current();return labels[lang>=0&&lang<19?lang:0][action];
}
static void footer_hint(int x,int width,int y,const char *btn,const char *label)
{
    ui_hint_bounded(x,y,width,btn,label);
}
static void draw_footer(void)
{
    tab_t *tb=&g_tabs[g_tab];
    vita2d_draw_rectangle(0,FOOTER_Y,SCREEN_W,SCREEN_H-FOOTER_Y,C_SURFACE);
    vita2d_draw_rectangle(0,FOOTER_Y,SCREEN_W,1,C_PANEL2);
    const char *ok=g_side_focus>=0?side_label(g_side_focus):
        g_tab==TAB_SEARCH&&tb->sel<0?vt_tr(VT_I_SEARCH):vt_tr(VT_I_PLAY);
    const char *btns[]={g_ok_name,"△","□","L/R","SELECT","START"};
    const char *labels[]={ok,vt_tr(VT_I_REFRESH),games_label(),vt_tr(VT_I_TAB),vt_tr(VT_I_MENU),profile_label(0)};
    int count=6,cell=(SCREEN_W-24)/count,pos=0;
    for(int i=0;i<6;i++){footer_hint(12+pos++*cell,cell,FOOTER_Y+25,btns[i],labels[i]);}
}

static void draw_center_msg(const char *title, const char *msg, int top)
{
    int cx=(GRID_X+SCREEN_W)/2,cy=top+(FOOTER_Y-top)/2;
    int w=SCREEN_W-GRID_X-36;
    int tw=ui_text_w(1.02f,title);if(tw>w)tw=w;
    ui_text_fit(cx-tw/2,cy-10,tw,C_WHITE,1.02f,title);
    if(msg){int mw=ui_text_w(0.82f,msg);if(mw>w)mw=w;ui_text_fit(cx-mw/2,cy+22,mw,C_GREY,0.82f,msg);}
}

 #define TV_NAV_COUNT 6
#define TV_NAV_X 88
#define TV_NAV_STEP 144
/* Shared drawing/touch geometry: leave a full text line between sections. */
#define TV_ROW_TOP 264
#define TV_SEARCH_ROW_TOP 289
#define TV_ROW_STEP 132
#define TV_TILE_W 222
#define TV_TILE_H 78
#define TV_NAME_BASELINE 98
#define TV_TILE_HIT_H 106
#define TV_SECTION_BASELINE 12
_Static_assert(TV_ROW_STEP-TV_SECTION_BASELINE-TV_NAME_BASELINE>=22, "TV section title clearance");
_Static_assert(TV_ROW_TOP+TV_ROW_STEP+TV_TILE_HIT_H<=FOOTER_Y, "TV rows fit above footer");
static int tv_row_top(void){return !g_tv_home&&g_tab==TAB_SEARCH?TV_SEARCH_ROW_TOP:TV_ROW_TOP;}
static const char *tv_nav_label(int i){
    switch(i){case 0:return vt_lang_current()==2?"Início":vt_lang_current()==1?"Inicio":"Home";case 1:return vt_tr(VT_I_FOLLOWING);case 2:return channels_label();case 3:return vt_tr(VT_I_SEARCH);case 4:return profile_label(0);default:return vt_tr(VT_I_SETTINGS);}
}
static void tv_tile(const twitch_stream_t *s,int x,int y,int active){
    const int w=TV_TILE_W,h=TV_TILE_H;
    vita2d_draw_rectangle(x,y,w,h,RGBA8(28,28,32,255));
    vita2d_texture *t=s->live?img_get(s->preview):NULL;
    if(t)ui_image(t,x,y,w,h);else draw_avatar(s->avatar,s->display,x+86,y+8,54);
    if(s->live){const char *live=vt_tr(VT_I_LIVE);int bw=ui_text_w(.68f,live)+12;if(bw<49)bw=49;if(bw>w-10)bw=w-10;ui_round_rect(x+5,y+5,bw,18,3,C_RED);ui_text_fit(x+10,y+18,bw-10,C_WHITE,.68f,live);}
    if(saved_has(s->login)&&!account_follow_known(s->login)){ui_round_rect(x+75,y+5,142,18,3,C_PANEL);ui_text_fit(x+79,y+18,134,C_WHITE,.65f,twitch_follow_label(0,1));}
    if(active)ui_frame(x-3,y-3,w+6,h+6,3,C_PURPLE);
    ui_text_fit(x,y+TV_NAME_BASELINE,w,C_WHITE,.79f,s->display[0]?s->display:s->login);
}
static void tv_row(int tab,int y,const char *label){
    tab_t *tb=&g_tabs[tab];int n=home_count(tb),pos=home_pos(tb);
    /* Following and top rows use their own selection; category filter applies only to Explore. */
    int grid=!g_tv_home&&(tab==TAB_FOLLOWED||(tab==TAB_EXPLORE&&!g_game_id[0]&&!g_game_filter[0]));
    int slots=grid?8:4;
    int first=pos>=0?(pos/slots)*slots:0;
    ui_text_fit(20,y-TV_SECTION_BASELINE,800,C_WHITE,.98f,label);
    if(!n){if(tb->busy||tb->state==LS_LOADING)ui_spinner(36,y+37,9,C_PURPLE);else ui_text_fit(20,y+40,900,C_GREY,.84f,tb->need_login?vt_tr(VT_I_ACCOUNT):tb->state==LS_ERROR?tb->err:vt_msg("Sin resultados"));return;}
    for(int k=0;k<slots&&first+k<n;k++){int idx=home_index(tb,first+k);if(idx>=0)tv_tile(&tb->items[idx],20+(k%4)*233,y+(k/4)*TV_ROW_STEP,!g_tv_nav_focus&&g_tab==tab&&idx==tb->sel);}
    if(first+slots<n)img_prefetch(tb->items[home_index(tb,first+slots)].preview);
    char page[32];snprintf(page,sizeof(page),"%d/%d",pos>=0?pos+1:1,n);ui_text_fit(864,y-TV_SECTION_BASELINE,77,C_GREY,.76f,page);
}
static void draw_tv_home(void){
    vita2d_draw_rectangle(0,0,SCREEN_W,SCREEN_H,RGBA8(14,14,16,255));
    vita2d_draw_rectangle(0,0,SCREEN_W,54,RGBA8(20,20,23,255));
    draw_glitch(25,10,1.45f,C_PURPLE);
    for(int i=0;i<TV_NAV_COUNT;i++){
        int x=TV_NAV_X+i*TV_NAV_STEP;
        if(g_tv_nav_focus&&g_tv_nav==i)ui_round_rect(x,8,TV_NAV_STEP-6,37,5,RGBA8(56,45,78,255));
        ui_text_fit(x+5,32,TV_NAV_STEP-14,g_tv_nav==i?C_WHITE:C_GREY,.84f,tv_nav_label(i));
        if(g_tv_nav==i)vita2d_draw_rectangle(x+5,45,TV_NAV_STEP-14,3,C_PURPLE);
    }
    tab_t *tb=&g_tabs[g_tab];int n=home_count(tb),pos=home_pos(tb);
    int idx=pos>=0?home_index(tb,pos):n?home_index(tb,0):-1;
    if(g_tab==TAB_SEARCH){
        ui_round_rect(20,60,920,35,5,C_PANEL);ui_text_fit(32,84,896,C_WHITE,.94f,g_search[0]?g_search:vt_tr(VT_I_SEARCH));
        if(!g_tv_nav_focus&&tb->sel<0)ui_frame(20,60,920,35,2,C_PURPLE);
    }
    int search_hero=!g_tv_home&&g_tab==TAB_SEARCH;
    int hy=search_hero?102:65,hh=search_hero?157:165;
    if(idx>=0){
        const twitch_stream_t *s=&tb->items[idx];char line[256],v[32];
        vita2d_texture *t=s->live?img_get(s->preview):NULL;
        int hw=hh*16/9;
        vita2d_draw_rectangle(20,hy,hw,hh,RGBA8(27,27,30,255));
        if(t)ui_image(t,20,hy,hw,hh);else draw_avatar(s->avatar,s->display,20+(hw-106)/2,hy+(hh-106)/2,106);
        ui_text_fit(397,hy+25,540,s->live?C_RED:C_GREY,.87f,s->live?vt_tr(VT_I_LIVE):vt_tr(VT_I_OFFLINE));
        ui_text_fit(397,hy+60,540,C_WHITE,1.48f,s->display[0]?s->display:s->login);
        ui_text_fit(397,hy+91,540,C_WHITE,1.03f,s->title);
        ui_fmt_viewers(s->viewers,v,sizeof(v));snprintf(line,sizeof(line),"%s  |  %s",s->game,s->live?v:vt_tr(VT_I_OFFLINE));
        ui_text_fit(397,hy+120,540,C_GREY,.87f,line);
        ui_round_rect(397,hy+132,170,30,5,C_PURPLE);ui_text_fit(409,hy+153,146,C_WHITE,.83f,s->live?vt_tr(VT_I_PLAY):profile_label(0));
    }else{
        ui_text_fit(22,hy+44,900,C_WHITE,1.4f,g_tab==TAB_FOLLOWED?following_mode():g_game_filter[0]?g_game_filter:channels_label());
        if(tb->busy||tb->state==LS_LOADING)ui_spinner(34,hy+82,12,C_PURPLE);
        else ui_text_fit(22,hy+84,900,C_GREY,.9f,tb->need_login?vt_tr(VT_I_ACCOUNT):tb->state==LS_ERROR?tb->err:vt_msg("Sin resultados"));
    }
    if(g_tv_home){
        tv_row(TAB_FOLLOWED,tv_row_top(),g_follow_offline==2?saved_title():vt_tr(VT_I_FOLLOWING));
        tv_row(TAB_EXPLORE,tv_row_top()+TV_ROW_STEP,vt_lang_current()==2?"Canais ao vivo populares":vt_lang_current()==1?"Canales en directo populares":"Top live channels");
    }else{
        tv_row(g_tab,tv_row_top(),g_tab==TAB_FOLLOWED?following_mode():g_tab==TAB_SEARCH?vt_tr(VT_I_SEARCH):g_game_filter[0]?g_game_filter:channels_label());
        if(g_tab==TAB_SEARCH||(g_tab==TAB_EXPLORE&&(g_game_id[0]||g_game_filter[0])))ui_text_fit(20,425,920,C_GREY,.9f,vt_lang_current()==2?"Use esquerda/direita para navegar • START para perfil":"Use left/right to browse • START for profile");
    }
    draw_footer();
    if(g_toast[0]&&sceKernelGetProcessTimeWide()<g_toast_until){ui_round_rect(100,218,760,37,5,C_PANEL2);ui_text_fit(116,243,728,C_WHITE,.9f,g_toast);}
}
static void (*g_media_backdrop)(void);
static void draw_home(void)
{
    if(g_media_backdrop){g_media_backdrop();return;}
    if(g_tv_layout){draw_tv_home();return;}
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
        ui_hint_bounded(x+560-160,y+210-22,136,g_ok_name,hint);
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
        ui_hint_bounded(x+460-180,y+150-20,156,g_back_name,vt_msg("Cancelar"));
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
static int play_viewers=-1;

static void play_channel(const char *login,const char *display,const char *title,int viewers)
{
    if(play_state!=PLAY_IDLE)return;
    play_viewers=viewers;
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

static const char *profile_sub_badge(void);
static void recordings_screen(const twitch_stream_t *channel);
static void recordings_screen_kind(const twitch_stream_t *channel,int initial);
static void own_profile(void);
static void channel_full_profile(const twitch_stream_t *channel,int own,int return_to_live);
static m3u8_preview_t live_capture;
static void clip_dialog(const char *login,const twitch_media_t *v,int pos,const m3u8_variant_t *quality,const m3u8_preview_t *snapshot);
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
    char channel_avatar[192]="",channel_game[96]="";
    for(int t=0;t<TAB_COUNT;t++){
        tab_t *tb=&g_tabs[t];
        for(int i=0;i<tb->count;i++)if(!strcmp(tb->items[i].login,login)){
            snprintf(channel_avatar,sizeof(channel_avatar),"%s",tb->items[i].avatar);
            snprintf(channel_game,sizeof(channel_game),"%s",tb->items[i].game);
            break;
        }
    }
    int retries=0, reductions=0, r=0, ad_switched=0;
    int requested=-1,rollback=-1,manual=0;
    static char ad_backup[M3U8_URL_MAX];
    ad_backup[0]=0;
    char wanted[48];
    static m3u8_variant_t previous;
    for(;;){
    memset(&pp, 0, sizeof(pp));
    pp.url = play_list[sel]->url;
    pp.channel = display && display[0] ? display : login;
    pp.title = clean_title;
    pp.saved_has=saved_has;pp.saved_toggle=saved_toggle;pp.saved_label=saved_label;pp.saved_ensure=saved_ensure;pp.account_follow_known=account_follow_known(login);
    pp.profile_avatar=channel_avatar;pp.profile_game=channel_game;
    pp.profile_label=profile_label(0);pp.follow_label=profile_label(1);
    pp.quality = play_list[sel]->name;
    pp.width = play_list[sel]->width;
    pp.height = play_list[sel]->height;
    pp.fps = (float)play_list[sel]->fps;
    pp.font = ui_font;
    pp.btn_back = g_btn_back;
    pp.login = play_job.login;
    pp.chat_nick = g_logged ? g_auth.login : NULL;
    pp.chat_oauth = g_logged ? g_auth.access : NULL;
    pp.chat_sender_id = g_logged ? g_auth.user_id : NULL;
    pp.chat_on = &g_chat_on;
    pp.viewer_count = &play_viewers;
    pp.variants=play_list;pp.variant_count=play_count;pp.variant_current=sel;
    requested=-1;pp.variant_requested=&requested;
    pp.profile=g_profile;pp.auto_quality=g_auto_quality&&!manual;
    pp.has_lower=vt_profile_lower(play_list,play_count,play_list[sel])>=0;
    pp.diagnostics=g_diag;pp.volume=&g_volume;
    pp.ad_backup_url=ad_switched?NULL:ad_backup;pp.live_capture=&live_capture;
    LOGI("startup: selected %s %s %dx%d %.0f fps", login, pp.quality, pp.width, pp.height, pp.fps);
    scePowerSetArmClockFrequency(vt_profile_clock(g_profile,pp.height,pp.fps));
    LOGI("startup: entering player");
    unsigned long long opened_at=sceKernelGetProcessTimeWide();
    r = player_run(&pp, err, sizeof(err));
    unsigned long long elapsed=sceKernelGetProcessTimeWide()-opened_at;
    LOGI("quality flow: player returned %d",r);
    scePowerSetArmClockFrequency(333);
    settings_save();
    if(r==PLAYER_CREATE_CLIP){clip_dialog(login,NULL,0,play_list[sel],&live_capture);r=PLAYER_RECONNECT;retries=0;}
    if(r==PLAYER_CHANNEL_VIDEOS||r==PLAYER_FULL_PROFILE){twitch_stream_t channel={0};snprintf(channel.login,sizeof(channel.login),"%s",login);snprintf(channel.display,sizeof(channel.display),"%s",display);snprintf(channel.avatar,sizeof(channel.avatar),"%s",channel_avatar);channel.live=1;channel.viewers=play_viewers;snprintf(channel.title,sizeof(channel.title),"%s",clean_title);if(r==PLAYER_FULL_PROFILE)channel_full_profile(&channel,0,1);else recordings_screen(&channel);r=PLAYER_RECONNECT;retries=0;}
    if(r==PLAYER_CHANGE_QUALITY&&requested>=0&&requested<play_count&&requested!=sel){
        rollback=sel;sel=requested;manual=1;retries=0;reductions=0;ad_switched=0;ad_backup[0]=0;
        snprintf(g_pref_quality,sizeof(g_pref_quality),"%s",play_list[sel]->name);
        settings_save();
        LOGI("player: manual quality change to %s",g_pref_quality);
        vita2d_start_drawing();vita2d_clear_screen();
        ui_spinner(SCREEN_W/2,SCREEN_H/2-32,14,C_PURPLE);
        ui_text_center(SCREEN_W/2,SCREEN_H/2+12,C_WHITE,1.0f,vt_tr(VT_I_QUALITY));
        ui_text_center(SCREEN_W/2,SCREEN_H/2+42,C_GREY,0.85f,g_pref_quality);
        frame_end();
        continue;
    }
    if(r<0&&rollback>=0&&rollback<play_count&&elapsed<15000000ULL){
        sel=rollback;rollback=-1;ad_switched=0;ad_backup[0]=0;
        snprintf(g_pref_quality,sizeof(g_pref_quality),"%s",play_list[sel]->name);
        settings_save();
        LOGW("player: selected quality failed, restoring %s",g_pref_quality);
        continue;
    }
    rollback=-1;
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
    if(g_tab!=TAB_FOLLOWED)tab_load(TAB_FOLLOWED);



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
    ui_hint_bounded(x+28,y+h-25,w/2-48,g_ok_name,vt_tr(VT_I_PLAY));
    ui_hint_bounded(x+w/2+20,y+h-25,w/2-48,g_back_name,vt_tr(VT_I_BACK));
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
            play_state=PLAY_LAUNCH; /* Always open directly; manual quality stays in player. */
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

/* Explicit confirmation is handled inside the profile Session tab. */
static void account_logout(void)
{
    LOGI("sesion cerrada por el usuario");
    if(g_auth_checking){
        task_cancel(&g_auth_task);
        task_wait(&g_auth_task);
        g_auth_checking=0;
        memset(&g_auth_job,0,sizeof(g_auth_job));
    }
    g_logged=0;
    memset(&g_auth,0,sizeof(g_auth));
    memset(account_oauth,0,sizeof(account_oauth));
    account_n=0;
    sceIoRemove(AUTH_FILE);
    g_tabs[TAB_FOLLOWED].count=0;
    tab_load(TAB_FOLLOWED);
    toast(vt_msg("Has cerrado sesion"));
}
static const char *session_text(int k)
{
    static const char *const labels[VT_LANG_COUNT][3]={
        {"Do you want to sign out?","Yes","No"},
        {"¿Deseas cerrar sesión?","Sí","No"},
        {"Deseja sair da conta?","Sim","Não"},
        {"Voulez-vous vous déconnecter ?","Oui","Non"},
        {"Vuoi uscire dall'account?","Sì","No"},
        {"Möchtest du dich abmelden?","Ja","Nein"},
        {"Выйти из аккаунта?","Да","Нет"},
        {"Ingin keluar dari akun?","Ya","Tidak"},
        {"Gusto mo bang mag-sign out?","Oo","Hindi"},
        {"Gusto ka bang mogawas sa account?","Oo","Dili"},
        {"Wil je uitloggen?","Ja","Nee"},
        {"Czy chcesz się wylogować?","Tak","Nie"},
        {"Çıkış yapmak istiyor musun?","Evet","Hayır"},
        {"Вийти з облікового запису?","Так","Ні"},
        {"Vill du logga ut?","Ja","Nej"},
        {"ログアウトしますか？","はい","いいえ"},
        {"로그아웃하시겠습니까?","예","아니요"},
        {"要退出登录吗？","是","否"},
        {"要登出帳號嗎？","是","否"}
    };
    int l=vt_lang_current();return labels[l>=0&&l<VT_LANG_COUNT?l:0][k];
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
        ui_text_fit(x+32,y+50,w-64,C_WHITE,1.3f,vt_msg("Iniciar sesion en Twitch"));
        ui_text_fit(x+32,y+92,w-64,C_GREY,0.95f,vt_msg("1. En tu movil o PC abre:"));
        ui_text(x + 60, y + 128, C_PURPLE, 1.4f, "twitch.tv/activate");
        ui_text_fit(x+32,y+168,w-64,C_GREY,0.95f,vt_msg("2. Inicia sesion e introduce este codigo:"));
        ui_round_rect(x + 60, y + 184, 300, 64, 10, C_PANEL2);
        ui_text_center(x + 210, y + 230, C_WHITE, 2.0f, code);
        ui_spinner(x + 400, y + 216, 10, C_PURPLE);
        ui_text_fit(x+422,y+222,w-450,C_GREY,0.85f,vt_msg("Esperando..."));
        unsigned long long left = expires > now ? (expires - now) / 1000000ULL : 0;
        char t[64];
        snprintf(t, sizeof(t), vt_msg("El codigo caduca en %llu:%02llu"), left / 60, left % 60);
        ui_text_fit(x+32,y+h-50,w-64,C_GREY,0.8f,t);
        footer_hint(x+w-200,176,y+h-22,g_back_name,vt_msg("Cancelar"));
        frame_end();
    }
}

static void account_screen(void)
{
    if(g_logged)own_profile();
    else {account_login();if(g_logged)own_profile();}
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
            vt_lang_set(sel);g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;
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
        ui_hint_bounded(x+24,y+h-14,(w-48)/2,g_ok_name,vt_tr(VT_I_PLAY));
        ui_hint_bounded(x+w/2,y+h-14,w/2-24,g_back_name,vt_tr(VT_I_BACK));
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
            g_region=sel;g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;settings_save();
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
        ui_hint_bounded(x+24,y+h-22,w/2-32,g_ok_name,vt_tr(VT_I_PLAY));
        ui_hint_bounded(x+w/2,y+h-22,w/2-24,g_back_name,vt_tr(VT_I_BACK));
        vita2d_end_drawing();vita2d_swap_buffers();dirty=0;
        sceKernelDelayThread(8000);
    }
}
static void recordings_screen(const twitch_stream_t *channel);
static const char *setting_group(int i){
    static const char *pt[]={"Informações de streaming","Interface e idioma","Conta e privacidade","Armazenamento e sobre","Voltar"};
    static const char *es[]={"Información de streaming","Interfaz e idioma","Cuenta y privacidad","Almacenamiento y acerca de","Volver"};
    static const char *en[]={"Streaming information","Interface and language","Account and privacy","Storage and about","Back"};
    return (vt_lang_current()==2?pt:vt_lang_current()==1?es:en)[i];
}
static const char *setting_label(int id){
    switch(id){
    case 1:return vt_tr(VT_I_LANGUAGE);case 2:return vt_tr(VT_I_TOUCH);case 3:return vt_tr(VT_I_CHAT);
    case 4:return vt_tr(VT_I_CLEAR_CACHE);case 5:return vt_tr(VT_I_ABOUT);
    case 6:return vt_opt(VT_O_PROFILE);case 7:return vt_opt(VT_O_AUTO);case 8:return vt_opt(VT_O_DIAG);
    case 9:return vt_opt(VT_O_REMEMBER);case 10:return region_title();
    case 11:return vt_lang_current()==2?"Modo de interface":"Layout";
    default:return vt_tr(VT_I_BACK);}
}
static void setting_action(int id){
    switch(id){
    case 1:language_screen();break;case 2:ui_touch_enable(!ui_touch_enabled());break;case 3:g_chat_on=!g_chat_on;break;
    case 4:img_clear();toast(vt_msg("Thumbnail cache cleared"));break;
    case 5:modal_message("VitaTwitch — Credits",vt_lang_current()==2?"Original: Dexlei. Dono e mantenedor do fork: joaopedr00000 (GitHub). Apaixonado por consoles portáteis, adoro contribuir com a comunidade.":vt_lang_current()==1?"Original: Dexlei. Dueño y mantenedor del fork: joaopedr00000 (GitHub). Me apasionan las consolas portátiles y contribuir a la comunidad.":"Original: Dexlei. Fork owner and maintainer: joaopedr00000 (GitHub). I love handheld consoles and contributing to the community.");break;
    case 6:g_profile=(g_profile+1)%VT_PROFILE_COUNT;break;case 7:g_auto_quality=!g_auto_quality;break;case 8:g_diag=!g_diag;break;
    case 9:g_remember=!g_remember;if(g_remember&&g_logged)auth_save(&g_auth,AUTH_FILE);else sceIoRemove(AUTH_FILE);break;
    case 10:region_screen();break;
    case 11:g_tv_layout=!g_tv_layout;g_tv_home=1;g_tv_nav=0;g_tv_nav_focus=0;g_side_focus=-1;g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;g_tabs[g_tab].scroll=0;break;
    }settings_save();
}
static void extras_menu(void){
    static const int ids[4][6]={{3,6,7,8,10,0},{1,2,11,0,0,0},{9,0,0,0,0,0},{4,5,0,0,0,0}};
    static const int counts[]={6,4,2,3};
    int group=-1,sel=0,armed=0;
    for(;;){
        int n=group<0?5:counts[group],x=130,y=58,w=700,h=420,row=49;
        ui_input_poll(&g_in);unsigned b=g_in.pressed;
        if(!(g_in.held&(g_btn_ok|g_btn_back|SCE_CTRL_SELECT))&&!g_in.touch_down)armed=1;
        if(armed&&(b&(g_btn_back|SCE_CTRL_SELECT))){if(group<0)return;sel=group;group=-1;armed=0;continue;}
        if(b&SCE_CTRL_UP)sel=(sel+n-1)%n;if(b&SCE_CTRL_DOWN)sel=(sel+1)%n;
        int choose=armed&&(b&g_btn_ok);
        for(int i=0;i<n;i++)if(armed&&ui_touch_hit(&g_in,x+20,y+70+i*row,w-40,row-4)){sel=i;choose=1;break;}
        if(choose){armed=0;if(group<0){if(sel==4)return;group=sel;sel=0;continue;}
            int id=ids[group][sel];if(!id){sel=group;group=-1;continue;}setting_action(id);}
        frame_begin();draw_home();int ox,oy;draw_modal_box(w,h,&ox,&oy);
        ui_text_fit(x+24,y+42,w-48,C_WHITE,1.2f,group<0?vt_tr(VT_I_SETTINGS):setting_group(group));
        for(int i=0;i<n;i++){
            int yy=y+70+i*row,id=group<0?0:ids[group][i];
            ui_round_rect(x+20,yy,w-40,row-4,5,i==sel?CLASSIC_PURPLE:C_PANEL);
            ui_text_fit(x+34,yy+29,w-250,C_WHITE,1.0f,group<0?setting_group(i):setting_label(id));
            const char *value=NULL;
            if(id==1)value=vt_lang_name(vt_lang_current());if(id==6)value=vt_opt((vt_option_id)(VT_O_ECONOMY+g_profile));if(id==10)value=region_label(g_region);
            if(id==11)value=g_tv_layout?"TV":"Handheld";
            if(id==2||id==3||id==7||id==8||id==9){int on=id==2?ui_touch_enabled():id==3?g_chat_on:id==7?g_auto_quality:id==8?g_diag:g_remember;value=vt_tr(on?VT_I_ON:VT_I_OFF);}
            if(value)ui_text_fit(x+w-210,yy+29,170,C_GREY,.9f,value);
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
    side_icons_init();
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

typedef struct {char login[32],bio[512];} profile_job_t;
static int profile_run(void *arg,volatile int *abort_flag)
{
    profile_job_t *j=arg;return twitch_fetch_channel_bio(j->login,j->bio,sizeof(j->bio),abort_flag);
}
#define MEDIA_FILE DATA_DIR "/videos-saved.tsv"
#define MEDIA_LIMIT 64
typedef struct {twitch_media_t v;int pos,saved;} media_saved_t;
static media_saved_t media_saved[MEDIA_LIMIT];
static int media_saved_n,media_loaded;
static int media_find(const char *id){for(int i=0;i<media_saved_n;i++)if(!strcmp(media_saved[i].v.id,id))return i;return -1;}
static void media_load(void){
    if(media_loaded)return;media_loaded=1;FILE *f=fopen(MEDIA_FILE,"r");if(!f)f=fopen(MEDIA_FILE ".bak","r");if(!f)return;
    char line[512];while(media_saved_n<MEDIA_LIMIT&&fgets(line,sizeof(line),f)){
        media_saved_t e={0};char *parts[8];int n=0;char *s=line;
        while(n<8){parts[n++]=s;char *tab=strchr(s,'\t');if(!tab)break;*tab=0;s=tab+1;}
        if(n!=8)continue;parts[7][strcspn(parts[7],"\r\n")]=0;
        if(!parts[0][0]||strlen(parts[0])>=sizeof(e.v.id)||media_find(parts[0])>=0)continue;
        saved_field(e.v.id,sizeof(e.v.id),parts[0]);saved_field(e.v.channel,sizeof(e.v.channel),parts[1]);saved_field(e.v.broadcaster,sizeof(e.v.broadcaster),parts[2]);
        saved_field(e.v.type,sizeof(e.v.type),parts[3]);e.v.seconds=atoi(parts[4]);e.pos=atoi(parts[5]);e.saved=atoi(parts[6])!=0;saved_field(e.v.title,sizeof(e.v.title),parts[7]);
        if(e.pos<0)e.pos=0;media_saved[media_saved_n++]=e;
    }fclose(f);
}
static int media_save(void){
    ensure_data_dir();FILE *f=fopen(MEDIA_FILE ".tmp","w");if(!f)return -1;int rc=0;
    for(int i=0;i<media_saved_n;i++){
        media_saved_t *e=&media_saved[i];char title[192];saved_field(title,sizeof(title),e->v.title);
        if(fprintf(f,"%s\t%s\t%s\t%s\t%d\t%d\t%d\t%s\n",e->v.id,e->v.channel,e->v.broadcaster,e->v.type,e->v.seconds,e->pos,e->saved,title)<0)rc=-1;
    }
    if(fclose(f))rc=-1;if(rc<0){sceIoRemove(MEDIA_FILE ".tmp");return -1;}
    sceIoRemove(MEDIA_FILE ".bak");int backed=sceIoRename(MEDIA_FILE,MEDIA_FILE ".bak")>=0;
    if(sceIoRename(MEDIA_FILE ".tmp",MEDIA_FILE)<0){if(backed)sceIoRename(MEDIA_FILE ".bak",MEDIA_FILE);return -1;}sceIoRemove(MEDIA_FILE ".bak");return 0;
}
static int media_remember(const twitch_media_t *v,int pos,int toggle){
    media_load();int i=media_find(v->id);
    if(i<0){if(media_saved_n>=MEDIA_LIMIT){for(int k=0;k<media_saved_n;k++)if(!media_saved[k].saved){i=k;break;}if(i<0)return -1;}else i=media_saved_n++;
        memset(&media_saved[i],0,sizeof(media_saved[i]));}
    media_saved[i].v=*v;if(pos>=0)media_saved[i].pos=pos;if(toggle)media_saved[i].saved=!media_saved[i].saved;
    return media_save();
}
typedef struct {char login[32],oauth[128],err[192];int kind,n;twitch_media_t out[VT_MEDIA_MAX];} media_job_t;
typedef struct {char id[80],oauth[128],err[192];int n;m3u8_variant_t v[M3U8_MAX_VARIANTS];} vod_job_t;
static int media_job_run(void *arg,volatile int *abort_flag){media_job_t *j=arg;j->n=twitch_fetch_media(j->login,j->oauth,j->kind,j->out,VT_MEDIA_MAX,j->err,sizeof(j->err),abort_flag);return j->n;}
static int vod_job_run(void *arg,volatile int *abort_flag){vod_job_t *j=arg;j->n=twitch_vod_variants(j->id,j->oauth,j->v,M3U8_MAX_VARIANTS,j->err,sizeof(j->err),abort_flag);return j->n;}
static int clip_preview(const char *login,const twitch_media_t *v,const m3u8_variant_t *q,const m3u8_preview_t *snapshot,int start,int end){
    if(!q||(!v&&(!snapshot||!snapshot->duration)))return -1;
    player_params_t pp={0};char err[192]="";int pos=start;
    pp.recorded=1;pp.start_seconds=start;pp.duration_seconds=v?v->seconds:snapshot->duration;pp.preview_end=end;pp.position_seconds=&pos;
    pp.preview_playlist=v?NULL:snapshot->text;pp.url=q->url;pp.channel=login;pp.title=v?v->title:"Clip preview";
    pp.quality=q->name;pp.width=q->width;pp.height=q->height;pp.fps=q->fps;pp.font=ui_font;pp.btn_back=g_btn_back;pp.profile=g_profile;pp.volume=&g_volume;
    scePowerSetArmClockFrequency(vt_profile_clock(g_profile,pp.height,pp.fps));
    int rc=player_run(&pp,err,sizeof(err));scePowerSetArmClockFrequency(333);
    if(rc<0||rc==PLAYER_RECONNECT){modal_message("Preview",err[0]?err:"Preview unavailable");return -1;}
    return rc==PLAYER_RECORDING_END?1:0;
}
static void clip_dialog(const char *login,const twitch_media_t *v,int pos,const m3u8_variant_t *quality,const m3u8_preview_t *snapshot){
    int total=v?v->seconds:snapshot?snapshot->duration:0;
    if(v&&total<pos)total=pos;
    int end=v?pos:total,duration=end<30?end:30;
    if(!v&&total<5){duration=30;end=30;}
    if(v&&end<5){modal_message("Clips","Minimum clip length: 5s");return;}
    int selected=0,loop=0,armed=0;char title[96]="VitaTwitch clip";
    for(;;){
        ui_input_poll(&g_in);unsigned b=g_in.pressed;
        if(!(g_in.held&(g_btn_ok|g_btn_back|SCE_CTRL_LTRIGGER))&&!g_in.touch_down)armed=1;
        if(armed&&(b&g_btn_back))return;
        if(b&SCE_CTRL_UP)selected=(selected+6)%7;if(b&SCE_CTRL_DOWN)selected=(selected+1)%7;
        int adjust=(b&SCE_CTRL_RIGHT)?1:(b&SCE_CTRL_LEFT)?-1:0;
        if(adjust&&selected==0){duration+=adjust*5;if(duration<5)duration=5;if(duration>60)duration=60;if((v||total>=5)&&duration>end)duration=end;}
        if(adjust&&selected==1&&total>=5){end+=adjust*5;if(end<duration)end=duration;if(end>total)end=total;}
        int choose=armed&&(b&g_btn_ok);
        for(int i=0;i<7;i++)if(armed&&ui_touch_hit(&g_in,140,106+i*46,680,42)){selected=i;choose=1;break;}
        if(choose){
            armed=0;
            if(selected==2){char edit[96];if(ime_input("Clip title",title,edit,sizeof(edit),80)&&edit[0])snprintf(title,sizeof(title),"%s",edit);}
            if(selected==3){
                if(!v&&total<5)modal_message("Preview",vt_lang_current()==2?"Histórico da live ainda insuficiente.":"Not enough live history.");
                else {int rc;do{rc=clip_preview(login,v,quality,snapshot,end-duration,end);}while(loop&&rc==1);}
            }
            if(selected==4)loop=!loop;
            if(selected==6)return;
            if(selected==5){
                if(!g_logged){modal_message("Clips",vt_lang_current()==2?"Entre na conta Twitch para publicar clipes.":"Sign in to publish clips.");continue;}
                /* Live endpoint selects the server's current moment, not this preview. */
                if(!v){
                    int confirmed=0,ready=0;
                    for(;;){ui_input_poll(&g_in);if(!(g_in.held&(g_btn_ok|g_btn_back))&&!g_in.touch_down)ready=1;
                        if(ready&&(g_in.pressed&g_btn_back))break;
                        if(ready&&(g_in.pressed&g_btn_ok)){confirmed=1;break;}
                        frame_begin();draw_home();int x,y;draw_modal_box(730,240,&x,&y);
                        ui_text_fit(x+24,y+39,682,C_WHITE,1.1f,vt_lang_current()==2?"Confirmar clipe da live":"Confirm live clip");
                        wrap_text(x+24,y+85,682,C_GREY,.9f,vt_lang_current()==2?"A Twitch seleciona o momento atual do servidor. O clipe publicado pode ser diferente da prévia. Confirmar ou voltar?":"Twitch selects the current server moment. The published clip may differ from the preview. Confirm or go back?",4);frame_end();}
                    if(!confirmed){armed=0;continue;}
                }
                break;
            }
        }
        frame_begin();draw_home();int x,y;draw_modal_box(720,470,&x,&y);
        ui_text_fit(140,79,680,C_WHITE,1.18f,vt_lang_current()==2?"Editor de clipes (experimental)":"Clip editor (experimental)");
        char d[96],range[96],tl[128],lp[64];
        snprintf(d,sizeof(d),"%s: %d s",vt_lang_current()==2?"Duração":"Duration",duration);
        snprintf(range,sizeof(range),"%s: %d - %d s",vt_lang_current()==2?"Trecho":"Range",end-duration,end);
        snprintf(tl,sizeof(tl),"%s: %s",vt_lang_current()==2?"Título":"Title",title);
        snprintf(lp,sizeof(lp),"%s: %s",vt_lang_current()==2?"Repetir prévia":"Loop preview",loop?"ON":"OFF");
        const char *labels[]={d,range,tl,vt_lang_current()==2?"Pré-visualizar":"Preview",lp,vt_lang_current()==2?"Publicar":"Publish",vt_lang_current()==2?"Cancelar":"Cancel"};
        for(int i=0;i<7;i++){int yy=106+i*46;ui_round_rect(140,yy,680,42,5,selected==i?C_PURPLE:C_PANEL);ui_text_fit(154,yy+28,652,C_WHITE,.96f,labels[i]);}
        ui_text_fit(140,451,680,C_GREY,.78f,vt_lang_current()==2?"Direcional ajusta duração/fim; prévia alinhada ao segmento HLS.":"D-pad adjusts duration/end; preview is HLS segment-aligned.");
        ui_text_fit(140,475,680,C_GREY,.78f,vt_lang_current()==2?"Confirme Publicar para enviar à Twitch.":"Select Publish to send to Twitch.");frame_end();
    }
    twitch_clip_job_t j={0};snprintf(j.login,sizeof(j.login),"%s",login?login:"");snprintf(j.oauth,sizeof(j.oauth),"%s",g_auth.access);snprintf(j.user_id,sizeof(j.user_id),"%s",g_auth.user_id);
    j.duration=duration;j.offset=end;snprintf(j.title,sizeof(j.title),"%s",title);
    if(v){snprintf(j.vod_id,sizeof(j.vod_id),"%s",v->id);snprintf(j.broadcaster,sizeof(j.broadcaster),"%s",v->broadcaster);}
    task_t task={0};int rc=run_with_spinner(&task,twitch_create_clip,&j,vt_lang_current()==2?"Criando clipe na Twitch...":"Creating clip on Twitch...");
    if(j.confirmed)modal_message(vt_lang_current()==2?"Clipe confirmado":"Clip confirmed",j.url);
    else if(j.id[0])modal_message(vt_lang_current()==2?"Clipe ainda não confirmado":"Clip not confirmed",j.url);
    else if(rc==0)modal_message("Clips",j.error[0]?j.error:"Clip failed");
}
typedef struct {char id[80],url[M3U8_URL_MAX],err[192],vod[32];int height,rc,offset;} clip_mp4_job_t;
static int clip_mp4_job_run(void *arg,volatile int *abort_flag){clip_mp4_job_t *j=arg;j->rc=twitch_clip_mp4(j->id,j->url,sizeof(j->url),&j->height,j->err,sizeof(j->err),j->vod,sizeof(j->vod),&j->offset,abort_flag);return j->rc;}
static int play_clip_mp4(twitch_media_t *v,char *failure,size_t failure_cap,int can_previous,int can_next,int *position){
    static clip_mp4_job_t j;memset(&j,0,sizeof(j));j.rc=-1;snprintf(j.id,sizeof(j.id),"%s",v->id);snprintf(j.vod,sizeof(j.vod),"%s",v->vod_id);j.offset=v->offset;
    task_t task={0};if(run_with_spinner(&task,clip_mp4_job_run,&j,"Clip MP4...")<0)return 0;
    if(j.rc<0){snprintf(failure,failure_cap,"%s",j.err);if(j.vod[0]&&j.offset>=0){snprintf(v->vod_id,sizeof(v->vod_id),"%s",j.vod);v->offset=j.offset;}return -1;}
    int pos=*position;
    for(;;){
    player_params_t pp={0};char err[192]="";int target=pos;
    pp.recorded=2;pp.media_navigation=1;pp.can_previous=can_previous;pp.can_next=can_next;
    pp.duration_seconds=v->seconds>0?v->seconds:60;pp.start_seconds=pos;pp.position_seconds=&pos;pp.seek_requested=&target;pp.loading_backdrop=g_media_backdrop;
    pp.url=j.url;pp.channel=v->channel;pp.title=v->title;pp.quality="MP4";pp.height=j.height;pp.width=(j.height*16/9+1)&~1;pp.fps=30;
    pp.font=ui_font;pp.btn_back=g_btn_back;pp.profile=g_profile;pp.volume=&g_volume;
    scePowerSetArmClockFrequency(vt_profile_clock(g_profile,pp.height,pp.fps));int rc=player_run(&pp,err,sizeof(err));scePowerSetArmClockFrequency(333);
    *position=pos;
    if(rc==PLAYER_SEEK){pos=target;*position=pos;continue;}
    if(rc==PLAYER_PREVIOUS_MEDIA||rc==PLAYER_NEXT_MEDIA)return rc;
    if(rc<0||rc==PLAYER_RECONNECT){snprintf(failure,failure_cap,"%s",err[0]?err:"MP4 playback failed");return -1;}return 0;
    }
}
static int recorded_play(twitch_media_t *v,int can_previous,int can_next){
    if(!strcmp(v->type,"clip")){
        twitch_media_t resolved=*v;char failure[192]="";v=&resolved;
        int clip_pos=0,mp4_rc=play_clip_mp4(&resolved,failure,sizeof(failure),can_previous,can_next,&clip_pos);if(mp4_rc>=0)return mp4_rc;
        if(!v->vod_id[0]||v->offset<0||v->seconds<1||v->offset>604800-v->seconds){char message[256];snprintf(message,sizeof(message),"%s\n%s",failure,vt_lang_current()==2?"Sem gravação de origem disponível.":"No source VOD available.");modal_message("Clip",message);return 0;}
        static vod_job_t clip_job;memset(&clip_job,0,sizeof(clip_job));snprintf(clip_job.id,sizeof(clip_job.id),"%s",v->vod_id);snprintf(clip_job.oauth,sizeof(clip_job.oauth),"%s",g_logged?g_auth.access:"");
        task_t ct={0};if(run_with_spinner(&ct,vod_job_run,&clip_job,vt_lang_current()==2?"Carregando clipe pela gravação...":"Loading clip from source VOD...")<0)return 0;
        m3u8_variant_t *options[M3U8_MAX_VARIANTS];int cn=0;
        for(int i=0;i<clip_job.n;i++)if(!clip_job.v[i].audio_only&&clip_job.v[i].height>0&&clip_job.v[i].height<=720&&!strncmp(clip_job.v[i].url,"https://",8))options[cn++]=&clip_job.v[i];
        if(!cn){modal_message("Clip",clip_job.err[0]?clip_job.err:"Source VOD unavailable");return 0;}
        int cs=vt_profile_pick(options,cn,g_profile);if(cs<0||cs>=cn)cs=0;
        for(;;){
            player_params_t pp={0};char err[192]="";int target=clip_pos;
            m3u8_variant_t *q=options[cs];int requested=cs;
            pp.recorded=1;pp.media_navigation=1;pp.can_previous=can_previous;pp.can_next=can_next;
            pp.timeline_offset=v->offset;pp.start_seconds=v->offset+clip_pos;pp.duration_seconds=v->seconds;pp.preview_end=v->seconds;pp.position_seconds=&clip_pos;pp.seek_requested=&target;
            pp.url=q->url;pp.channel=v->channel;pp.title=v->title;pp.quality=q->name;pp.width=q->width;pp.height=q->height;pp.fps=q->fps;
            pp.font=ui_font;pp.btn_back=g_btn_back;pp.profile=g_profile;pp.volume=&g_volume;pp.loading_backdrop=g_media_backdrop;
            pp.variants=options;pp.variant_count=cn;pp.variant_current=cs;pp.variant_requested=&requested;
            scePowerSetArmClockFrequency(vt_profile_clock(g_profile,pp.height,pp.fps));int rc=player_run(&pp,err,sizeof(err));scePowerSetArmClockFrequency(333);
            if(rc==PLAYER_SEEK){clip_pos=target;continue;}
            if(rc==PLAYER_CHANGE_QUALITY&&requested>=0&&requested<cn){cs=requested;continue;}
            if(rc==PLAYER_PREVIOUS_MEDIA||rc==PLAYER_NEXT_MEDIA)return rc;
            if(rc<0||rc==PLAYER_RECONNECT)modal_message("Clip",err[0]?err:"Clip playback failed");
            return 0;
        }
    }
    static vod_job_t j;memset(&j,0,sizeof(j));snprintf(j.id,sizeof(j.id),"%s",v->id);snprintf(j.oauth,sizeof(j.oauth),"%s",g_logged?g_auth.access:"");
    task_t task={0};if(run_with_spinner(&task,vod_job_run,&j,vt_lang_current()==2?"Carregando gravação...":"Loading recording...")<0)return 0;
    if(j.n<=0){if(!strcmp(j.err,twitch_vod_access_message("SUBSCRIPTION")))v->subscriber_only=1;modal_message("VOD",j.err[0]?j.err:twitch_vod_access_message("UNKNOWN"));return 0;}
    m3u8_variant_t *variants[M3U8_MAX_VARIANTS];int n=0;
    for(int i=0;i<j.n;i++)if(!j.v[i].audio_only&&j.v[i].height>0&&j.v[i].height<=720&& !strncmp(j.v[i].url,"https://",8))variants[n++]=&j.v[i];
    if(!n){modal_message("VOD","No compatible H.264 quality <= 720p");return 0;}
    int sel=vt_profile_pick(variants,n,g_profile);if(sel<0||sel>=n)sel=0;
    media_load();int index=media_find(v->id),pos=index>=0?media_saved[index].pos:0;
    if(pos<0||(v->seconds>0&&pos>=v->seconds-2))pos=0;
    for(;;){
        player_params_t pp={0};m3u8_variant_t *q=variants[sel];char err[192]="";int target=pos,requested=sel;
        pp.recorded=1;pp.loading_backdrop=g_media_backdrop;pp.media_navigation=1;pp.can_previous=can_previous;pp.can_next=can_next;pp.start_seconds=pos;pp.duration_seconds=v->seconds;pp.position_seconds=&pos;pp.seek_requested=&target;
        pp.url=q->url;pp.channel=v->channel;pp.title=v->title;pp.quality=q->name;pp.width=q->width;pp.height=q->height;pp.fps=q->fps;
        pp.font=ui_font;pp.btn_back=g_btn_back;pp.profile=g_profile;pp.volume=&g_volume;pp.variants=variants;pp.variant_count=n;pp.variant_current=sel;pp.variant_requested=&requested;
        scePowerSetArmClockFrequency(vt_profile_clock(g_profile,pp.height,pp.fps));
        int rc=player_run(&pp,err,sizeof(err));
        scePowerSetArmClockFrequency(333);
        if(rc==PLAYER_RECORDING_END)pos=0;
        if(rc==PLAYER_SEEK)pos=target;
        if(media_remember(v,pos,0)<0)toast("Could not save playback position");
        if(rc==PLAYER_PREVIOUS_MEDIA||rc==PLAYER_NEXT_MEDIA)return rc;
        if(rc==PLAYER_SEEK)continue;
        if(rc==PLAYER_CHANGE_QUALITY&&requested>=0&&requested<n){sel=requested;continue;}
        if(rc==PLAYER_CREATE_CLIP){clip_dialog(v->channel,v,pos,q,NULL);continue;}
        if(rc<0||rc==PLAYER_RECONNECT)modal_message("VOD",err[0]?err:"Recording playback failed");
        break;
    }
    return 0;
}
static const char *media_kind(int k){static const char *pt[]={"Todos","Gravações","Destaques","Publicados","Clipes","Salvos","Continuar"};static const char *en[]={"All","Past broadcasts","Highlights","Uploads","Clips","Saved","Continue"};return vt_lang_current()==2?pt[k]:en[k];}
static void recordings_screen(const twitch_stream_t *channel){recordings_screen_kind(channel,channel?0:5);}
static void recordings_screen_kind(const twitch_stream_t *channel,int initial){
    static media_job_t j;int kind=initial,sel=0;
    media_load();
    for(;;){
        memset(&j,0,sizeof(j));
        if(kind>=5){
            for(int i=0;i<media_saved_n&&j.n<VT_MEDIA_MAX;i++)if((kind==5?media_saved[i].saved:media_saved[i].pos>0)&&(!channel||!strcmp(channel->login,media_saved[i].v.channel)))j.out[j.n++]=media_saved[i].v;
        }else{
            snprintf(j.login,sizeof(j.login),"%s",channel->login);snprintf(j.oauth,sizeof(j.oauth),"%s",g_logged?g_auth.access:"");j.kind=kind;
            task_t task={0};if(run_with_spinner(&task,media_job_run,&j,vt_lang_current()==2?"Carregando vídeos...":"Loading videos...")<0)return;
        }
        if(sel<0||sel>=j.n)sel=0;int reload=0;
        while(!reload){
            ui_input_poll(&g_in);unsigned b=g_in.pressed;
            if(b&(g_btn_back|SCE_CTRL_SELECT))return;
            if(b&SCE_CTRL_TRIANGLE){reload=1;break;}
            if(b&(SCE_CTRL_LTRIGGER|SCE_CTRL_RTRIGGER)){kind=channel?(kind+(b&SCE_CTRL_RTRIGGER?1:6))%7:kind==5?6:5;sel=0;break;}
            if(j.n>0){
                if(b&SCE_CTRL_UP)sel=(sel+j.n-1)%j.n;if(b&SCE_CTRL_DOWN)sel=(sel+1)%j.n;
                int top=sel/5*5;
                for(int k=0;k<5&&top+k<j.n;k++)if(ui_touch_hit(&g_in,42,100+k*78,876,74)){sel=top+k;b|=g_btn_ok;break;}
                if(b&SCE_CTRL_SQUARE){if(!strcmp(j.out[sel].type,"clip"))toast("Clip bookmarks not available");else if(media_remember(&j.out[sel],-1,1)<0)toast("Could not save video");else {toast(vt_lang_current()==2?"Favorito atualizado":"Bookmark updated");if(kind>=5)reload=1;}}
                if(b&g_btn_ok){
                    for(;;){
                        int prev=sel-1,next=sel+1;
                        int rc=recorded_play(&j.out[sel],prev>=0,next<j.n);
                        if(rc==PLAYER_PREVIOUS_MEDIA&&prev>=0){sel=prev;continue;}
                        if(rc==PLAYER_NEXT_MEDIA&&next<j.n){sel=next;continue;}
                        break;
                    }
                    ui_input_poll(&g_in);
                    if(kind>=5)reload=1;
                }
            }
            frame_begin();vita2d_draw_rectangle(0,0,960,544,CLASSIC_BG);
            ui_text_fit(28,34,900,C_WHITE,1.25f,channel?channel->display:vt_lang_current()==2?"Biblioteca de vídeos":"Video library");
            ui_text_fit(28,72,900,C_GREY,.96f,media_kind(kind));
            if(j.n<0){wrap_text(42,145,876,C_GREY,.92f,j.err,3);ui_text_fit(42,235,876,C_WHITE,.9f,vt_lang_current()==2?"Triângulo: tentar novamente":"Triangle: retry");}
            else if(!j.n)ui_text_fit(42,145,876,C_GREY,.9f,vt_lang_current()==2?"Nenhum vídeo":"No videos");
            int top=sel/5*5;
            for(int k=0;k<5&&top+k<j.n;k++){
                int i=top+k,y=100+k*78;twitch_media_t *v=&j.out[i];vita2d_draw_rectangle(42,y,876,74,i==sel?C_PURPLE:C_PANEL);
                vita2d_texture *tex=v->thumb[0]?img_get(v->thumb):NULL;if(tex)ui_image(tex,46,y+4,116,66);
                ui_text_fit(174,y+27,720,C_WHITE,1.0f,v->title);
                char line[160];int ix=media_find(v->id);snprintf(line,sizeof(line),"%.10s | %02d:%02d:%02d | %s%s",v->date,v->seconds/3600,v->seconds/60%60,v->seconds%60,v->type,ix>=0&&media_saved[ix].saved?" | Saved":"");
                ui_text_fit(174,y+55,v->subscriber_only?430:720,C_GREY,.84f,line);
                if(v->subscriber_only){ui_round_rect(620,y+34,284,28,4,C_SCRIM);ui_text_fit(630,y+54,264,C_WHITE,.84f,profile_sub_badge());}
            }
            ui_text_fit(28,525,900,C_WHITE,.88f,vt_lang_current()==2?"L/R Filtro | X Assistir | Quadrado Salvar | O Voltar":"L/R Filter | Confirm Play | Square Save | Back");
            frame_end();
        }
    }
}
static void profile_screen(const twitch_stream_t *s)
{
    char url[96],line[160];
    if(twitch_channel_url(s->login,url,sizeof(url))<0)return;
    profile_job_t job={0};snprintf(job.login,sizeof(job.login),"%s",s->login);
    task_t task={0},follow_task={0};
    twitch_follow_job_t follow={0};follow.state=-1;
    snprintf(follow.login,sizeof(follow.login),"%s",s->login);
    snprintf(follow.oauth,sizeof(follow.oauth),"%s",g_logged?g_auth.access:"");
    snprintf(follow.user_id,sizeof(follow.user_id),"%s",g_logged?g_auth.user_id:"");
    int follow_busy=follow.oauth[0]&&task_start(&follow_task,twitch_follow_run,&follow)==0;
    int known=account_follow_known(s->login);
    int follow_state=known?1:-1,follow_status=follow_busy?2:known?TW_FOLLOW_ALREADY:0;
    int followers=-1;char created[32]="",started[32]="",avatar[192]="";
    int loading=task_start(&task,profile_run,&job)==0,selected=s->live?0:1,armed=0;
    const int x=92,y=32,w=776,h=480,bx=x+24,by=y+302,bw=w-48,rh=39;
    for(;;){
        ui_input_poll(&g_in);
        if(loading&&task_done(&task)){task_wait(&task);loading=0;}
        if(follow_busy&&task_done(&follow_task)){follow_busy=0;follow_state=follow.state<0&&known?1:follow.state;if(follow_status<TW_FOLLOW_LOCAL_SAVED)follow_status=follow_state==1?TW_FOLLOW_ALREADY:follow.status;followers=follow.followers;snprintf(created,sizeof(created),"%s",follow.created_at);snprintf(started,sizeof(started),"%s",follow.started_at);snprintf(avatar,sizeof(avatar),"%s",follow.avatar);}
        if(!(g_in.held&(g_btn_ok|g_btn_back|SCE_CTRL_START))&&!g_in.touch_down)armed=1;
        if(armed&&(g_in.pressed&(g_btn_back|SCE_CTRL_START)))break;
        int choose=armed&&(g_in.pressed&g_btn_ok);
        if(g_in.pressed&SCE_CTRL_UP)selected=(selected+3)%4;
        if(g_in.pressed&SCE_CTRL_DOWN)selected=(selected+1)%4;
        for(int i=0;i<4;i++)if(armed&&ui_touch_hit(&g_in,bx,by+i*rh,bw,rh-3)){selected=i;choose=1;break;}
        if(choose){
            if(selected==0&&(s->live||saved_has(s->login))&&!follow_busy){
                if(loading){task_cancel(&task);task_wait(&task);}
                play_channel(s->login,s->display,s->title,s->viewers);return;
            }
            if(selected==3)break;
            if(selected==2){recordings_screen(s);armed=0;}
            if(selected==1){
                int rc=saved_has(s->login)?saved_toggle(s->login,s->display,avatar[0]?avatar:s->avatar):saved_ensure(s->login,s->display,avatar[0]?avatar:s->avatar);
                follow_status=rc<0?TW_FOLLOW_LOCAL_ERROR:rc?TW_FOLLOW_LOCAL_SAVED:TW_FOLLOW_LOCAL_REMOVED;armed=0;
            }
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        frame_begin();draw_home();
        int ox,oy;draw_modal_box(w,h,&ox,&oy);
        ui_text(x+24,y+36,C_WHITE,1.15f,profile_label(0));
        draw_avatar(avatar[0]?avatar:s->avatar,s->display,x+24,y+52,76);
        ui_text_fit(x+116,y+77,w-142,C_WHITE,1.35f,s->display[0]?s->display:s->login);
        snprintf(line,sizeof(line),"@%s",s->login);
        ui_text_fit(x+116,y+103,w-142,C_GREY,.9f,line);
        if(s->live){char viewers[32];ui_fmt_viewers(s->viewers,viewers,sizeof(viewers));snprintf(line,sizeof(line),"%s | %s | %s",vt_tr(VT_I_LIVE),viewers,s->game);}
        else snprintf(line,sizeof(line),"%s",vt_tr(VT_I_OFFLINE));
        ui_text_fit(x+116,y+127,w-142,s->live?C_RED:C_GREY,.82f,line);
        ui_text_fit(x+24,y+157,w-48,C_WHITE,.88f,s->title);
        if(loading)ui_text_fit(x+24,y+187,w-48,C_GREY,.85f,vt_tr(VT_I_LOADING));
        else if(job.bio[0])wrap_text(x+24,y+187,w-48,C_GREY,.82f,job.bio,3);
        if(followers>=0||created[0]||started[0]){snprintf(line,sizeof(line),"%s %d | %s %.10s | %s %.16s UTC",vt_lang_current()==2?"Seguidores":"Followers",followers,vt_lang_current()==2?"Desde":"Since",created,vt_lang_current()==2?"Live desde":"Live since",started);ui_text_fit(x+24,y+251,w-48,C_GREY,.75f,line);}
        ui_text_fit(x+24,y+270,w-48,C_GREY,.75f,twitch_follow_notice(follow_state==1?TW_FOLLOW_ALREADY:TW_FOLLOW_LOCAL_SAVED));
        const char *detail=follow_status==TW_FOLLOW_LOCAL_SAVED?twitch_follow_notice_detail(follow_status):
            (follow_status==TW_FOLLOW_LOCAL_ERROR||follow_status==TW_FOLLOW_LOCAL_REMOVED)?twitch_follow_notice(follow_status):
            saved_has(s->login)?twitch_follow_label(0,1):follow_busy?vt_tr(VT_I_LOADING):follow_state==1?twitch_follow_notice_detail(TW_FOLLOW_ALREADY):"";
        ui_text_fit(x+24,y+294,w-48,C_GREY,.8f,detail);
        const char *labels[]={vt_tr(VT_I_PLAY),saved_label(saved_has(s->login)),vt_lang_current()==2?"Vídeos e clipes":"Videos and clips",vt_tr(VT_I_BACK)};
        for(int i=0;i<4;i++){
            int yy=by+i*rh;ui_round_rect(bx,yy,bw,rh-3,5,selected==i?CLASSIC_PURPLE:CLASSIC_TILE);
            ui_text_fit(bx+14,yy+26,bw-28,i==0&&!s->live&&!saved_has(s->login)?C_DIM:C_WHITE,.93f,labels[i]);
        }
        frame_end();
    }
    if(loading){task_cancel(&task);task_wait(&task);}
    if(follow_busy){task_cancel(&follow_task);while(!task_done(&follow_task))sceKernelDelayThread(10000);}
    tab_load(TAB_FOLLOWED);
}
typedef struct {twitch_stream_t s;} own_job_t;
static int own_refresh(void *arg,volatile int *abort_flag){own_job_t *j=arg;twitch_refresh_saved(&j->s,1,abort_flag);return 0;}
typedef struct {char login[32],oauth[128];twitch_about_t about;} about_job_t;
static int about_run(void *arg,volatile int *abort_flag){about_job_t *j=arg;return twitch_fetch_about(j->login,j->oauth,&j->about,abort_flag);}
/* Two cached catalogs, one network worker; navigation never leaves the profile. */
static const char *profile_sub_badge(void){
    static const char *const labels[VT_LANG_COUNT]={
        "Subscribers only","Solo suscriptores","Exclusivo para inscritos","Réservé aux abonnés",
        "Solo abbonati","Nur Abonnenten","Только подписчики","Khusus pelanggan",
        "Subscriber lang","Subscriber lamang","Alleen abonnees","Tylko subskrybenci",
        "Yalnızca aboneler","Лише підписники","Endast prenumeranter","サブスク限定",
        "구독자 전용","仅限订阅者","僅限訂閱者"};
    int l=vt_lang_current();return labels[l>=0&&l<VT_LANG_COUNT?l:0];
}
static void profile_media_card(const twitch_media_t *v,int x,int y,int focused){
    const int w=222,h=148;
    vita2d_draw_rectangle(x,y,w,h,C_PANEL);
    vita2d_texture *tex=v->thumb[0]?img_get(v->thumb):NULL;
    if(tex)ui_image(tex,x+3,y+3,216,112);
    else{vita2d_draw_rectangle(x+3,y+3,216,112,C_BG);ui_text_center(x+111,y+63,C_GREY,1.f,"Twitch");}
    if(v->subscriber_only){
        vita2d_draw_rectangle(x+3,y+3,216,112,C_SCRIM);
        ui_round_rect(x+104,y+35,14,12,4,C_WHITE);
        vita2d_draw_rectangle(x+107,y+38,8,8,C_BG);
        ui_round_rect(x+101,y+43,20,14,3,C_WHITE);
        ui_round_rect(x+6,y+61,w-12,28,4,C_BG);
        ui_text_fit(x+10,y+80,w-20,C_WHITE,.87f,profile_sub_badge());
    }
    char duration[32];snprintf(duration,sizeof(duration),"%02d:%02d:%02d",v->seconds/3600,v->seconds/60%60,v->seconds%60);
    ui_round_rect(x+151,y+93,66,21,3,C_SCRIM);ui_text_fit(x+155,y+108,59,C_WHITE,.7f,duration);
    ui_text_fit(x+6,y+133,w-12,C_WHITE,.84f,v->title);
    if(focused)ui_frame(x,y,w,h,3,C_PURPLE);
}
/* Keep loaded pages and playback indices stable. Bound storage for the Vita. */
#define PROFILE_MEDIA_MAX 510
typedef struct {int n,source;char next[VT_MEDIA_CURSOR],err[192];twitch_media_t out[PROFILE_MEDIA_MAX];} profile_catalog_t;
typedef struct {char login[32],oauth[128],after[VT_MEDIA_CURSOR],next[VT_MEDIA_CURSOR],err[192];int kind,n,source,max;twitch_media_t out[VT_MEDIA_MAX];} profile_page_job_t;
static int profile_page_run(void *arg,volatile int *abort_flag){
    profile_page_job_t *j=arg;
    j->n=twitch_fetch_media_page(j->login,j->oauth,j->kind,j->out,j->max,j->after,&j->source,j->next,sizeof(j->next),j->err,sizeof(j->err),abort_flag);
    return j->n;
}
static const char *profile_more_label(void){
    static const char *const labels[VT_LANG_COUNT]={"Show more","Mostrar más","Mostrar mais","Afficher plus","Mostra altro","Mehr anzeigen","Показать ещё","Tampilkan lainnya","Magpakita pa","Ipakita pa","Meer tonen","Pokaż więcej","Daha fazla göster","Показати ще","Visa fler","もっと見る","더 보기","显示更多","顯示更多"};
    int l=vt_lang_current();return labels[l>=0&&l<VT_LANG_COUNT?l:0];
}
static int profile_has_more(const profile_catalog_t *j,int ci,int page){
    return ci<2&&j->next[0]&&j->n<PROFILE_MEDIA_MAX&&page+8>=j->n;
}
typedef struct {const twitch_stream_t *s;const about_job_t *details;profile_catalog_t *catalogs;int *state,*cursor;int own,sel,grid_focus,session_focus,session_yes;} profile_backdrop_t;
static profile_backdrop_t profile_bg;
static void draw_profile_view(void){
    const twitch_stream_t *s=profile_bg.s;const about_job_t *details=profile_bg.details;
    profile_catalog_t *catalogs=profile_bg.catalogs;int *state=profile_bg.state,*cursor=profile_bg.cursor;
    int own=profile_bg.own,sel=profile_bg.sel,grid_focus=profile_bg.grid_focus,session_focus=profile_bg.session_focus,session_yes=profile_bg.session_yes;
    int count=own?7:4,step=924/count,ci=sel==1?0:sel==2?1:own&&sel==3?2:own&&sel==4?3:-1;
vita2d_draw_rectangle(0,0,960,544,CLASSIC_BG);
        draw_avatar(s->avatar,s->display,25,20,84);
        ui_text_fit(125,47,810,C_WHITE,1.35f,s->display[0]?s->display:s->login);
        char at[64];snprintf(at,sizeof(at),"@%s",s->login);ui_text_fit(125,75,810,C_GREY,.95f,at);
        ui_text_fit(125,103,810,s->live?C_RED:C_GREY,.9f,s->live?vt_tr(VT_I_LIVE):vt_tr(VT_I_OFFLINE));
        const char *pt[]={"Ao vivo","Vídeos","Clipes","Salvos","Continuar","Sobre","Sessão"};
        const char *es[]={"En vivo","Vídeos","Clips","Guardados","Continuar","Acerca de","Sesión"};
        const char *en[]={"Live","Videos","Clips","Saved","Continue","About","Session"};
        const char **labels=vt_lang_current()==2?pt:vt_lang_current()==1?es:en;
        for(int i=0;i<count;i++){int x=18+i*step;ui_round_rect(x,138,step-4,44,5,sel==i?C_PURPLE:C_PANEL);ui_text_fit(x+8,167,step-20,C_WHITE,.9f,labels[!own&&i==3?5:i]);}
        if(sel==(own?5:3)){
            int ptbr=vt_lang_current()==2;char line[192];
            ui_text_fit(30,211,900,C_WHITE,1.1f,ptbr?"Sobre o canal":"About the channel");
            if(details->about.followers>=0)snprintf(line,sizeof(line),"%s: %d",ptbr?"Seguidores":"Followers",details->about.followers);
            else snprintf(line,sizeof(line),"%s: —",ptbr?"Seguidores":"Followers");
            ui_text_fit(30,242,440,C_GREY,.94f,line);
            snprintf(line,sizeof(line),"%s: %.10s",ptbr?"Conta criada":"Joined",details->about.created[0]?details->about.created:"—");
            ui_text_fit(490,242,440,C_GREY,.94f,line);
            snprintf(line,sizeof(line),"twitch.tv/%s",s->login);ui_text_fit(30,269,900,C_WHITE,.9f,line);
            ui_text_fit(30,302,900,C_WHITE,1.0f,ptbr?"Biografia":"Biography");
            wrap_text(30,327,900,C_GREY,.87f,details->about.bio[0]?details->about.bio:ptbr?"Não disponível.":"Unavailable.",3);
            ui_text_fit(30,411,900,C_WHITE,1.0f,ptbr?"Links e redes sociais":"Links and social media");
            wrap_text(30,439,900,C_GREY,.8f,details->about.links[0]?details->about.links:ptbr?"Links não disponibilizados pela Twitch.":"Links unavailable from Twitch.",3);
        }else if(own&&sel==6){
            ui_text_center(480,270,C_WHITE,1.25f,session_text(0));
            for(int i=0;i<2;i++){
                int xx=180+i*320,selected=(i==0)==session_yes;
                ui_round_rect(xx,320,280,58,6,selected?CLASSIC_PURPLE:C_PANEL);
                if(selected&&session_focus)ui_frame(xx,320,280,58,2,C_WHITE);
                ui_text_center(xx+140,358,C_WHITE,1.05f,session_text(i+1));
            }
        }else if(sel==0){ui_text_fit(30,235,900,C_WHITE,1.1f,s->live?s->title:vt_tr(VT_I_OFFLINE));wrap_text(30,285,900,C_GREY,.9f,details->about.bio,4);}
        else if(ci>=0){
            profile_catalog_t *j=&catalogs[ci];
            if(state[ci]==0||state[ci]==1){ui_spinner(480,315,18,C_PURPLE);ui_text_center(480,352,C_WHITE,1.f,vt_tr(VT_I_LOADING));}
            else if(state[ci]==3){ui_text_fit(30,475,900,C_GREY,.88f,j->err);ui_text_center(480,352,C_WHITE,.95f,vt_lang_current()==2?"X: tentar novamente":"Confirm: retry");}
            else if(j->n==0)ui_text_center(480,352,C_GREY,1.f,vt_lang_current()==2?(ci==1?"Nenhum clipe":"Nenhum vídeo"):(ci==1?"No clips":"No videos"));
            else{
                int page=cursor[ci]/8*8;
                for(int k=0;k<8&&page+k<j->n;k++)profile_media_card(&j->out[page+k],18+(k%4)*232,193+(k/4)*152,grid_focus==1&&cursor[ci]==page+k);
                char pages[48];snprintf(pages,sizeof(pages),"%d / %d",page/8+1,(j->n+7)/8);ui_text_fit(810,520,125,C_GREY,.82f,pages);
                if(profile_has_more(j,ci,page)||state[ci]==4){
                    ui_round_rect(600,498,200,36,5,grid_focus==2?C_PURPLE:C_PANEL);
                    if(state[ci]==4)ui_spinner(700,516,9,C_WHITE);
                    else ui_text_fit(610,521,180,C_WHITE,.9f,profile_more_label());
                    if(grid_focus==2)ui_frame(600,498,200,36,2,C_WHITE);
                }
                if(j->err[0]){ui_round_rect(30,460,900,30,4,C_BG);ui_text_fit(40,481,880,C_WHITE,.8f,j->err);}
            }
        }else ui_text_fit(30,235,900,C_GREY,1.05f,vt_lang_current()==2?"Confirme para abrir esta seção":"Confirm to open this section");
        if(own&&sel==6){
            ui_hint_bounded(25,520,440,g_ok_name,vt_msg("Aceptar"));
            ui_hint_bounded(500,520,420,g_back_name,vt_tr(VT_I_BACK));
        }else ui_text_fit(25,520,ci>=0?560:910,C_WHITE,.82f,ci>=0?(vt_lang_current()==2?"Setas: navegar | L/R: páginas | X: abrir | Triângulo: atualizar | O: voltar":"D-pad: navigate | L/R: pages | Confirm: open | Triangle: refresh | Back"):(vt_lang_current()==2?"Esquerda/direita: abas | Confirmar | Voltar":"Left/right: tabs | Confirm | Back"));

}
/* One cancellable anonymous manifest probe at a time, only for displayed
 * VOD cards. A generic 403, TLS failure or timeout never marks a video. */
typedef struct {task_t task;vod_job_t job;int active,ci,index;} profile_probe_t;
static void profile_probe_stop(profile_probe_t *p){
    if(p->active){task_cancel(&p->task);while(!task_done(&p->task))sceKernelDelayThread(10000);task_wait(&p->task);p->active=0;}
}
static void channel_full_profile(const twitch_stream_t *channel,int own,int return_to_live){
    twitch_stream_t s=*channel;
    about_job_t details={0};details.about.followers=-1;snprintf(details.login,sizeof(details.login),"%s",s.login);
    snprintf(details.oauth,sizeof(details.oauth),"%s",g_logged?g_auth.access:"");
    task_t bt={0};run_with_spinner(&bt,about_run,&details,vt_tr(VT_I_LOADING));
    if(!details.about.bio[0]){profile_job_t bio={0};snprintf(bio.login,sizeof(bio.login),"%s",s.login);task_t t={0};
        run_with_spinner(&t,profile_run,&bio,vt_tr(VT_I_LOADING));snprintf(details.about.bio,sizeof(details.about.bio),"%s",bio.bio);}
    static profile_catalog_t catalogs[4];memset(catalogs,0,sizeof(catalogs));media_load();
    static profile_page_job_t page_job;
    task_t mt={0};int active=-1,state[4]={0,0,0,0},cursor[4]={0,0,0,0};
    static profile_probe_t probe;memset(&probe,0,sizeof(probe));
    unsigned char checked[4][PROFILE_MEDIA_MAX]={{0}};
    int sel=0,armed=0,grid_focus=0,session_focus=0,session_yes=0;const int count=own?7:4,step=924/count;
    for(;;){
        ui_input_poll(&g_in);unsigned b=g_in.pressed;
        if(!(g_in.held&(g_btn_ok|g_btn_back))&&!g_in.touch_down)armed=1;
        if(active>=0&&task_done(&mt)){
            task_wait(&mt);profile_catalog_t *cat=&catalogs[active];int append=state[active]==4;
            if(page_job.n>=0){
                int old=cat->n;
                for(int i=0;i<page_job.n&&cat->n<PROFILE_MEDIA_MAX;i++){
                    int dup=0;for(int k=0;k<cat->n;k++)if(!strcmp(cat->out[k].id,page_job.out[i].id)){dup=1;break;}
                    if(!dup)cat->out[cat->n++]=page_job.out[i];
                }
                cat->source=page_job.source;
                snprintf(cat->next,sizeof(cat->next),"%s",strcmp(page_job.after,page_job.next)?page_job.next:"");
                cat->err[0]=0;state[active]=2;
                if(append&&cat->n>old){cursor[active]=old;if(sel==active+1)grid_focus=1;}
                else if(append&&sel==active+1)grid_focus=1;
            }else{
                snprintf(cat->err,sizeof(cat->err),"%s",page_job.err[0]?page_job.err:vt_msg("No se pudo cargar"));
                state[active]=append?2:3;
            }
            active=-1;
        }
        if(probe.active&&task_done(&probe.task)){
            task_wait(&probe.task);probe.active=0;
            profile_catalog_t *v=&catalogs[probe.ci];int k=probe.index;
            if(k>=0&&k<v->n&&!strcmp(v->out[k].id,probe.job.id)){
                if(!strcmp(probe.job.err,twitch_vod_access_message("SUBSCRIPTION"))){v->out[k].subscriber_only=1;int saved=media_find(v->out[k].id);if(saved>=0)media_saved[saved].v.subscriber_only=1;}
                checked[probe.ci][k]=1;
                LOGI("profile: VOD restriction probe %s marked=%d variants=%d",v->out[k].id,v->out[k].subscriber_only,probe.job.n);
            }
        }
        if((own&&!g_logged)||(armed&&(b&g_btn_back))){
            if((grid_focus||session_focus)&&(!own||g_logged)){grid_focus=0;session_focus=0;session_yes=0;armed=0;continue;}
            profile_probe_stop(&probe);
            if(active>=0){task_cancel(&mt);while(!task_done(&mt))sceKernelDelayThread(10000);task_wait(&mt);}return;
        }
        int ci=sel==1?0:sel==2?1:own&&sel==3?2:own&&sel==4?3:-1;
        if(!grid_focus&&!session_focus){
            if(b&SCE_CTRL_LEFT)sel=(sel+count-1)%count;
            if(b&SCE_CTRL_RIGHT)sel=(sel+1)%count;
        }
        int choose=armed&&(b&g_btn_ok),tab_touch=0;
        for(int i=0;i<count;i++)if(armed&&ui_touch_hit(&g_in,18+i*step,138,step-4,44)){sel=i;grid_focus=0;session_focus=0;session_yes=0;tab_touch=1;choose=0;break;}
        ci=sel==1?0:sel==2?1:own&&sel==3?2:own&&sel==4?3:-1;
        if(probe.active&&(probe.ci!=ci||(ci>=0&&(b&SCE_CTRL_TRIANGLE))))profile_probe_stop(&probe);
        if(ci>=0){
            profile_catalog_t *j=&catalogs[ci];
            if(state[ci]==3&&(choose||(b&SCE_CTRL_TRIANGLE)))state[ci]=0;
            if(state[ci]==2&&(b&SCE_CTRL_TRIANGLE)){state[ci]=0;grid_focus=0;cursor[ci]=0;memset(checked[ci],0,sizeof(checked[ci]));}
            if(ci>=2&&state[ci]==0){
                profile_probe_stop(&probe);memset(checked[ci],0,sizeof(checked[ci]));
                memset(j,0,sizeof(*j));
                for(int i=0;i<media_saved_n&&j->n<VT_MEDIA_MAX;i++)if(ci==2?media_saved[i].saved:media_saved[i].pos>0)j->out[j->n++]=media_saved[i].v;
                state[ci]=2;
            }
            if(ci<2&&state[ci]==0&&active<0){
                memset(j,0,sizeof(*j));memset(&page_job,0,sizeof(page_job));
                page_job.n=-1;page_job.kind=ci?4:0;page_job.max=VT_MEDIA_MAX;
                snprintf(page_job.login,sizeof(page_job.login),"%s",s.login);snprintf(page_job.oauth,sizeof(page_job.oauth),"%s",g_logged?g_auth.access:"");
                if(task_start(&mt,profile_page_run,&page_job)==0){active=ci;state[ci]=1;}else{state[ci]=3;snprintf(j->err,sizeof(j->err),"%s",vt_tr(VT_I_LOADING));}
            }
            if(state[ci]==2&&j->n>0){
                int *pos=&cursor[ci];if(*pos>=j->n)*pos=0;
                if(!grid_focus&&((b&SCE_CTRL_DOWN)||choose)){grid_focus=1;choose=0;}
                else if(grid_focus==2){
                    if(b&SCE_CTRL_UP){grid_focus=1;choose=0;}
                }else if(grid_focus==1){
                    if(b&SCE_CTRL_LEFT&&*pos>0)(*pos)--;
                    if(b&SCE_CTRL_RIGHT&&*pos+1<j->n)(*pos)++;
                    if(b&SCE_CTRL_DOWN){if(*pos+4<j->n)*pos+=4;else if(profile_has_more(j,ci,*pos/8*8))grid_focus=2;}

                    if(b&SCE_CTRL_UP){if(*pos%8>=4)*pos-=4;else grid_focus=0;}
                }
                if(b&SCE_CTRL_LTRIGGER&&*pos>=8){*pos=(*pos/8-1)*8;grid_focus=1;choose=0;}
                if(b&SCE_CTRL_RTRIGGER&&(*pos/8+1)*8<j->n){*pos=(*pos/8+1)*8;grid_focus=1;choose=0;}
                int page=*pos/8*8;
                if(probe.active&&(probe.index<page||probe.index>=page+8))profile_probe_stop(&probe);
                for(int k=0;k<8&&page+k<j->n;k++)if(armed&&ui_touch_hit(&g_in,18+(k%4)*232,193+(k/4)*152,222,148)){*pos=page+k;grid_focus=1;choose=1;break;}
                if(grid_focus==1&&(b&SCE_CTRL_SQUARE)&&strcmp(j->out[*pos].type,"clip")){media_remember(&j->out[*pos],-1,1);if(ci==2)state[ci]=0;choose=0;}
                if(armed&&profile_has_more(j,ci,page)&&ui_touch_hit(&g_in,600,498,200,36)){grid_focus=2;choose=1;}
                if(choose&&grid_focus==2){
                    armed=0;choose=0;profile_probe_stop(&probe);
                    if(active<0&&profile_has_more(j,ci,page)){
                        memset(&page_job,0,sizeof(page_job));page_job.n=-1;page_job.kind=ci?4:0;page_job.source=j->source;
                        page_job.max=PROFILE_MEDIA_MAX-j->n;if(page_job.max>VT_MEDIA_MAX)page_job.max=VT_MEDIA_MAX;
                        snprintf(page_job.login,sizeof(page_job.login),"%s",s.login);snprintf(page_job.oauth,sizeof(page_job.oauth),"%s",g_logged?g_auth.access:"");
                        snprintf(page_job.after,sizeof(page_job.after),"%s",j->next);j->err[0]=0;
                        if(task_start(&mt,profile_page_run,&page_job)==0){active=ci;state[ci]=4;}
                        else snprintf(j->err,sizeof(j->err),"%s",vt_msg("No se pudo cargar"));
                    }
                }
                if(choose&&grid_focus==1){
                    armed=0;profile_probe_stop(&probe);
                    /* Finish another catalog's worker before entering the player. */
                    if(active>=0){task_cancel(&mt);while(!task_done(&mt))sceKernelDelayThread(10000);task_wait(&mt);state[active]=state[active]==4?2:0;active=-1;}
                    profile_bg=(profile_backdrop_t){&s,&details,catalogs,state,cursor,own,sel,grid_focus,session_focus,session_yes};
                    g_media_backdrop=draw_profile_view;
                    for(;;){
                        int rc=recorded_play(&j->out[*pos],*pos>0,*pos+1<j->n);
                        if(rc==PLAYER_PREVIOUS_MEDIA&&*pos>0){(*pos)--;continue;}
                        if(rc==PLAYER_NEXT_MEDIA&&*pos+1<j->n){(*pos)++;continue;}
                        break;
                    }
                    g_media_backdrop=NULL;
                    if(ci>=2)state[ci]=0;
                    ui_input_poll(&g_in);
                }else if(ci!=1&&state[ci]==2&&!probe.active&&active<0&&net_connected()){
                    for(int k=page;k<page+8&&k<j->n;k++){
                        if(checked[ci][k]||j->out[k].subscriber_only||!strcmp(j->out[k].type,"clip"))continue;
                        memset(&probe.job,0,sizeof(probe.job));probe.job.n=-1;
                        snprintf(probe.job.id,sizeof(probe.job.id),"%s",j->out[k].id);
                        probe.ci=ci;probe.index=k;
                        if(task_start(&probe.task,vod_job_run,&probe.job)==0)probe.active=1;else checked[ci][k]=1;
                        break;
                    }
                }
            }
        }else if(own&&sel==6){
            /* Selecting the tab only displays the question. The default is No.
             * A touch on either button works without D-pad focus. */
            int confirm=0;
            if(!session_focus&&(choose||(b&SCE_CTRL_DOWN))){session_focus=1;session_yes=0;choose=0;}
            else if(session_focus){
                if(b&SCE_CTRL_UP){session_focus=0;session_yes=0;choose=0;}
                if(b&SCE_CTRL_LEFT)session_yes=1;
                if(b&SCE_CTRL_RIGHT)session_yes=0;
                if(choose)confirm=1;
            }
            if(armed&&ui_touch_hit(&g_in,180,320,280,58)){session_yes=1;confirm=1;}
            if(armed&&ui_touch_hit(&g_in,500,320,280,58)){session_yes=0;confirm=1;}
            if(confirm){
                armed=0;
                if(session_yes){
                    if(active>=0){task_cancel(&mt);while(!task_done(&mt))sceKernelDelayThread(10000);task_wait(&mt);}
                    profile_probe_stop(&probe);account_logout();return;
                }
                session_focus=0;session_yes=0;
            }
        }else if(choose||tab_touch){
            armed=0;
            if(sel==0){profile_probe_stop(&probe);if(active>=0){task_cancel(&mt);while(!task_done(&mt))sceKernelDelayThread(10000);task_wait(&mt);active=-1;}if(return_to_live)return;if(s.live){play_channel(s.login,s.display,s.title,s.viewers);return;}else toast(vt_tr(VT_I_OFFLINE));}
        }
        profile_bg=(profile_backdrop_t){&s,&details,catalogs,state,cursor,own,sel,grid_focus,session_focus,session_yes};
        frame_begin();draw_profile_view();
        frame_end();
    }
}
static void own_profile(void){
    if(!g_logged)return;
    own_job_t own={0};snprintf(own.s.login,sizeof(own.s.login),"%s",g_auth.login);snprintf(own.s.display,sizeof(own.s.display),"%s",g_auth.display);snprintf(own.s.avatar,sizeof(own.s.avatar),"%s",g_auth.avatar);
    task_t rt={0};run_with_spinner(&rt,own_refresh,&own,vt_tr(VT_I_LOADING));
    channel_full_profile(&own.s,1,0);
}
static void profile_selected(void)
{
    tab_t *tb=&g_tabs[g_tab];
    if(g_side_focus>=0||tb->state!=LS_READY||tb->sel<0||tb->sel>=tb->count||home_pos(tb)<0)return;
    twitch_stream_t s=tb->items[tb->sel];profile_screen(&s);
}

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
    if(s.live)play_channel(s.login,s.display,s.title,s.viewers);
    else profile_screen(&s);
}

static void clear_search(void){tab_t *tb=&g_tabs[TAB_SEARCH];g_search[0]=0;tb->count=0;tb->sel=-1;tb->scroll=0;tb->state=LS_EMPTY;if(tb->busy)task_cancel(&tb->task);settings_save();}
static void home_switch(int t)
{
    if(g_tab==TAB_SEARCH&&t!=TAB_SEARCH)clear_search();
    g_tab=t;g_side_focus=-1;g_touch_manual_scroll=0;
    if(t!=TAB_EXPLORE)g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;
    tab_t *tb=&g_tabs[t];tb->scroll=0;tb->sel=t==TAB_SEARCH?-1:home_index(tb,0);
    if(t==TAB_EXPLORE){char expected[256];explore_query(expected,sizeof(expected));if(strcmp(tb->job.query,expected)){tb->state=LS_EMPTY;tb->count=0;}}
    if(tb->state==LS_EMPTY||tb->state==LS_ERROR)tab_load(t);
}

/* Category browser: Helix box art with local fallback. Inspired by LiveVita (Cecil2121). */
typedef struct {char oauth[64];twitch_category_t items[60];int n;} category_job_t;
static category_job_t category_job;
#define CATEGORY_FAV_FILE DATA_DIR "/categories-favorites.txt"
static char category_favs[60][32];static int category_fav_n;
static int category_is_fav(const char *id){for(int i=0;id&&*id&&i<category_fav_n;i++)if(!strcmp(id,category_favs[i]))return 1;return 0;}
static void category_favs_load(void){category_fav_n=0;FILE *fp=fopen(CATEGORY_FAV_FILE,"r");if(!fp)return;char line[64];while(category_fav_n<60&&fgets(line,sizeof(line),fp)){line[strcspn(line,"\r\n")]=0;if(*line&&strlen(line)<32&&strlen(line)==strspn(line,"0123456789")&&!category_is_fav(line))snprintf(category_favs[category_fav_n++],32,"%s",line);}fclose(fp);}
static int category_fav_toggle(const char *id){if(!id||!*id)return -1;int at=-1;for(int i=0;i<category_fav_n;i++)if(!strcmp(id,category_favs[i]))at=i;if(at>=0){memmove(category_favs+at,category_favs+at+1,(size_t)(category_fav_n-at-1)*sizeof(category_favs[0]));category_fav_n--;}else if(category_fav_n<60)snprintf(category_favs[category_fav_n++],32,"%s",id);else return -1;FILE *fp=fopen(CATEGORY_FAV_FILE ".tmp","w");if(!fp){category_favs_load();return -1;}int rc=0;for(int i=0;i<category_fav_n;i++)if(fprintf(fp,"%s\n",category_favs[i])<0)rc=-1;if(fclose(fp)!=0)rc=-1;if(!rc&&sceIoRename(CATEGORY_FAV_FILE ".tmp",CATEGORY_FAV_FILE)<0)rc=-1;if(rc<0){sceIoRemove(CATEGORY_FAV_FILE ".tmp");category_favs_load();}return rc;}
static int categories_run(void *arg,volatile int *abort_flag){
    category_job_t *j=arg;j->n=twitch_fetch_categories(j->oauth,j->items,60,abort_flag);return j->n;
}
static const char *category_note(int remote){
    int l=vt_lang_current();
    if(remote)return l==2?"Categorias populares da Twitch":l==1?"Categorías populares de Twitch":"Top Twitch categories";
    return l==2?"Catálogo indisponível: categorias das lives carregadas":l==1?"Catálogo no disponible: categorías de directos cargados":"Directory unavailable: categories from loaded streams";
}
static void games_screen(void)
{
    tab_t *tb=&g_tabs[TAB_EXPLORE];
    memset(&category_job,0,sizeof(category_job));category_job.n=-1;
    snprintf(category_job.oauth,sizeof(category_job.oauth),"%s",g_logged?g_auth.access:"");
    task_t task={0};
    if(run_with_spinner(&task,categories_run,&category_job,vt_tr(VT_I_LOADING))<0){
        if(task.busy||!task_done(&task)){task_cancel(&task);while(!task_done(&task))sceKernelDelayThread(10000);}return;
    }
    int remote=category_job.n>0,n=remote?category_job.n:0,sel=0;
    if(!remote){
        for(int i=0;i<tb->count&&n<60;i++){
            if(!tb->items[i].game[0])continue;
            int seen=0;for(int j=0;j<n;j++)if(!strcmp(category_job.items[j].name,tb->items[i].game)){seen=1;break;}
            if(!seen){twitch_category_t *g=&category_job.items[n++];memset(g,0,sizeof(*g));snprintf(g->name,sizeof(g->name),"%s",tb->items[i].game);twitch_category_cover(g->name,g->art,sizeof(g->art));}
        }
    }
    if(!n){toast(category_note(0));return;}
    category_favs_load();
    for(int i=0,k=0;i<n;i++)if(category_is_fav(category_job.items[i].id)){twitch_category_t g=category_job.items[i];memmove(category_job.items+k+1,category_job.items+k,(size_t)(i-k)*sizeof(g));category_job.items[k++]=g;}
    const int x=76,y=25,w=808,h=494,cols=5,rows=2,cw=150,ch=190;
    int armed=0;
    for(;;){
        ui_input_poll(&g_in);
        if(!(g_in.held&g_btn_ok)&&!g_in.touch_down)armed=1;
        if(g_in.pressed&(g_btn_back|SCE_CTRL_SELECT))return;
        if(g_in.pressed&SCE_CTRL_LEFT)sel=(sel+n-1)%n;
        if(g_in.pressed&SCE_CTRL_RIGHT)sel=(sel+1)%n;
        if(g_in.pressed&SCE_CTRL_UP)sel=sel>=cols?sel-cols:sel;
        if(g_in.pressed&SCE_CTRL_DOWN)sel=sel+cols<n?sel+cols:sel;
        if(g_in.pressed&SCE_CTRL_SQUARE){if(category_fav_toggle(category_job.items[sel].id)<0)toast(vt_lang_current()==2?"Não foi possível salvar":"Could not save");}
        if(g_in.pressed&SCE_CTRL_LTRIGGER)sel=sel>=cols*rows?sel-cols*rows:0;
        if(g_in.pressed&SCE_CTRL_RTRIGGER)sel=sel+cols*rows<n?sel+cols*rows:n-1;
        if(g_in.touch_dragged&&g_in.touch_dy>18&&sel>=cols)sel-=cols;
        if(g_in.touch_dragged&&g_in.touch_dy< -18&&sel+cols<n)sel+=cols;
        int first=(sel/(cols*rows))*(cols*rows),choose=armed&&!!(g_in.pressed&g_btn_ok);
        for(int j=0;j<cols*rows&&first+j<n;j++){
            int xx=x+26+(j%cols)*cw,yy=y+65+(j/cols)*ch;
            if(armed&&ui_touch_hit(&g_in,xx,yy,cw-8,ch-6)){sel=first+j;choose=1;break;}
        }
        if(choose){
            twitch_category_t *g=&category_job.items[sel];
            snprintf(g_game_filter,sizeof(g_game_filter),"%s",g->name);
            snprintf(g_game_id,sizeof(g_game_id),"%s",g->id);
            snprintf(g_game_slug,sizeof(g_game_slug),"%s",g->slug);
            if(g_tv_layout){g_tv_home=0;g_tv_nav=2;g_tv_nav_focus=0;}
            home_switch(TAB_EXPLORE);return;
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        frame_begin();draw_home();int bx,by;draw_modal_box(w,h,&bx,&by);
        ui_text(x+24,y+30,C_WHITE,1.15f,games_label());
        ui_text_fit(x+24,y+51,w-48,C_GREY,.72f,category_note(remote));
        for(int j=0;j<cols*rows&&first+j<n;j++){
            int k=first+j,xx=x+26+(j%cols)*cw,yy=y+65+(j/cols)*ch;
            twitch_category_t *g=&category_job.items[k];
            vita2d_draw_rectangle(xx,yy,cw-8,ch-6,CLASSIC_TILE);
            vita2d_texture *art=g->art[0]?img_get(g->art):NULL;
            if(art)ui_image(art,xx+18,yy+3,106,142);
            else ui_text_center(xx+71,yy+82,C_GREY,.78f,games_label());
            ui_text_fit(xx+5,yy+165,cw-18,C_WHITE,.78f,g->name);
            if(category_is_fav(g->id)){vita2d_draw_rectangle(xx+3,yy+3,24,23,C_PURPLE);ui_text_center(xx+15,yy+21,C_WHITE,.85f,"*");}
            if(k==sel)ui_frame(xx,yy,cw-8,ch-6,3,C_WHITE);
        }
        for(int j=first+cols*rows;j<n&&j<first+cols*rows+cols;j++)if(category_job.items[j].art[0])img_prefetch(category_job.items[j].art);
        ui_hint(x+25,y+h-18,g_back_name,vt_tr(VT_I_BACK));
        ui_hint_bounded(x+180,y+h-18,360,"□",vt_lang_current()==2?"Favoritar / remover":"Favorite / remove");
        char page[64];snprintf(page,sizeof(page),"%d | L/R %d/%d",n,first/(cols*rows)+1,(n+cols*rows-1)/(cols*rows));
        ui_text_fit(x+w-200,y+h-18,176,C_GREY,.8f,page);
        frame_end();
    }
}
static void side_open(int i)
{
    switch(i){
    case 0:home_switch(TAB_SEARCH);break;
    case 1:home_switch(TAB_FOLLOWED);break;
    case 2:g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;home_switch(TAB_EXPLORE);break;
    case 3:games_screen();break;
    case 4:extras_menu();break;
    case PROFILE_FOCUS:account_screen();break;
    }
}

static void tv_activate(int i){
    g_tv_nav=i;g_tv_nav_focus=0;g_side_focus=-1;
    switch(i){
    case 0:g_tv_home=1;g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;home_switch(TAB_EXPLORE);if(g_tabs[TAB_FOLLOWED].state==LS_EMPTY)tab_load(TAB_FOLLOWED);break;
    case 1:g_tv_home=0;home_switch(TAB_FOLLOWED);break;
    case 2:g_tv_home=0;g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;home_switch(TAB_EXPLORE);break;
    case 3:g_tv_home=0;home_switch(TAB_SEARCH);break;
    case 4:if(g_tab==TAB_SEARCH)clear_search();account_screen();g_tv_nav_focus=1;break;
    case 5:if(g_tab==TAB_SEARCH)clear_search();extras_menu();g_tv_nav_focus=1;break;
    }
}
static void tv_input(void){
    unsigned p=g_in.pressed;tab_t *tb=&g_tabs[g_tab];
    if(p&SCE_CTRL_TRIANGLE&&(g_in.held&SCE_CTRL_LTRIGGER)&&g_tab==TAB_FOLLOWED){g_follow_offline=(g_follow_offline+1)%3;settings_save();following_rebuild();tab_load(g_tab);return;}
    if(p&SCE_CTRL_SELECT){extras_menu();return;}
    if(p&SCE_CTRL_START){if(!g_tv_nav_focus)profile_selected();return;}
    if(p&(SCE_CTRL_LTRIGGER|SCE_CTRL_RTRIGGER)){int next=(g_tv_nav+(p&SCE_CTRL_RTRIGGER?1:TV_NAV_COUNT-1))%TV_NAV_COUNT;tv_activate(next);return;}
    if(g_in.touch_tap){
        for(int i=0;i<TV_NAV_COUNT;i++)if(ui_touch_hit(&g_in,TV_NAV_X+i*TV_NAV_STEP,0,TV_NAV_STEP,54)){tv_activate(i);return;}
        /* Touch activates the visible hero directly, independent of nav focus.
         * Mirror draw_tv_home's selection fallback and exact hit geometry. */
        int hy=(!g_tv_home&&g_tab==TAB_SEARCH)?102:65,hh=(!g_tv_home&&g_tab==TAB_SEARCH)?157:165;
        if(ui_touch_hit(&g_in,397,hy+132,170,30)||ui_touch_hit(&g_in,20,hy,hh*16/9,hh)){
            int pos=home_pos(tb),n=home_count(tb);
            int idx=pos>=0?home_index(tb,pos):n?home_index(tb,0):-1;
            if(idx>=0){g_tv_nav_focus=0;g_side_focus=-1;tb->sel=idx;open_selected();}
            return;
        }
        if(g_tab==TAB_SEARCH&&ui_touch_hit(&g_in,20,60,920,35)){tb->sel=-1;g_tv_nav_focus=0;open_selected();return;}
        int grid=!g_tv_home&&(g_tab==TAB_FOLLOWED||(g_tab==TAB_EXPLORE&&!g_game_id[0]&&!g_game_filter[0]));
        for(int row=0;row<(g_tv_home?2:1);row++){
            int t=g_tv_home?(row?TAB_EXPLORE:TAB_FOLLOWED):g_tab;tab_t *r=&g_tabs[t];int n=home_count(r),pos=home_pos(r),slots=grid?8:4,first=pos>=0?pos/slots*slots:0;
            for(int k=0;k<slots&&first+k<n;k++)if(ui_touch_hit(&g_in,20+(k%4)*233,tv_row_top()+(row+k/4)*TV_ROW_STEP,TV_TILE_W,TV_TILE_HIT_H)){g_tab=t;g_side_focus=-1;g_tv_nav_focus=0;r->sel=home_index(r,first+k);open_selected();return;}
        }
        int cell=(SCREEN_W-24)/(6);
        if(ui_touch_hit(&g_in,12+cell*(5),FOOTER_Y,cell,SCREEN_H-FOOTER_Y)){
            g_tv_nav_focus=0;g_side_focus=-1;profile_selected();return;
        }
        if(ui_touch_hit(&g_in,12,FOOTER_Y,cell,SCREEN_H-FOOTER_Y)){
            g_tv_nav_focus=0;g_side_focus=-1;
            if(tb->need_login)tv_activate(4);else open_selected();return;
        }
        if(ui_touch_hit(&g_in,12+cell,FOOTER_Y,cell,SCREEN_H-FOOTER_Y)){img_clear();tab_load(g_tab);return;}
        if(ui_touch_hit(&g_in,12+2*cell,FOOTER_Y,cell,SCREEN_H-FOOTER_Y)){games_screen();return;}
        if(ui_touch_hit(&g_in,12+cell*(4),FOOTER_Y,cell,SCREEN_H-FOOTER_Y)){extras_menu();return;}
        return;
    }
    if(p&SCE_CTRL_TRIANGLE){if(g_tab==TAB_FOLLOWED&&(g_in.held&SCE_CTRL_LTRIGGER)){g_follow_offline=(g_follow_offline+1)%3;settings_save();following_rebuild();}img_clear();tab_load(g_tab);return;}
    if(p&SCE_CTRL_SQUARE){games_screen();return;}

    if(g_tv_nav_focus){
        if(p&SCE_CTRL_LEFT)g_tv_nav=(g_tv_nav+TV_NAV_COUNT-1)%TV_NAV_COUNT;
        if(p&SCE_CTRL_RIGHT)g_tv_nav=(g_tv_nav+1)%TV_NAV_COUNT;
        if(p&g_btn_ok){tv_activate(g_tv_nav);return;}
        if(p&SCE_CTRL_DOWN){g_tv_nav_focus=0;return;}return;
    }
    if(p&g_btn_back){if(g_tab==TAB_SEARCH)clear_search();g_tv_nav_focus=1;return;}
    int grid=!g_tv_home&&(g_tab==TAB_FOLLOWED||(g_tab==TAB_EXPLORE&&!g_game_id[0]&&!g_game_filter[0]));
    if(grid&&(p&(SCE_CTRL_UP|SCE_CTRL_DOWN))){
        int pos=home_pos(tb),n=home_count(tb);
        if((p&SCE_CTRL_DOWN)&&pos>=0&&pos+4<n){tb->sel=home_index(tb,pos+4);return;}
        if((p&SCE_CTRL_UP)&&pos>=0&&pos%8>=4){tb->sel=home_index(tb,pos-4);return;}
        if(p&SCE_CTRL_DOWN)return;
    }
    if(p&SCE_CTRL_UP){
        if(g_tv_home&&g_tab==TAB_EXPLORE){g_tab=TAB_FOLLOWED;return;}
        if(g_tab==TAB_SEARCH&&tb->sel>=0){tb->sel=-1;return;}
        g_tv_nav_focus=1;return;
    }
    if(p&SCE_CTRL_DOWN){
        if(g_tv_home&&g_tab==TAB_FOLLOWED){g_tab=TAB_EXPLORE;return;}
        if(g_tab==TAB_SEARCH&&tb->sel<0&&home_count(tb)>0)tb->sel=home_index(tb,0);
        return;
    }
    int n=home_count(tb),pos=home_pos(tb);
    if(n&&(p&(SCE_CTRL_LEFT|SCE_CTRL_RIGHT))){int k=pos<0?0:(pos+(p&SCE_CTRL_RIGHT?1:n-1))%n;tb->sel=home_index(tb,k);}
    if(p&g_btn_ok){if(tb->need_login)tv_activate(4);else open_selected();}
}

int main(void)
{
    init_system();
    settings_load();saved_load();
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
    if(g_tv_layout){g_tv_home=1;g_tv_nav=0;g_tab=TAB_EXPLORE;}
    tab_load(g_tab);
    if(g_tab!=TAB_FOLLOWED)tab_load(TAB_FOLLOWED);
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
        if(!g_tv_layout&&(p&SCE_CTRL_TRIANGLE)&&(g_in.held&SCE_CTRL_LTRIGGER)&&g_tab==TAB_FOLLOWED){g_follow_offline=(g_follow_offline+1)%3;settings_save();following_rebuild();tab_load(g_tab);continue;}
        if(g_tv_layout){tv_input();sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);frame_begin();draw_home();frame_end();continue;}

        if(ui_touch_hit(&g_in,778,0,182,HEADER_H)){g_side_focus=PROFILE_FOCUS;account_screen();continue;}
        /* Never consume a touch/drag in the controller-only profile focus path. */
        if(g_side_focus==PROFILE_FOCUS&&!g_in.touch_tap&&!g_in.touch_down&&!(p&SCE_CTRL_SQUARE)){if(p&g_btn_ok){account_screen();continue;}if(p&SCE_CTRL_DOWN)g_side_focus=-1;else if(p&(SCE_CTRL_LEFT|g_btn_back))g_side_focus=side_active();if(!(p&(SCE_CTRL_LTRIGGER|SCE_CTRL_RTRIGGER|SCE_CTRL_SELECT))){frame_begin();draw_home();frame_end();continue;}}
        int profile_cell=(SCREEN_W-24)/(6);
        if((p&SCE_CTRL_START)||ui_touch_hit(&g_in,12+profile_cell*(5),FOOTER_Y,profile_cell,SCREEN_H-FOOTER_Y)){
            if(g_in.touch_tap)g_side_focus=-1;
            profile_selected();continue;
        }
        if(ui_touch_hit(&g_in,12+2*profile_cell,FOOTER_Y,profile_cell,SCREEN_H-FOOTER_Y)){games_screen();continue;}
        if (p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
            g_game_filter[0]=g_game_id[0]=g_game_slug[0]=0;
            home_switch((g_tab+(p&SCE_CTRL_RTRIGGER?1:TAB_COUNT-1))%TAB_COUNT);
            continue;
        }
        if(p&SCE_CTRL_SELECT){extras_menu();continue;}
        if(g_in.touch_tap&&g_in.touch_x<GRID_X&&g_in.touch_y<FOOTER_Y){
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
        if(p&SCE_CTRL_SQUARE){games_screen();continue;}

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
            else if(p&SCE_CTRL_UP){if(pos>=0)next=home_move(pos,n,0,-1);if(next<0){if(g_tab==TAB_SEARCH&&tb->sel>=0)tb->sel=-1;else g_side_focus=PROFILE_FOCUS;}}
            if(next>=0)tb->sel=home_index(tb,next);
            if(p&g_btn_back){if(g_tab==TAB_SEARCH)clear_search();g_side_focus=side_active();}
            if(g_tab!=TAB_SEARCH&&home_pos(tb)<0&&n)tb->sel=home_index(tb,0);
            if(g_side_focus<0&&(p&g_btn_ok)){
                if(tb->need_login)g_side_focus=PROFILE_FOCUS;
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
    if(side_icon_atlas)vita2d_free_texture(side_icon_atlas);
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

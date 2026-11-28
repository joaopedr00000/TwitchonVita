#include "m3u8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copies the next line into line (without CR/LF) and returns a pointer past it. */
static const char *next_line(const char *p, char *line, size_t cap)
{
    size_t n = 0;
    while (*p && *p != '\n' && *p != '\r') {
        if (n + 1 < cap)
            line[n++] = *p;
        p++;
    }
    line[n] = 0;
    while (*p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Reads ATTR=value or ATTR="value" from an attribute list. */
static int get_attr(const char *line, const char *attr, char *out, size_t cap)
{
    size_t alen = strlen(attr);
    const char *p = line;
    while ((p = strstr(p, attr)) != NULL) {
        char before = (p == line) ? ':' : p[-1];
        if ((before == ':' || before == ',') && p[alen] == '=') {
            p += alen + 1;
            size_t n = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && n + 1 < cap)
                    out[n++] = *p++;
            } else {
                while (*p && *p != ',' && n + 1 < cap)
                    out[n++] = *p++;
            }
            out[n] = 0;
            return 1;
        }
        p += alen;
    }
    return 0;
}

typedef struct {
    char group[48];
    char name[48];
} media_name_t;

int m3u8_parse_master(const char *text, m3u8_variant_t *out, int max)
{
    static char line[M3U8_URL_MAX + 64];
    media_name_t names[M3U8_MAX_VARIANTS];
    int nnames = 0, count = 0, pending = 0;
    m3u8_variant_t cur;
    const char *p = text;

    memset(&cur, 0, sizeof(cur));
    while (*p) {
        p = next_line(p, line, sizeof(line));
        if (!strncmp(line, "#EXT-X-MEDIA:", 13)) {
            if (nnames < M3U8_MAX_VARIANTS &&
                get_attr(line, "GROUP-ID", names[nnames].group, sizeof(names[nnames].group)) &&
                get_attr(line, "NAME", names[nnames].name, sizeof(names[nnames].name)))
                nnames++;
        } else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            char tmp[64];
            memset(&cur, 0, sizeof(cur));
            if (get_attr(line, "BANDWIDTH", tmp, sizeof(tmp)))
                cur.bandwidth = atol(tmp);
            if (get_attr(line, "RESOLUTION", tmp, sizeof(tmp)))
                sscanf(tmp, "%dx%d", &cur.width, &cur.height);
            if (get_attr(line, "FRAME-RATE", tmp, sizeof(tmp)))
                cur.fps = (float)atof(tmp);
            if (get_attr(line, "VIDEO", tmp, sizeof(tmp))) {
                snprintf(cur.name, sizeof(cur.name), "%s", tmp);
                for (int i = 0; i < nnames; i++)
                    if (!strcmp(names[i].group, tmp))
                        snprintf(cur.name, sizeof(cur.name), "%s", names[i].name);
            }
            if (get_attr(line, "CODECS", tmp, sizeof(tmp)) && !strstr(tmp, "avc1"))
                cur.audio_only = 1;
            if (cur.width == 0)
                cur.audio_only = 1;
            pending = 1;
        } else if (line[0] && line[0] != '#' && pending) {
            snprintf(cur.url, sizeof(cur.url), "%s", line);
            if (!cur.name[0])
                snprintf(cur.name, sizeof(cur.name), "%dp", cur.height);
            if (count < max)
                out[count++] = cur;
            pending = 0;
        }
    }
    return count;
}

static void resolve_url(const char *base, const char *ref, char *out, size_t cap)
{
    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8) || !base) {
        snprintf(out, cap, "%s", ref);
        return;
    }
    const char *q = strchr(base, '?');
    size_t blen = q ? (size_t)(q - base) : strlen(base);
    size_t slash = 0;
    for (size_t i = 0; i < blen; i++)
        if (base[i] == '/')
            slash = i + 1;
    snprintf(out, cap, "%.*s%s", (int)slash, base, ref);
}

/* Recognize explicit Twitch SSAI markers, not arbitrary HLS titles. */
#define AD_RANGES 32
typedef struct { double start, end; int source, ad; } ad_range_t;
static double utc_time(const char *v)
{
    int y,m,d,h,mi,n=0;
    double sec;
    if(sscanf(v,"%d-%d-%dT%d:%d:%lf%n",&y,&m,&d,&h,&mi,&sec,&n)!=6 ||
       y<1970||y>2100||m<1||m>12||d<1||d>31||h<0||h>23||mi<0||mi>59||
       !(sec>=0&&sec<61)||(strcmp(v+n,"Z")&&strcmp(v+n,"+00:00")))return -1;
    static const int md[]={31,28,31,30,31,30,31,31,30,31,30,31};
    long days=0;
    for(int a=1970;a<y;a++)days+=365+(a%4==0&&(a%100!=0||a%400==0));
    int leap=y%4==0&&(y%100!=0||y%400==0);
    if(d>md[m-1]+(m==2&&leap))return -1;
    for(int a=1;a<m;a++)days+=md[a-1]+(a==2&&leap);
    return (days+d-1)*86400.0+h*3600+mi*60+sec;
}
static int amazon_title(const char *v)
{
    return !strcmp(v,"Amazon")||!strncmp(v,"Amazon|",7);
}
static int collect_ad_ranges(const char *text,ad_range_t *ranges,char *line,size_t cap)
{
    int count=0;
    while(*text&&count<AD_RANGES){
        text=next_line(text,line,cap);
        if(strncmp(line,"#EXT-X-DATERANGE:",17))continue;
        char cls[64]={0},id[80]={0},v[128];
        get_attr(line,"CLASS",cls,sizeof(cls));
        get_attr(line,"ID",id,sizeof(id));
        int source=!strcmp(cls,"twitch-stream-source");
        int ad=!strcmp(cls,"twitch-stitched-ad")||!strncmp(id,"stitched-ad-",12);
        if(source){
            if(!get_attr(line,"X-TV-TWITCH-STREAM-SOURCE",v,sizeof(v)))continue;
            if(!strcmp(v,"live"))ad=0;
            else if(amazon_title(v))ad=1;
            else continue;
        }else if(!ad)continue;
        if(!get_attr(line,"START-DATE",v,sizeof(v)))continue;
        double start=utc_time(v),end=-1;
        if(start<0)continue;
        if(get_attr(line,"END-DATE",v,sizeof(v)))end=utc_time(v);
        if(end<=start&&get_attr(line,"DURATION",v,sizeof(v))){
            double duration=atof(v);
            if(duration>0&&duration<=600)end=start+duration;
        }
        if(!source&&end<=start)continue;
        ranges[count++]=(ad_range_t){start,end,source,ad};
    }
    return count;
}
static int date_is_ad(double date,const ad_range_t *ranges,int count)
{
    if(date<0)return 0;
    int source=-1;
    for(int i=0;i<count;i++){
        const ad_range_t *r=&ranges[i];
        if(date+0.001<r->start)continue;
        if(!r->source&&date<r->end-0.001)return 1;
        if(r->source&&(source<0||r->start>=ranges[source].start))source=i;
    }
    return source>=0&&ranges[source].ad&&(ranges[source].end<0||date<ranges[source].end-0.001);
}

int m3u8_parse_media(const char *text, const char *base_url, m3u8_media_t *out)
{
    static char line[8192];
    const char *p = text;
    ad_range_t ranges[AD_RANGES];
    double program_date=-1;
    long long idx = 0;
    int disc = 0, is_ad = 0, have_inf = 0, cur_map = -1;
    float dur = 0;

    memset(out, 0, sizeof(*out));
    if (!text || strncmp(text, "#EXTM3U", 7))
        return -1;
    int nranges=collect_ad_ranges(text,ranges,line,sizeof(line));

    while (*p) {
        p = next_line(p, line, sizeof(line));
        if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) {
            out->target_duration = (float)atof(line + 22);
        } else if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            out->media_sequence = atoll(line + 22);
        } else if (!strncmp(line, "#EXT-X-DISCONTINUITY", 20) && line[20] == 0) {
            disc = 1;
        } else if (!strncmp(line, "#EXT-X-MAP:", 11)) {
            /* fMP4: init segment for the following media segments */
            const char *u = strstr(line, "URI=\"");
            if (u) {
                u += 5;
                const char *e = strchr(u, '"');
                char ref[M3U8_URL_MAX];
                size_t l = e ? (size_t)(e - u) : strlen(u);
                if (l >= sizeof(ref)) l = sizeof(ref) - 1;
                memcpy(ref, u, l);
                ref[l] = 0;
                char abs[M3U8_URL_MAX];
                resolve_url(base_url, ref, abs, sizeof(abs));
                cur_map = -1;
                for (int k = 0; k < out->nmap; k++)
                    if (!strcmp(out->map_url[k], abs))
                        cur_map = k;
                if (cur_map < 0) {
                    if (out->nmap == M3U8_MAX_MAPS) { /* keep the newest ones */
                        memmove(out->map_url[0], out->map_url[1], sizeof(out->map_url[0]) * (M3U8_MAX_MAPS - 1));
                        out->nmap--;
                        for (int k = 0; k < out->count; k++)
                            out->seg[k].map = out->seg[k].map > 0 ? out->seg[k].map - 1 : -1;
                    }
                    snprintf(out->map_url[out->nmap], sizeof(out->map_url[0]), "%s", abs);
                    cur_map = out->nmap++;
                }
            }
        } else if (!strncmp(line, "#EXT-X-ENDLIST", 14)) {
            out->ended = 1;
        } else if (!strncmp(line, "#EXT-X-PROGRAM-DATE-TIME:", 25)) {
            program_date=utc_time(line+25);
        } else if (!strncmp(line, "#EXTINF:", 8)) {
            dur = (float)atof(line + 8);
            const char *title = strchr(line, ',');
            title = title ? title + 1 : "";
            is_ad = amazon_title(title);
            have_inf = 1;
        } else if (line[0] && line[0] != '#') {
            if (have_inf) {
                if (out->count == M3U8_MAX_SEGMENTS) {
                    memmove(&out->seg[0], &out->seg[1], sizeof(out->seg[0]) * (M3U8_MAX_SEGMENTS - 1));
                    out->count--;
                }
                m3u8_segment_t *s = &out->seg[out->count++];
                s->seq = out->media_sequence + idx;
                s->duration = dur;
                s->discontinuity = disc;
                s->is_ad = is_ad || date_is_ad(program_date,ranges,nranges);
                s->map = cur_map;
                resolve_url(base_url, line, s->url, sizeof(s->url));
            }
            if(have_inf && program_date>=0)program_date+=dur;
            idx++;
            disc = 0;
            is_ad = 0;
            have_inf = 0;
        }
    }
    if (out->target_duration <= 0)
        out->target_duration = 2;
    return 0;
}

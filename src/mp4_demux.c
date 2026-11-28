#include "mp4_demux.h"

#include <string.h>

#define MAX_JUMP (10LL * 90000) /* >10 s jump => new timeline (as ts_demux) */
#define FRAME_GAP 3003

/* ------------------------------------------------------------- helpers */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

typedef struct {
    const uint8_t *p;     /* box payload */
    size_t len;           /* payload length */
    const uint8_t *start; /* box start (header) */
    char type[5];
} box_t;

/* Reads the box at *pos within [base, base+len). Returns 0 and advances *pos. */
static int next_box(const uint8_t *base, size_t len, size_t *pos, box_t *b)
{
    if (*pos + 8 > len)
        return -1;
    const uint8_t *h = base + *pos;
    uint64_t size = rd32(h);
    size_t hdr = 8;
    if (size == 1) {
        if (*pos + 16 > len)
            return -1;
        size = rd64(h + 8);
        hdr = 16;
    } else if (size == 0) {
        size = len - *pos;
    }
    if (size < hdr || size > len - *pos)
        return -1;
    memcpy(b->type, h + 4, 4);
    b->type[4] = 0;
    b->start = h;
    b->p = h + hdr;
    b->len = (size_t)size - hdr;
    *pos += (size_t)size;
    return 0;
}

static int find_box(const uint8_t *base, size_t len, const char *type, box_t *out)
{
    size_t pos = 0;
    box_t b;
    while (next_box(base, len, &pos, &b) == 0)
        if (!memcmp(b.type, type, 4)) {
            *out = b;
            return 0;
        }
    return -1;
}

static mp4_track_t *track_by_id(mp4_demux_t *d, uint32_t id)
{
    for (int i = 0; i < d->ntrk; i++)
        if (d->trk[i].id == id)
            return &d->trk[i];
    return NULL;
}

int mp4_looks_like(const uint8_t *data, size_t len)
{
    if (len < 8)
        return 0;
    if (data[0] == 0x47)
        return 0; /* MPEG-TS sync byte */
    const uint8_t *t = data + 4;
    return !memcmp(t, "ftyp", 4) || !memcmp(t, "styp", 4) || !memcmp(t, "moof", 4) ||
           !memcmp(t, "sidx", 4) || !memcmp(t, "moov", 4) || !memcmp(t, "emsg", 4) || !memcmp(t, "prft", 4);
}

/* -------------------------------------------------------- init segment */

static void parse_avcc(mp4_demux_t *d, const uint8_t *p, size_t n)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    if (n < 7)
        return;
    d->nal_len_size = (p[4] & 3) + 1;
    buf_reset(&d->ps);
    size_t off = 5;
    for (int pass = 0; pass < 2; pass++) {
        if (off >= n)
            return;
        int count = pass == 0 ? (p[off] & 0x1F) : p[off];
        off++;
        for (int i = 0; i < count; i++) {
            if (off + 2 > n)
                return;
            size_t l = rd16(p + off);
            off += 2;
            if (off + l > n)
                return;
            buf_append(&d->ps, sc, 4);
            buf_append(&d->ps, p + off, l);
            off += l;
        }
    }
}

/* esds: find the DecoderSpecificInfo (tag 0x05) and read the AudioSpecificConfig */
static void parse_esds(mp4_demux_t *d, const uint8_t *p, size_t n)
{
    for (size_t i = 4; i + 2 < n; i++) {
        if (p[i] != 0x05)
            continue;
        size_t j = i + 1, len = 0;
        for (int k = 0; k < 4 && j < n; k++) {
            uint8_t b = p[j++];
            len = (len << 7) | (b & 0x7F);
            if (!(b & 0x80))
                break;
        }
        if (len < 2 || j + 2 > n)
            continue;
        int obj = p[j] >> 3;
        int fi = ((p[j] & 7) << 1) | (p[j + 1] >> 7);
        int ch = (p[j + 1] >> 3) & 0xF;
        if (obj >= 1 && obj <= 4 && fi < 13 && ch >= 1 && ch <= 2) {
            d->aac_object = obj;
            d->aac_freq_idx = fi;
            d->aac_channels = ch;
            return;
        }
    }
}

static void parse_stsd(mp4_demux_t *d, mp4_track_t *t, const uint8_t *p, size_t n)
{
    if (n < 8)
        return;
    size_t pos = 8; /* fullbox header + entry_count */
    box_t e;
    if (next_box(p, n, &pos, &e) < 0)
        return;
    box_t c;
    if (t->is_video && (!memcmp(e.type, "avc1", 4) || !memcmp(e.type, "avc3", 4)) && e.len > 78) {
        if (find_box(e.p + 78, e.len - 78, "avcC", &c) == 0)
            parse_avcc(d, c.p, c.len);
    } else if (t->is_audio && !memcmp(e.type, "mp4a", 4) && e.len > 28) {
        if (find_box(e.p + 28, e.len - 28, "esds", &c) == 0)
            parse_esds(d, c.p, c.len);
    } else {
        /* unsupported codec (e.g. HEVC/AV1): ignore this track */
        t->is_video = t->is_audio = 0;
    }
}

static void parse_trak(mp4_demux_t *d, const uint8_t *p, size_t n)
{
    if (d->ntrk >= 4)
        return;
    mp4_track_t t;
    memset(&t, 0, sizeof(t));
    box_t b, mdia, x;
    if (find_box(p, n, "tkhd", &b) == 0 && b.len >= 24)
        t.id = rd32(b.p + (b.p[0] == 1 ? 20 : 12));
    if (find_box(p, n, "mdia", &mdia) < 0)
        return;
    if (find_box(mdia.p, mdia.len, "mdhd", &b) == 0 && b.len >= 24)
        t.timescale = rd32(b.p + (b.p[0] == 1 ? 20 : 12));
    if (find_box(mdia.p, mdia.len, "hdlr", &b) == 0 && b.len >= 12) {
        t.is_video = !memcmp(b.p + 8, "vide", 4);
        t.is_audio = !memcmp(b.p + 8, "soun", 4);
    }
    if (!t.timescale || (!t.is_video && !t.is_audio))
        return;
    if (find_box(mdia.p, mdia.len, "minf", &x) == 0 && find_box(x.p, x.len, "stbl", &b) == 0 &&
        find_box(b.p, b.len, "stsd", &x) == 0)
        parse_stsd(d, &t, x.p, x.len);
    if (t.is_video || t.is_audio)
        d->trk[d->ntrk++] = t;
}

int mp4_demux_set_init(mp4_demux_t *d, const uint8_t *data, size_t len)
{
    box_t moov, b;
    size_t pos = 0;

    d->ntrk = 0;
    d->ready = 0;
    d->nal_len_size = 4;
    d->aac_object = 0;
    buf_reset(&d->ps);
    if (find_box(data, len, "moov", &moov) < 0)
        return -1;
    while (next_box(moov.p, moov.len, &pos, &b) == 0)
        if (!memcmp(b.type, "trak", 4))
            parse_trak(d, b.p, b.len);
    /* trex: per-track defaults used when tfhd/trun leave fields out */
    if (find_box(moov.p, moov.len, "mvex", &b) == 0) {
        size_t p2 = 0;
        box_t tx;
        while (next_box(b.p, b.len, &p2, &tx) == 0) {
            if (memcmp(tx.type, "trex", 4) || tx.len < 24)
                continue;
            mp4_track_t *t = track_by_id(d, rd32(tx.p + 4));
            if (t) {
                t->def_duration = rd32(tx.p + 12);
                t->def_size = rd32(tx.p + 16);
                t->def_flags = rd32(tx.p + 20);
            }
        }
    }
    int usable = 0;
    for (int i = 0; i < d->ntrk; i++)
        if ((d->trk[i].is_video && d->ps.size) || (d->trk[i].is_audio && d->aac_object))
            usable = 1;
    d->ready = usable;
    d->disc_pending = 1; /* a new init segment starts a new timeline */
    return usable ? 0 : -1;
}

/* -------------------------------------------------------- media segment */

static void fix_timestamps(mp4_demux_t *d, int64_t *pts, int64_t *dts)
{
    *pts += d->offset;
    *dts += d->offset;
    if (d->have_last) {
        int64_t diff = *pts - d->last_pts;
        if (d->disc_pending || diff > MAX_JUMP || diff < -MAX_JUMP) {
            int64_t adj = (d->last_pts + FRAME_GAP) - *pts;
            d->offset += adj;
            *pts += adj;
            *dts += adj;
        }
    }
    d->disc_pending = 0;
    if (!d->have_last || *pts > d->last_pts)
        d->last_pts = *pts;
    d->have_last = 1;
}

static int64_t to90k(int64_t t, uint32_t timescale)
{
    if (timescale == 90000)
        return t;
    /* split to avoid overflow with large decode times */
    return (t / timescale) * 90000 + (t % timescale) * 90000 / timescale;
}

static int emit_video(mp4_demux_t *d, const uint8_t *s, size_t n, int sync, int64_t pts, int64_t dts)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    int ls = d->nal_len_size;
    int has_idr = 0;

    buf_reset(&d->au);
    for (size_t off = 0; off + (size_t)ls <= n;) {
        size_t l = 0;
        for (int k = 0; k < ls; k++)
            l = (l << 8) | s[off + k];
        off += (size_t)ls;
        if (l == 0 || l > n - off)
            break;
        int type = s[off] & 0x1F;
        if (type == 5)
            has_idr = 1;
        off += l;
    }
    /* keyframes carry the parameter sets in-band for the hardware decoder */
    if ((has_idr || sync) && d->ps.size)
        buf_append(&d->au, d->ps.data, d->ps.size);
    for (size_t off = 0; off + (size_t)ls <= n;) {
        size_t l = 0;
        for (int k = 0; k < ls; k++)
            l = (l << 8) | s[off + k];
        off += (size_t)ls;
        if (l == 0 || l > n - off)
            break;
        buf_append(&d->au, sc, 4);
        buf_append(&d->au, s + off, l);
        off += l;
    }
    if (!d->au.size || !d->cb)
        return 0;
    return d->cb(d->user, TS_STREAM_VIDEO, d->au.data, (int)d->au.size, pts, dts);
}

static int emit_audio(mp4_demux_t *d, const uint8_t *s, size_t n, int64_t pts)
{
    size_t fl = n + 7;
    if (!d->aac_object || fl > 0x1FFF || !d->cb)
        return 0;
    uint8_t h[7];
    h[0] = 0xFF;
    h[1] = 0xF1; /* MPEG-4, no CRC */
    h[2] = (uint8_t)(((d->aac_object - 1) & 3) << 6 | (d->aac_freq_idx & 0xF) << 2 | ((d->aac_channels >> 2) & 1));
    h[3] = (uint8_t)((d->aac_channels & 3) << 6 | ((fl >> 11) & 3));
    h[4] = (uint8_t)((fl >> 3) & 0xFF);
    h[5] = (uint8_t)((fl & 7) << 5 | 0x1F);
    h[6] = 0xFC;
    buf_reset(&d->au);
    buf_append(&d->au, h, 7);
    buf_append(&d->au, s, n);
    return d->cb(d->user, TS_STREAM_AUDIO, d->au.data, (int)d->au.size, pts, pts);
}

static int parse_traf(mp4_demux_t *d, const uint8_t *seg, size_t seg_len, const uint8_t *moof_start,
                      const uint8_t *p, size_t n)
{
    box_t tfhd, b;
    if (find_box(p, n, "tfhd", &tfhd) < 0 || tfhd.len < 8)
        return 0;
    uint32_t tf = rd32(tfhd.p) & 0xFFFFFF;
    mp4_track_t *t = track_by_id(d, rd32(tfhd.p + 4));
    if (!t || (!t->is_video && !t->is_audio))
        return 0;
    size_t o = 8;
    const uint8_t *base = moof_start; /* default-base-is-moof (CMAF) */
    uint32_t def_dur = t->def_duration, def_size = t->def_size, def_flags = t->def_flags;
    if (tf & 0x01) {
        if (o + 8 > tfhd.len)
            return 0;
        uint64_t bo = rd64(tfhd.p + o);
        if (bo < seg_len)
            base = seg + bo;
        o += 8;
    }
    if (tf & 0x02) o += 4;
    if (tf & 0x08) { if (o + 4 > tfhd.len) return 0; def_dur = rd32(tfhd.p + o); o += 4; }
    if (tf & 0x10) { if (o + 4 > tfhd.len) return 0; def_size = rd32(tfhd.p + o); o += 4; }
    if (tf & 0x20) { if (o + 4 > tfhd.len) return 0; def_flags = rd32(tfhd.p + o); o += 4; }

    int64_t dt = 0;
    if (find_box(p, n, "tfdt", &b) == 0 && b.len >= 8)
        dt = b.p[0] == 1 && b.len >= 12 ? (int64_t)rd64(b.p + 4) : (int64_t)rd32(b.p + 4);

    /* one or more trun boxes */
    size_t pos = 0;
    while (next_box(p, n, &pos, &b) == 0) {
        if (memcmp(b.type, "trun", 4) || b.len < 8)
            continue;
        uint32_t fl = rd32(b.p) & 0xFFFFFF;
        uint32_t count = rd32(b.p + 4);
        size_t q = 8;
        const uint8_t *data = base;
        if (fl & 0x01) {
            if (q + 4 > b.len) return 0;
            int32_t off = (int32_t)rd32(b.p + q);
            q += 4;
            data = base + off;
        }
        uint32_t first_flags = def_flags;
        int have_first = 0;
        if (fl & 0x04) {
            if (q + 4 > b.len) return 0;
            first_flags = rd32(b.p + q);
            have_first = 1;
            q += 4;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t dur = def_dur, size = def_size, sflags = (i == 0 && have_first) ? first_flags : def_flags;
            int32_t cto = 0;
            if (fl & 0x100) { if (q + 4 > b.len) return 0; dur = rd32(b.p + q); q += 4; }
            if (fl & 0x200) { if (q + 4 > b.len) return 0; size = rd32(b.p + q); q += 4; }
            if (fl & 0x400) { if (q + 4 > b.len) return 0; sflags = rd32(b.p + q); q += 4; }
            if (fl & 0x800) { if (q + 4 > b.len) return 0; cto = (int32_t)rd32(b.p + q); q += 4; }
            if (data < seg || data + size > seg + seg_len)
                return 0; /* sample outside the segment: stop this run */
            int64_t dts = to90k(dt, t->timescale);
            int64_t pts = to90k(dt + cto, t->timescale);
            fix_timestamps(d, &pts, &dts);
            int r = 0;
            if (t->is_video) {
                int sync = !(sflags & 0x00010000); /* sample_is_non_sync_sample */
                r = emit_video(d, data, size, sync, pts, dts);
            } else {
                r = emit_audio(d, data, size, pts);
            }
            if (r < 0)
                return r;
            data += size;
            dt += dur;
        }
    }
    return 0;
}

int mp4_demux_feed(mp4_demux_t *d, const uint8_t *data, size_t len)
{
    if (!d->ready)
        return -2;
    size_t pos = 0;
    box_t b;
    while (next_box(data, len, &pos, &b) == 0) {
        if (memcmp(b.type, "moof", 4))
            continue;
        size_t p2 = 0;
        box_t traf;
        while (next_box(b.p, b.len, &p2, &traf) == 0) {
            if (memcmp(traf.type, "traf", 4))
                continue;
            int r = parse_traf(d, data, len, b.start, traf.p, traf.len);
            if (r < 0)
                return r;
        }
    }
    return 0;
}

void mp4_demux_discontinuity(mp4_demux_t *d)
{
    d->disc_pending = 1;
}

void mp4_demux_init(mp4_demux_t *d, ts_es_cb cb, void *user)
{
    memset(d, 0, sizeof(*d));
    d->cb = cb;
    d->user = user;
    d->nal_len_size = 4;
}

void mp4_demux_free(mp4_demux_t *d)
{
    buf_free(&d->ps);
    buf_free(&d->au);
    d->ready = 0;
}

/* Bounded conventional MP4 sample-table reader for short clips. Interleave
 * by decode time to avoid filling one player queue while starving the other. */
typedef struct {
    mp4_track_t *track;
    box_t stsz,stsc,stco,stts,ctts,stss;
    uint32_t count,idx,chunk,in_chunk,sc,ts,ts_left,ct,ct_left,sync;
    uint32_t fixed,chunks,co64;
    uint64_t off;
    int64_t dt;
} flat_track_t;
static int flat_table(const box_t *stbl,const char *name,box_t *b,size_t stride){
    if(find_box(stbl->p,stbl->len,name,b)<0||b->len<8)return -1;
    return rd32(b->p+4)<=(b->len-8)/stride?0:-1;
}
static uint32_t flat_val(const box_t *b,uint32_t i,size_t stride,size_t col){return rd32(b->p+8+(size_t)i*stride+col);}
static int flat_chunk(flat_track_t *t){
    if(t->chunk>=t->chunks)return -1;
    uint32_t n=rd32(t->stsc.p+4);
    while(t->sc+1<n&&flat_val(&t->stsc,t->sc+1,12,0)<=t->chunk+1)t->sc++;
    if(!n||flat_val(&t->stsc,t->sc,12,0)>t->chunk+1||flat_val(&t->stsc,t->sc,12,8)!=1)return -1;
    t->in_chunk=flat_val(&t->stsc,t->sc,12,4);
    if(!t->in_chunk)return -1;
    t->off=t->co64?rd64(t->stco.p+8+(size_t)t->chunk*8):flat_val(&t->stco,t->chunk,4,0);
    return 0;
}
static int flat_init(flat_track_t *t,mp4_demux_t *d,const box_t *trak){
    box_t tk,mdia,minf,stbl;
    memset(t,0,sizeof(*t));
    if(find_box(trak->p,trak->len,"tkhd",&tk)<0||tk.len<24)return -1;
    t->track=track_by_id(d,rd32(tk.p+(tk.p[0]==1?20:12)));
    if(!t->track)return 1;
    if(find_box(trak->p,trak->len,"mdia",&mdia)<0||find_box(mdia.p,mdia.len,"minf",&minf)<0||find_box(minf.p,minf.len,"stbl",&stbl)<0)return -1;
    if(find_box(stbl.p,stbl.len,"stsz",&t->stsz)<0||t->stsz.len<12)return -1;
    t->fixed=rd32(t->stsz.p+4);t->count=rd32(t->stsz.p+8);
    if(!t->count||t->count>20000||(!t->fixed&&t->count>(t->stsz.len-12)/4))return -1;
    if(flat_table(&stbl,"stsc",&t->stsc,12)<0||flat_table(&stbl,"stts",&t->stts,8)<0)return -1;
    if(flat_table(&stbl,"stco",&t->stco,4)<0){if(flat_table(&stbl,"co64",&t->stco,8)<0)return -1;t->co64=1;}
    t->chunks=rd32(t->stco.p+4);
    if(flat_table(&stbl,"ctts",&t->ctts,8)<0)memset(&t->ctts,0,sizeof(t->ctts));
    if(flat_table(&stbl,"stss",&t->stss,4)<0)memset(&t->stss,0,sizeof(t->stss));
    return flat_chunk(t);
}
static int flat_emit(mp4_demux_t *d,flat_track_t *t,const uint8_t *data,size_t len){
    uint32_t size=t->fixed?t->fixed:rd32(t->stsz.p+12+(size_t)t->idx*4);
    if(!size||t->off>len||size>len-t->off)return -1;
    if(!t->ts_left){if(t->ts>=rd32(t->stts.p+4))return -1;t->ts_left=flat_val(&t->stts,t->ts,8,0);if(!t->ts_left)return -1;}
    int64_t comp=0;
    if(t->ctts.p){if(!t->ct_left){if(t->ct>=rd32(t->ctts.p+4))return -1;t->ct_left=flat_val(&t->ctts,t->ct,8,0);if(!t->ct_left)return -1;}
        uint32_t v=flat_val(&t->ctts,t->ct,8,4);comp=t->ctts.p[0]==1?(int64_t)(int32_t)v:(int64_t)v;if(!--t->ct_left)t->ct++;}
    int sync=!t->stss.p;
    if(t->stss.p&&t->sync<rd32(t->stss.p+4)&&flat_val(&t->stss,t->sync,4,0)==t->idx+1){sync=1;t->sync++;}
    int64_t pts=to90k(t->dt+comp,t->track->timescale),dts=to90k(t->dt,t->track->timescale);
    int rc=!d?0:t->track->is_video?emit_video(d,data+(size_t)t->off,size,sync,pts,dts):emit_audio(d,data+(size_t)t->off,size,pts);
    if(rc<0)return -1;
    t->dt+=flat_val(&t->stts,t->ts,8,4);if(!--t->ts_left)t->ts++;
    t->idx++;t->off+=size;if(!--t->in_chunk&&t->idx<t->count){t->chunk++;if(flat_chunk(t)<0)return -1;}
    return 0;
}
int mp4_demux_file_seek(mp4_demux_t *d,const uint8_t *data,size_t len,int start,volatile int *actual){
    box_t moov,b;size_t pos=0;flat_track_t tracks[4];int n=0,video=0;
    if(mp4_demux_set_init(d,data,len)<0||find_box(data,len,"moov",&moov)<0)return -1;
    if(find_box(moov.p,moov.len,"mvex",&b)==0){if(actual)*actual=0;return start>0?-1:mp4_demux_feed(d,data,len);}
    while(next_box(moov.p,moov.len,&pos,&b)==0)if(!memcmp(b.type,"trak",4)){
        if(n>=4)return -1;
        int rc=flat_init(&tracks[n],d,&b);if(rc<0)return -1;if(rc>0)continue;
        if(tracks[n].track->is_video&&d->ps.size)video=1;n++;
    }
    if(!video)return -1;
    int64_t anchor=0;
    if(start>0){
        for(int i=0;i<n;i++)if(tracks[i].track->is_video){
            flat_track_t walk=tracks[i],best=walk;
            while(walk.idx<walk.count&&to90k(walk.dt,walk.track->timescale)<=(int64_t)start*90000){
                if(!walk.stss.p||(walk.sync<rd32(walk.stss.p+4)&&flat_val(&walk.stss,walk.sync,4,0)==walk.idx+1))best=walk;
                if(flat_emit(NULL,&walk,data,len)<0)return -1;
            }
            tracks[i]=best;anchor=to90k(best.dt,best.track->timescale);break;
        }
        for(int i=0;i<n;i++)if(!tracks[i].track->is_video){
            while(tracks[i].idx+1<tracks[i].count&&to90k(tracks[i].dt,tracks[i].track->timescale)<anchor)
                if(flat_emit(NULL,&tracks[i],data,len)<0)return -1;
        }
    }
    if(actual)*actual=(int)(anchor/90000);
    for(;;){
        int k=-1;int64_t best=0;
        for(int i=0;i<n;i++)if(tracks[i].idx<tracks[i].count){int64_t ts=to90k(tracks[i].dt,tracks[i].track->timescale);if(k<0||ts<best){k=i;best=ts;}}
        if(k<0)return 0;
        if(flat_emit(d,&tracks[k],data,len)<0)return -1;
    }
}

int mp4_demux_file(mp4_demux_t *d,const uint8_t *data,size_t len){return mp4_demux_file_seek(d,data,len,0,NULL);}

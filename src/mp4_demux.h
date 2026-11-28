#ifndef VT_MP4_DEMUX_H
#define VT_MP4_DEMUX_H

/*
 * Fragmented MP4 (CMAF) demuxer for HLS streams that use #EXT-X-MAP, as
 * Twitch does for channels on Enhanced Broadcasting. Produces the same output
 * as ts_demux: H.264 access units in Annex B (SPS/PPS before keyframes) and
 * single ADTS-framed AAC frames, with 90 kHz timestamps made continuous across
 * discontinuities.
 */

#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "ts_demux.h" /* ts_es_cb, TS_STREAM_*, TS_NOPTS */

typedef struct {
    uint32_t id;
    uint32_t timescale;
    int is_video, is_audio;
    uint32_t def_duration, def_size, def_flags; /* from trex */
} mp4_track_t;

typedef struct {
    mp4_track_t trk[4];
    int ntrk;
    int ready;                 /* init segment parsed */

    /* H.264 */
    int nal_len_size;
    buf_t ps;                  /* SPS + PPS in Annex B */
    /* AAC (AudioSpecificConfig) */
    int aac_object, aac_freq_idx, aac_channels;

    buf_t au;                  /* scratch for one converted sample */

    /* timestamp continuity (same rules as ts_demux) */
    int64_t offset, last_pts;
    int have_last, disc_pending;

    ts_es_cb cb;
    void *user;
} mp4_demux_t;

void mp4_demux_init(mp4_demux_t *d, ts_es_cb cb, void *user);
void mp4_demux_free(mp4_demux_t *d);
/* Parses the init segment (ftyp + moov). Returns 0 if a usable track was found. */
int mp4_demux_set_init(mp4_demux_t *d, const uint8_t *data, size_t len);
/* Parses one media segment (styp/sidx/moof/mdat...). Returns <0 if the
 * callback aborted or the init segment is missing. */
int mp4_demux_feed(mp4_demux_t *d, const uint8_t *data, size_t len);
int mp4_demux_file(mp4_demux_t *d,const uint8_t *data,size_t len);
void mp4_demux_discontinuity(mp4_demux_t *d);

/* 1 if the bytes look like an MP4 segment (box header), 0 for MPEG-TS. */
int mp4_looks_like(const uint8_t *data, size_t len);

#endif

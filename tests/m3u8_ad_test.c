#include "m3u8.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static m3u8_media_t media;
int main(void)
{
    const char *pl="#EXTM3U\n"
        "#EXT-X-TARGETDURATION:2\n"
        "#EXT-X-MEDIA-SEQUENCE:100\n"
        "#EXT-X-PROGRAM-DATE-TIME:2026-11-27T08:00:00.000Z\n"
        "#EXTINF:2,live\nlive0.ts\n"
        "#EXTINF:2,Amazon|creative\nad1.ts\n"
        "#EXTINF:2,\nad2.ts\n"
        "#EXTINF:2,Some legitimate title\nlive3.ts\n"
        "#EXT-X-DATERANGE:ID=\"stitched-ad-1\",CLASS=\"twitch-stitched-ad\",START-DATE=\"2026-11-27T08:00:02.000Z\",DURATION=4.0\n";
    assert(m3u8_parse_media(pl,"https://example.test/path/index.m3u8",&media)==0);
    assert(media.count==4 && media.seg[0].seq==100);
    assert(!media.seg[0].is_ad && media.seg[1].is_ad && media.seg[2].is_ad && !media.seg[3].is_ad);
    assert(!strcmp(media.seg[3].url,"https://example.test/path/live3.ts"));
    pl="#EXTM3U\n"
       "#EXT-X-DATERANGE:ID=\"a\",CLASS=\"twitch-stream-source\",START-DATE=\"2026-11-27T08:00:00Z\",END-ON-NEXT=YES,X-TV-TWITCH-STREAM-SOURCE=\"Amazon|creative\"\n"
       "#EXT-X-DATERANGE:ID=\"b\",CLASS=\"twitch-stream-source\",START-DATE=\"2026-11-27T08:00:04Z\",END-ON-NEXT=YES,X-TV-TWITCH-STREAM-SOURCE=\"live\"\n"
       "#EXT-X-PROGRAM-DATE-TIME:2026-11-27T08:00:00Z\n"
       "#EXTINF:2,\na.ts\n#EXTINF:2,\nb.ts\n#EXTINF:2,\nc.ts\n";
    assert(m3u8_parse_media(pl,"https://example.test/index.m3u8",&media)==0);
    assert(media.count==3 && media.seg[0].is_ad && media.seg[1].is_ad && !media.seg[2].is_ad);
    pl="#EXTM3U\n#EXTINF:2,Amazon\na.ts\n#EXTINF:2,live\nb.ts\n#EXTINF:2,other title\nc.ts\n";
    assert(m3u8_parse_media(pl,"https://example.test/index.m3u8",&media)==0);
    assert(media.count==3 && media.seg[0].is_ad && !media.seg[1].is_ad && !media.seg[2].is_ad);
    assert(m3u8_parse_media(NULL,NULL,&media)<0);
    puts("PASS Twitch HLS ad markers, date ranges and clean resumption");
    return 0;
}

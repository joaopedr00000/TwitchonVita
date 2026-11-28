#include "m3u8.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static m3u8_media_t m;
int main(void){
 static char pl[16000];strcpy(pl,"#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:100\n");
 for(int i=0;i<160;i++){char line[64];snprintf(line,sizeof(line),"#EXTINF:2.0,\ns%d.ts\n",i);strcat(pl,line);}
 strcat(pl,"#EXT-X-ENDLIST\n");
 assert(!m3u8_parse_vod(pl,"https://example.com/a/p.m3u8",&m,-1,0));
 assert(m.count==64&&m.first_index==0&&m.total_segments==160&&m.total_seconds==320&&m.seg[0].seq==100);
 assert(!m3u8_parse_vod(pl,"https://example.com/a/p.m3u8",&m,64,0));
 assert(m.first_seconds==128&&m.seg[0].seq==164);
 assert(!m3u8_parse_vod(pl,"https://example.com/a/p.m3u8",&m,-1,301));
 assert(m.first_index==150&&m.count==10&&!strcmp(m.seg[0].url,"https://example.com/a/s150.ts"));
 assert(!m3u8_parse_media(pl,"https://example.com/a/p.m3u8",&m)&&m.seg[0].seq==196);
 return 0;
}
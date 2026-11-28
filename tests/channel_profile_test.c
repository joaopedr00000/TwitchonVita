#include "twitch.h"
#include <assert.h>
#include <string.h>
int main(void){
    char url[96],bio[32];
    assert(twitch_channel_url("gaules",url,sizeof(url))==0);
    assert(!strcmp(url,"https://www.twitch.tv/gaules"));
    assert(twitch_channel_url("bad/path",url,sizeof(url))<0);
    assert(twitch_channel_url("abc",url,3)<0);
    assert(twitch_parse_channel_bio("{\"data\":{\"user\":{\"description\":\"Hello\"}}}",bio,sizeof(bio))==0);
    assert(!strcmp(bio,"Hello"));
    assert(twitch_parse_channel_bio("{\"data\":{\"user\":null}}",bio,sizeof(bio))<0);
    char id[32];
    assert(twitch_parse_follow_state("{\"data\":{\"user\":{\"id\":\"123\",\"self\":{\"canFollow\":true,\"follower\":null}}}}",id,sizeof(id))==0);
    assert(!strcmp(id,"123"));
    assert(twitch_parse_follow_state("{\"data\":{\"user\":{\"id\":\"123\",\"self\":{\"canFollow\":false,\"follower\":{}}}}}",id,sizeof(id))==1);
    assert(twitch_parse_follow_state("{\"data\":{\"user\":{\"id\":\"123\",\"self\":{\"canFollow\":false,\"follower\":null}}}}",id,sizeof(id))==2);
    assert(twitch_parse_follow_state("{\"errors\":[{}],\"data\":{\"user\":{\"id\":\"123\",\"self\":{\"canFollow\":true,\"follower\":null}}}}",id,sizeof(id))<0);
    assert(twitch_parse_follow_state("{\"data\":{\"user\":{\"id\":\"123\",\"self\":{}}}}",id,sizeof(id))<0);
    assert(twitch_parse_helix_follow("{\"data\":[]}","123")==0);
    assert(twitch_parse_helix_follow("{\"data\":[{\"broadcaster_id\":\"123\"}]}","123")==1);
    assert(twitch_parse_helix_follow("{\"data\":[{\"broadcaster_id\":\"456\"}]}","123")==-1);
    assert(twitch_parse_helix_follow("{\"error\":\"Unauthorized\"}","123")==-1);
    twitch_category_t g[2];twitch_stream_t s[2];
    assert(twitch_parse_categories("{\"data\":[{\"id\":\"123\",\"name\":\"Game\",\"box_art_url\":\"https://static-cdn.jtvnw.net/ttv-boxart/123-{width}x{height}.jpg\"}]}",g,2)==1);
    assert(!strcmp(g[0].art,"https://static-cdn.jtvnw.net/ttv-boxart/123-144x192.jpg"));
    assert(twitch_parse_categories("{\"data\":[{\"id\":\"bad\",\"name\":\"Invalid\"}]}",g,2)==0);
    assert(twitch_parse_category_streams("{\"data\":[{\"user_login\":\"test\",\"user_name\":\"Test\",\"game_name\":\"Game\",\"viewer_count\":17,\"thumbnail_url\":\"https://example.com/{width}x{height}.jpg\"}]}",s,2)==1);
    assert(s[0].live&&s[0].viewers==17&&!strcmp(s[0].preview,"https://example.com/320x180.jpg"));
    return 0;
}

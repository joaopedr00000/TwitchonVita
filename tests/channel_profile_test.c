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
    return 0;
}

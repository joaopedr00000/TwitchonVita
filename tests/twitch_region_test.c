#include "twitch.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){
    char out[1024],global[1024];
    assert(twitch_build_list_body(TW_LIST_TOP,"PT",out,sizeof(out))>0);
    assert(strstr(out,"options: {languages: [PT]}"));
    assert(twitch_build_list_body(TW_LIST_TOP,"ZH_HK",out,sizeof(out))>0);
    assert(strstr(out,"[ZH_HK]"));
    assert(twitch_build_list_body(TW_LIST_TOP,"",global,sizeof(global))>0);
    assert(!strstr(global,"languages:"));
    assert(twitch_build_list_body(TW_LIST_TOP,"PT]} invalid",out,sizeof(out))>0);
    assert(!strcmp(out,global));
    assert(twitch_build_list_body(TW_LIST_SEARCH,"PT",out,sizeof(out))>0);
    assert(strstr(out,"searchFor") && !strstr(out,"languages:"));
    assert(twitch_build_list_body(TW_LIST_FOLLOWED,"PT",out,sizeof(out))>0);
    assert(strstr(out,"followedLiveUsers") && !strstr(out,"languages:"));
    puts("PASS Explore language filter and global/search/follow isolation");
    return 0;
}

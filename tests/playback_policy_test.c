#include "playback_policy.h"
#include <assert.h>
int main(void) {
    m3u8_variant_t a={.width=640,.height=360,.fps=30,.bandwidth=700000},b={.width=854,.height=480,.fps=30,.bandwidth=1400000},c={.width=1280,.height=720,.fps=30,.bandwidth=2500000},d={.width=1280,.height=720,.fps=60,.bandwidth=4500000};
    m3u8_variant_t *v[]={&a,&b,&c,&d};
    assert(vt_profile_pick(v,4,VT_PROFILE_ECO)==1);
    assert(vt_profile_pick(v,4,VT_PROFILE_BALANCED)==2);
    assert(vt_profile_pick(v,4,VT_PROFILE_QUALITY)==3);
    assert(vt_profile_lower(v,4,&d)==2 && vt_profile_lower(v,4,&c)==1 && vt_profile_lower(v,4,&a)==-1);
    assert(vt_profile_edge(VT_PROFILE_LOW_LATENCY)<vt_profile_edge(VT_PROFILE_BALANCED));
    assert(vt_profile_buffer_ms(VT_PROFILE_BALANCED,3)>vt_profile_buffer_ms(VT_PROFILE_BALANCED,0));
    assert(vt_profile_buffer_ms(VT_PROFILE_ECO,99)<=2800);
    return 0;
}

#include "seek_burst.h"
#include <assert.h>
int main(void){
    vt_seek_burst s={0};
    vt_seek_add(&s,100,500,10,1);vt_seek_add(&s,101,500,10,2);vt_seek_add(&s,102,500,10,3);
    assert(s.base==100&&s.target==130&&!vt_seek_due(&s,450002)&&vt_seek_due(&s,450003));
    vt_seek_add(&s,103,500,-30,4);assert(s.target==100);
    s=(vt_seek_burst){0};assert(!vt_seek_tap(&s,1,100,500,10));assert(vt_seek_tap(&s,1,101,500,20));assert(s.target==111);
    assert(vt_seek_tap(&s,1,102,500,30)&&s.target==121);
    s=(vt_seek_burst){0};assert(!vt_seek_tap(&s,-1,100,500,1));assert(!vt_seek_tap(&s,-1,100,500,500000));assert(vt_seek_tap(&s,-1,100,500,500001));assert(s.target==90);
    s=(vt_seek_burst){0};vt_seek_add(&s,5,500,-10,1);assert(s.target==0);
    vt_seek_add(&s,5,500,1000,2);assert(s.target==499);
    return 0;
}

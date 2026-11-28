#include "ime_utf8.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
int main(void){
  uint16_t a[]={ 'O','l',0xe1,' ',0x4e2d,0xd83d,0xde00,0 };
  char b[64];uint16_t back[64];
  assert(vt_utf16_to_utf8(b,sizeof b,a,8)>0);
  vt_utf8_to_utf16(back,64,b);
  assert(memcmp(a,back,sizeof a)==0);
  assert(vt_utf16_to_utf8(b,1,a,8)==0&&b[0]==0);
  const uint16_t invalid[]={0xd800,'X',0};vt_utf16_to_utf8(b,sizeof b,invalid,3);
  assert(strcmp(b,"?X")==0);
  puts("PASS Vita keyboard UTF16 UTF8 round-trip, surrogate pairs, buffer bounds");
  return 0;
}

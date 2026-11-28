#include "chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 char out[CHAT_TEXT_MAX];char long_msg[465];
 assert(chat_prepare_outgoing("  Ola Twitch  ",out,sizeof out)==10);
 assert(strcmp(out,"Ola Twitch")==0);
 assert(chat_prepare_outgoing(" Olá 世界 ",out,sizeof out)>0);
 assert(chat_prepare_outgoing("\r\nPRIVMSG #other :injected",out,sizeof out)<0);
 assert(chat_prepare_outgoing("Hello\nJOIN #other",out,sizeof out)<0);
 assert(chat_prepare_outgoing("   ",out,sizeof out)<0);
 memset(long_msg,'a',sizeof long_msg-1);long_msg[sizeof long_msg-1]=0;
 assert(chat_prepare_outgoing(long_msg,out,sizeof out)<0);
 assert(chat_prepare_outgoing("OK",out,2)<0);
 assert(chat_prepare_outgoing(" OK ",out,sizeof out)==2);
 char reason[64];
 assert(chat_parse_send_result("{\"data\":[{\"message_id\":\"abc-123\",\"is_sent\":true,\"drop_reason\":null}]}",reason,sizeof reason)==0);
 assert(reason[0]==0);
 assert(chat_parse_send_result("{\"data\":[{\"message_id\":\"\",\"is_sent\":false,\"drop_reason\":{\"code\":\"automod_held\",\"message\":\"held\"}}]}",reason,sizeof reason)==1);
 assert(strcmp(reason,"automod_held")==0);
 assert(chat_parse_send_result("{\"data\":[{\"is_sent\":false,\"drop_reason\":{\"code\":\"verified_phone_number\"}}]}",reason,sizeof reason)==1);
 assert(strcmp(reason,"verified_phone_number")==0);
 assert(chat_parse_send_result("{\"data\":[{\"is_sent\":false,\"drop_reason\":null}]}",reason,sizeof reason)==1);
 assert(strcmp(reason,"unknown_drop")==0);
 assert(chat_parse_send_result("{\"data\":[{\"is_sent\":\"true\"}]}",reason,sizeof reason)==-1);
 assert(chat_parse_send_result("{\"data\":[{\"is_sent\":true,\"message_id\":\"\"}]}",reason,sizeof reason)==-1);
 assert(chat_parse_send_result("{\"data\":[]}",reason,sizeof reason)==-1);
 assert(chat_parse_send_result("invalid JSON",reason,sizeof reason)==-1);
 puts("PASS: chat payload validation and Helix accepted, dropped, malformed responses");
 return 0;
}

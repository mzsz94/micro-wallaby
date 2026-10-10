"""Exercise the production KIT HTTP handler, framing parser and route guards."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

APP = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define CONFIG_GATEWAY_NU40_KIT 1
#define CONFIG_GATEWAY_API_TOKEN "example-test-token"
#define DISPLAY_TEXT_MAX 84U
#define snprintk snprintf
#include "gateway_http.h"
#include "gateway_link.h"
static const char *input;
static size_t remaining, sent_size, display_size;
static char output[4096], display[85], selected[32];
static int display_calls, select_calls, display_error;
static int scan_error, transport_error, coex_error;
static int mode_calls, mode_error;
static char scan_mode[8];
static int audio_starts, audio_stops, audio_error;
static uint32_t audio_session;
static int smp_calls, smp_error;
static int alarm_calls, alarm_error;
static bool alarm_active;
static int robot_calls;
int gateway_robot_command(const char *command) { robot_calls++;return !strcmp(command,"stop")?0:-EPERM; }
int gateway_robot_status(char *s,size_t n,bool accepted) { return snprintf(s,n,"{\"accepted\":%s,\"command\":\"stop\",\"simulated\":true}",accepted?"true":"false"); }
int gateway_alarm_status(char *s,size_t n) { return snprintf(s,n,"{\"available\":true,\"active\":null,\"acknowledged\":false}"); }
int gateway_alarm_set(bool active) { alarm_calls++;alarm_active=active;return alarm_error; }
bool gateway_smp_available(void) { return true; }
int gateway_smp_transaction(const uint8_t *request,size_t size,uint8_t *reply,size_t capacity) {
 assert(size<=1024&&capacity>=1024);smp_calls++;if(smp_error)return smp_error;
 memcpy(reply,request,size);reply[0]++;return size;
}
int gateway_audio_status(char *s,size_t n) { return snprintf(s,n,"{\"available\":true,\"running\":false}"); }
bool gateway_audio_available(void) { return true; }
int gateway_audio_start(uint32_t session) { audio_starts++;audio_session=session;return audio_error; }
int gateway_audio_stop(void) { audio_stops++;return audio_error; }
size_t gateway_audio_chunk(uint8_t *out,size_t n) { assert(n>=176);memset(out,0,176);out[0]='W';out[1]='A';return 176; }
static int64_t k_uptime_get(void) { return 0; }
int http_read_before(int fd,void *data,size_t n,int64_t deadline) {
 (void)fd;(void)deadline;
 if(n>remaining)return -EIO;
 memcpy(data,input,n);input+=n;remaining-=n;return 0;
}
int http_send_all(int fd,const void *data,size_t n) {
 (void)fd;assert(sent_size+n<sizeof(output));memcpy(output+sent_size,data,n);
 sent_size+=n;output[sent_size]=0;return 0;
}
void gateway_get_state(struct gateway_state *s) { memset(s,0,sizeof(*s));s->wifi_connected=true;s->ble_connected=true;s->lcd_ready=true;strcpy(s->wifi_ipv4,"192.168.1.9"); }
void gateway_ota_status(char *s,size_t n) { snprintf(s,n,"{\"busy\":false}"); }
int gateway_peer_candidates(char *s,size_t n) { return snprintf(s,n,"[{\"address\":\"C0:11:22:33:44:55\",\"type\":\"random\"}]"); }
int gateway_scan_diagnostics(char *s,size_t n) { return scan_error ? -ENOSPC : snprintf(s,n,"{\"total\":3}"); }
int gateway_hci_diagnostics(char *s,size_t n) { return snprintf(s,n,"{\"reports\":4}"); }
int gateway_hci_transport_diagnostics(char *s,size_t n) { return transport_error ? (int)n : snprintf(s,n,"{\"uart_frames\":5}"); }
int gateway_coex_scan_diagnostics(char *s,size_t n) { return coex_error ? -ENOSPC : snprintf(s,n,"{\"reports\":6}"); }
int gateway_scan_mode(const char *mode) { mode_calls++;strcpy(scan_mode,mode);return mode_error; }
int gateway_select_peer(const char *a,const char *t) { select_calls++;snprintf(selected,sizeof(selected),"%s %s",a,t);return 0; }
int gateway_display_text(const uint8_t *s,size_t n,int t) { (void)t;display_calls++;display_size=n;memcpy(display,s,n);return display_error; }
'''
SUFFIX = r'''
static void request(const char *method,const char *path,const char *body,size_t length,bool auth) {
 static char request[2048];
 int n=snprintf(request,sizeof(request),"%s %s HTTP/1.1\r\nHost: device\r\n%sContent-Length: %zu\r\n\r\n",method,path,auth?"Authorization: Bearer example-test-token\r\n":"",length);
 assert(n>0);memcpy(request+n,body,length);input=request;remaining=n+length;sent_size=0;output[0]=0;
 handle(1,0);
}
static void expect(int code) { char wanted[32];snprintf(wanted,sizeof(wanted),"HTTP/1.1 %d ",code);assert(strncmp(output,wanted,strlen(wanted))==0); }
int main(void) {
 char full[85];memset(full,'A',sizeof(full));
 const char smp[] = {2,0,0,1,0,1,10,1,0xa0};
 request("GET","/v1/robot/status","",0,true);expect(200);
 request("POST","/v1/robot/command","stop",4,true);expect(200);assert(robot_calls==1);
 request("POST","/v1/robot/command","forward",7,true);expect(422);assert(robot_calls==2);
 request("POST","/v1/robot/command","stop",4,false);expect(401);assert(robot_calls==2);
 request("POST","/v1/robot/command","stop\0",5,true);expect(422);assert(robot_calls==2);
 request("GET","/v1/alarm/status","",0,true);expect(200);
 request("POST","/v1/alarm","on",2,true);expect(200);assert(alarm_calls==1&&alarm_active);
 request("POST","/v1/alarm","off",3,true);expect(200);assert(alarm_calls==2&&!alarm_active);
 request("POST","/v1/alarm","on",2,false);expect(401);assert(alarm_calls==2);
 request("POST","/v1/alarm","ON",2,true);expect(422);assert(alarm_calls==2);
 alarm_error=-EIO;request("POST","/v1/alarm","on",2,true);expect(422);alarm_error=0;
 request("GET","/v1/smp/status","",0,true);expect(200);
 request("POST","/v1/smp",smp,sizeof(smp),true);expect(200);assert(smp_calls==1);
 request("POST","/v1/smp",smp,sizeof(smp),false);expect(401);assert(smp_calls==1);
 request("POST","/v1/smp",smp,7,true);expect(400);assert(smp_calls==1);
 smp_error=-ENOTSUP;request("POST","/v1/smp",smp,sizeof(smp),true);expect(503);smp_error=0;
 request("GET","/v1/audio/status","",0,true);expect(200);assert(strstr(output,"\"available\":true")!=NULL);
 request("GET","/v1/audio/status","",0,false);expect(401);
 request("GET","/v1/audio/chunk","",0,true);expect(200);assert(strstr(output,"application/octet-stream")!=NULL);assert(strstr(output,"Content-Length: 176")!=NULL);
 request("GET","/v1/audio/chunk","",0,false);expect(401);
 request("POST","/v1/audio/start","4294967295",10,true);expect(202);assert(audio_starts==1&&audio_session==UINT32_MAX);
 request("POST","/v1/audio/start","0",1,true);expect(422);assert(audio_starts==1);
 request("POST","/v1/audio/start","4294967296",10,true);expect(422);assert(audio_starts==1);
 request("POST","/v1/audio/start","-1",2,true);expect(422);assert(audio_starts==1);
 request("POST","/v1/audio/start","1\n",2,true);expect(422);assert(audio_starts==1);
 request("POST","/v1/audio/start","1",1,false);expect(401);assert(audio_starts==1);
 request("POST","/v1/audio/stop","stop",4,true);expect(202);assert(audio_stops==1);
 request("POST","/v1/audio/stop","stop",4,false);expect(401);assert(audio_stops==1);
 request("POST","/v1/audio/stop","STOP",4,true);expect(422);assert(audio_stops==1);
 audio_error=-EBUSY;request("POST","/v1/audio/start","2",1,true);expect(409);audio_error=0;
 request("POST","/v1/display",full,84,true);expect(200);assert(display_calls==1&&display_size==84);
 request("POST","/v1/display",full,85,true);expect(400);assert(display_calls==1);
 request("POST","/v1/display","x",1,false);expect(401);assert(display_calls==1);
 request("POST","/v1/display","x\n ",3,true);expect(200);assert(display_size==3&&!memcmp(display,"x\n ",3));
 display_error=-EIO;request("POST","/v1/display","x",1,true);expect(422);
 request("POST","/v1/display","",0,true);expect(422);
 request("POST","/v1/lcd","1234",4,true);expect(405);
 request("POST","/v1/ota",full,80,true);expect(405);
 request("POST","/v1/ota/cancel","",0,true);expect(405);
 request("POST","/v1/peer","C0:11:22:33:44:55 random",24,true);expect(202);assert(select_calls==1&&!strcmp(selected,"C0:11:22:33:44:55 random"));
 request("POST","/v1/peer","C0:11:22:33:44:55 random",23,true);expect(422);assert(select_calls==1);
 request("POST","/v1/peer","C0:11:22:33:44:55 bogus!",24,true);expect(422);assert(select_calls==1);
 request("GET","/v1/peers","",0,true);expect(200);assert(strstr(output,"{\"peers\":[{")!=NULL);
 request("GET","/v1/scan","",0,true);expect(200);assert(strstr(output,"{\"adapter\":{\"total\":3},\"hci\":{\"reports\":4},\"transport\":{\"uart_frames\":5},\"coex\":{\"reports\":6}}")!=NULL);
 request("GET","/v1/scan","",0,false);expect(401);
 transport_error=1;request("GET","/v1/scan","",0,true);expect(503);transport_error=0;
 coex_error=1;request("GET","/v1/scan","",0,true);expect(503);coex_error=0;
 scan_error=1;request("GET","/v1/scan","",0,true);expect(503);scan_error=0;
 request("POST","/v1/scan","passive",7,true);expect(202);assert(mode_calls==1&&!strcmp(scan_mode,"passive"));
 request("POST","/v1/scan","active",6,true);expect(202);assert(mode_calls==2&&!strcmp(scan_mode,"active"));
 request("POST","/v1/scan","active",6,false);expect(401);assert(mode_calls==2);
 request("POST","/v1/scan","active\0",7,true);expect(422);assert(mode_calls==2);
 request("POST","/v1/scan","passiv",6,true);expect(422);assert(mode_calls==2);
 request("POST","/v1/scan","",0,true);expect(422);assert(mode_calls==2);
 mode_error=-EBUSY;request("POST","/v1/scan","passive",7,true);expect(409);assert(mode_calls==3);
 request("GET","/v1/status","",0,true);expect(200);assert(strstr(output,"\"protocol\":\"nu40-kit-display\"")!=NULL);assert(strstr(output,"\"power\":false,\"ota\":false")!=NULL);
 return 0;
}
'''

class KitHttpTests(unittest.TestCase):
    def test_production_handler_limits_authentication_and_unsupported_routes(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        self.assertIsNotNone(compiler)
        source = (APP / 'src/http_server.c').read_text()
        handler = source[source.index('static void respond('):source.index('void gateway_http_run(')]
        with tempfile.TemporaryDirectory() as directory:
            cfile = Path(directory) / 'kit_http.c'
            executable = Path(directory) / 'kit_http'
            cfile.write_text(PREFIX + handler + SUFFIX)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-fsanitize=address,undefined',
                            '-I'+str(APP/'include'), '-I'+str(APP.parent/'wireless_lcd_common/include'),
                            str(cfile), str(APP/'src/http_parse.c'), '-o', str(executable)],check=True,capture_output=True,text=True)
            subprocess.run([str(executable)],check=True,capture_output=True,text=True)

if __name__ == '__main__':
    unittest.main()

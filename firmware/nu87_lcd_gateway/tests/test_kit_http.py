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

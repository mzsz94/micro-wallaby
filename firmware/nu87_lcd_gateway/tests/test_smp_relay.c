/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../src/smp_relay.c"
static struct bt_conn device;
static int writes, pause_calls, disconnects, mode;
static uint8_t request_copy[1024];
static size_t copied;
int gateway_audio_pause(void) { pause_calls++; return 0; }
int bt_conn_disconnect(struct bt_conn *conn,uint8_t reason)
{ ARG_UNUSED(reason);disconnects++;gateway_smp_disconnected(conn);return 0; }
int bt_gatt_write_without_response(struct bt_conn *conn,uint16_t handle,const void *data,uint16_t length,bool sign)
{
 assert(conn==&device&&handle==42&&!sign&&length<=244);writes++;
 memcpy(request_copy+copied,data,length);copied+=length;
 if (mode==2) { gateway_smp_disconnected(conn);return -ENOTCONN; }
 if(copied>=8&&copied==gateway_smp_length(request_copy)&&mode!=1){
  request_copy[0]++;
  if(mode==3){request_copy[6]++;}
  size_t first=MIN(copied,20);
  smp_notification(conn,&smp_subscription,request_copy,first);
  if(copied>first){smp_notification(conn,&smp_subscription,request_copy+first,copied-first);}
 }
 return 0;
}
static void reset(int behavior)
{
 memset(&device,0,sizeof(device));device.refs=1;smp_peer=&device;
 smp_closed=false;smp_ready=smp_live=true;smp_active=false;
 smp_handle=42;copied=0;writes=pause_calls=disconnects=0;mode=behavior;
 mock_att_mtu=247;k_sem_reset(&smp_reply_done);
 mock_security=BT_SECURITY_L3;
}
int main(void)
{
 uint8_t request[300]={2,0,1,36,0,1,123,1},response[1024];
 reset(0);assert(gateway_smp_transaction(request,sizeof(request),response,sizeof(response))==300);
 assert(writes==2&&pause_calls==1&&response[0]==3&&response[6]==123&&device.refs==1&&!smp_active);
 for(int m=1;m<=3;m++){
  reset(m);assert(gateway_smp_transaction(request,sizeof(request),response,sizeof(response))<0);
  assert(disconnects==1&&!smp_ready&&!smp_active&&device.refs==1);
 }
 reset(0);smp_ready=false;assert(gateway_smp_transaction(request,300,response,1024)==-ENOTSUP&&writes==0);
 reset(0);smp_active=true;assert(gateway_smp_transaction(request,300,response,1024)==-EBUSY&&writes==0);
 reset(0);assert(gateway_smp_transaction(request,299,response,1024)==-EINVAL&&writes==0);
 reset(0);mock_security=2;assert(gateway_smp_transaction(request,300,response,1024)==-EACCES&&writes==0&&disconnects==1);
 reset(0);assert(require_authenticated_link(&device)==0);
 mock_security=2;assert(require_authenticated_link(&device)==-ETIMEDOUT);
 struct gateway_smp_rx rx={0};memcpy(rx.request,request,8);
 uint8_t reply[8]={3,0,4,0,0,1,123,1};assert(gateway_smp_receive(&rx,reply,8)==-1);
 reply[2]=0;reply[3]=0;reply[6]=124;assert(gateway_smp_receive(&rx,reply,8)==2);
 reply[6]=123;assert(gateway_smp_receive(&rx,reply,7)==-1);
 assert(gateway_smp_receive(&rx,reply,8)==1);
 reset(0);smp_ready=false;assert(gateway_smp_prepare(&device)==-ENOTSUP);
 assert(!gateway_smp_available());
 puts("PASS SMP relay fragmentation, matching response, timeout/disconnect, bounds and stale sequence");
 return 0;
}

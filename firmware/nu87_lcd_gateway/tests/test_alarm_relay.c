/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../src/alarm_relay.c"
static struct bt_conn device;
static int mode, writes, disconnects;
static bool stopped_for_alert;
void gateway_robot_alert(bool active) { stopped_for_alert = active; }
int bt_conn_disconnect(struct bt_conn *conn,uint8_t reason)
{ ARG_UNUSED(reason);disconnects++;gateway_alarm_disconnected(conn);return 0; }
int bt_gatt_write(struct bt_conn *conn,struct bt_gatt_write_params *params)
{
 writes++;assert(params->handle==42&&params->length==6);
 const uint8_t *p=params->data;
 uint8_t ack[8]={1,p[1],p[2],p[3],p[4],p[5],0,0};
 if(mode==1){ack[2]++;}
 if(mode==2){ack[6]=2;}
 if(mode==3){params->func(conn,0,params);return 0;}
 if(mode==4){gateway_alarm_disconnected(conn);params->func(conn,14,params);return 0;}
 alarm_notification(conn,&alarm_subscription,ack,sizeof(ack));
 if(mode==5){gateway_alarm_disconnected(conn);}
 params->func(conn,0,params);return 0;
}
static void reset(int behavior)
{
 memset(&device,0,sizeof(device));device.refs=1;alarm_peer=&device;
 alarm_closed=false;alarm_ready=alarm_live=true;alarm_pending=alarm_write_active=alarm_received=false;
 alarm_known=false;alarm_control_handle=42;alarm_events=alarm_source=0;
 mode=behavior;writes=disconnects=0;stopped_for_alert=false;
 k_sem_reset(&alarm_write_done);k_sem_reset(&alarm_reply_done);
}
int main(void)
{
 char json[256];reset(0);
 assert(gateway_alarm_set(true)==0&&alarm_active&&alarm_known&&stopped_for_alert&&device.refs==1);
 assert(gateway_alarm_set(false)==0&&!alarm_active&&!stopped_for_alert);
 for(int m=1;m<=4;m++){
  reset(m);assert(gateway_alarm_set(true)<0);assert(!alarm_known&&stopped_for_alert&&device.refs==1);
  if(m!=2){assert(disconnects==1);}
 }
 reset(5);assert(gateway_alarm_set(true)==0&&!alarm_known); /* Confirmed before disconnect. */
 reset(0);alarm_ready=false;assert(gateway_alarm_set(true)==-ENOTSUP&&writes==0&&stopped_for_alert);
 reset(0);uint8_t event[8]={1,1,0,0,0,0,0,1};
 alarm_notification(&device,&alarm_subscription,event,8);
 assert(alarm_events==1&&alarm_source==1&&alarm_known&&alarm_active&&stopped_for_alert);
 event[6]=2;alarm_notification(&device,&alarm_subscription,event,8);
 assert(alarm_events==2&&!alarm_known&&stopped_for_alert); /* Driver failed; detection still stops. */
 event[7]=0;alarm_notification(&device,&alarm_subscription,event,8);assert(alarm_events==2);
 gateway_alarm_disconnected(&device);
 assert(alarm_events==2&&!alarm_known);
 assert(gateway_alarm_status(json,sizeof(json))>0&&strstr(json,"\"active\":null")&&strstr(json,"\"eventCounter\":2"));
 puts("PASS alarm matching output ACK, timeout, errors, BTN4 event latch and disconnect uncertainty");
 return 0;
}

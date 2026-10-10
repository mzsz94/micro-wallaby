/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../src/robot_state.c"
int main(void)
{
 char json[256];
 assert(gateway_robot_command("forward")==0);
 assert(gateway_robot_status(json,sizeof(json),true)>0&&strstr(json,"\"state\":\"moving\"")&&strstr(json,"\"simulated\":true"));
 mock_time+=899;assert(robot_command==3);
 mock_time+=1;gateway_robot_status(json,sizeof(json),false);assert(robot_command==0);
 assert(gateway_robot_command("greet")==0);mock_time+=2999;gateway_robot_status(json,sizeof(json),false);assert(robot_command==2);
 mock_time++;gateway_robot_status(json,sizeof(json),false);assert(robot_command==0);
 gateway_robot_alert(true);assert(gateway_robot_command("forward")==-EPERM);
 assert(gateway_robot_command("dock")==-EPERM&&gateway_robot_command("stop")==0);
 gateway_robot_status(json,sizeof(json),false);assert(strstr(json,"\"state\":\"alert\""));
 gateway_robot_alert(false);assert(gateway_robot_command("left")==0);
 assert(gateway_robot_command("LEFT")==-EINVAL&&gateway_robot_command("attack")==-EINVAL);
 assert(gateway_robot_status(json,1,false)==-ENOSPC);
 puts("PASS NU87 simulated Wallaby motion bounds, allowlist and alert stop interlock");return 0;
}

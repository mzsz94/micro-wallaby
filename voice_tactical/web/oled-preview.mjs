// A host-side drawing of the requested WG1 scene. This is not a camera feed or
// proof of NU40 pixel output; the matching driver ACK is reported separately.
export function drawOledPreview(canvas, scene, now = performance.now()) {
  canvas.hidden = !scene;
  if (!scene) return;
  const c = canvas.getContext('2d');
  c.fillStyle = '#07100b'; c.fillRect(0, 0, 128, 32);
  c.fillStyle = '#cefbdc'; c.strokeStyle = '#cefbdc'; c.lineWidth = 1;
  const rect = (x, y, w, h) => c.fillRect(x, y, w, h);
  const line = (x,y,u,v) => {c.beginPath();c.moveTo(x+.5,y+.5);c.lineTo(u+.5,v+.5);c.stroke();};
  const labels = {L:'LIVE IMU',R:'SET REF',C:'CALIBRATE',P:'SIM PATROL',S:'SIM STOP',A:'TILT STOP',B:'SIM ALERT',X:'LINK STALE',U:'ATT MOVING'};
  const state = scene.state;
  c.save(); c.beginPath(); c.rect(0,0,64,32); c.clip();
  if(state==='A'||state==='B') {line(32,1,3,30);line(32,1,61,30);line(3,30,61,30);rect(30,11,5,10);rect(30,24,5,3);}
  else if(state==='X') {c.strokeRect(17.5,10.5,29,14);rect(23,3,4,7);rect(37,3,4,7);line(10,30,54,1);}
  else if(state==='U') {c.strokeRect(13.5,7.5,37,19);line(18,17,26,11);line(26,11,37,23);line(37,23,46,17);}
  else if(scene.view==='horizon') {
    c.strokeRect(1.5,1.5,61,29);
    const angle=scene.roll*Math.PI/180, y=16+Math.max(-45,Math.min(45,scene.pitch))*10/45;
    line(32-26*Math.cos(angle),y-26*Math.sin(angle),32+26*Math.cos(angle),y+26*Math.sin(angle));
    line(22,16,28,16);line(36,16,42,16);c.strokeRect(31.5,14.5,2,4);
  } else {
    c.strokeRect(7.5,4.5,49,25);rect(12,0,5,4);rect(47,0,5,4);
    const blink=now%3600<140&&state!=='C', dx=Math.max(-5,Math.min(5,Math.trunc(Math.sin(scene.roll*Math.PI/180)*5/.643))),dy=Math.max(-2,Math.min(2,Math.trunc(-scene.pitch/12)));
    for(const x of [14,39]) {rect(x,blink?16:10,11,blink?2:12);if(!blink){c.fillStyle='#07100b';rect(x+4+dx,13+dy,3,5);c.fillStyle='#cefbdc';}}
    line(26,26,38,26);if(state==='P')line(27,27,37,27);
  }
  c.restore();c.font='7px monospace';c.textBaseline='top';
  const signed=v=>(v>=0?'+':'')+v;
  const rows=state==='X'?[labels[state],'NO FRESH','ATTITUDE','CHECK LINK']:[labels[state],`R${signed(scene.roll)}`,`P${signed(scene.pitch)} T${scene.tilt}`,`#${scene.sample%1000000000}`];
  if(state==='B'){rows[1]='TRAINING';rows[2]='ACK/CLEAR';}
  if(state==='U'){rows[1]='NO GRAVITY';rows[2]='HOLD STILL';}
  rows.forEach((text,row)=>c.fillText(text,66,row*8));
}

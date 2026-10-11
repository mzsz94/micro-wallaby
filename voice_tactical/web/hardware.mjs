import { createAttitudeView } from '/simulation/attitude-view.mjs';
const $ = id => document.getElementById(id);
const attitudeView = createAttitudeView();
let latest = null, pending = false, ready = false;
const frame = $('simulator');
function post(type, extra = {}) { frame.contentWindow?.postMessage({ type, ...extra }, location.origin); }
async function api(path, value) {
  const response = await fetch(path, value === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(value) });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || '요청 실패');
  return result;
}
const format = (value, digits = 1) => Number.isFinite(value) ? value.toFixed(digits) : '—';
const colors = ['#aef0bf', '#7ac8ef', '#e9c875'];
function surface(id) {
  const canvas = $(id), rect = canvas.getBoundingClientRect();
  canvas.width = Math.round(rect.width * devicePixelRatio); canvas.height = Math.round(rect.height * devicePixelRatio);
  const ctx = canvas.getContext('2d'); ctx.scale(devicePixelRatio, devicePixelRatio);
  return { ctx, width: rect.width, height: rect.height };
}
function drawAttitude(state, orientation) {
  const { ctx, width: w, height: h } = surface('attitude');
  ctx.strokeStyle = '#2e4739'; ctx.lineWidth = 1;
  for (let i = 1; i < 8; i++) { ctx.beginPath(); ctx.moveTo(0, h*i/8); ctx.lineTo(w, h*i/8); ctx.stroke(); }
  const o = state.fresh && orientation?.gravity_valid ? orientation : null;
  if (!o) { ctx.fillStyle = '#b1c7b8'; ctx.font = '14px sans-serif'; ctx.textAlign = 'center'; ctx.fillText('새 실측 자세 없음', w/2, h/2); return; }
  const roll = o.roll*Math.PI/180, pitch = o.pitch*Math.PI/180;
  // Sensor axes rotate only with measured gravity. The camera is fixed.
  const project = (x,y,z) => {
    const yy = y*Math.cos(roll)-z*Math.sin(roll), zz = y*Math.sin(roll)+z*Math.cos(roll);
    const xx = x*Math.cos(pitch)+zz*Math.sin(pitch), z2 = -x*Math.sin(pitch)+zz*Math.cos(pitch);
    return [w*.27+xx*65+yy*20, h*.6-yy*18-z2*70];
  };
  const face = points => { ctx.beginPath(); points.forEach((p,i) => i ? ctx.lineTo(...project(...p)) : ctx.moveTo(...project(...p))); ctx.closePath(); ctx.fill(); ctx.stroke(); };
  ctx.fillStyle = '#345849'; ctx.strokeStyle = '#9bddb7'; ctx.lineWidth = 2;
  face([[-1,-.65,0],[1,-.65,0],[1,.65,0],[-1,.65,0]]);
  ctx.fillStyle = '#79c49b'; face([[-.45,-.25,.02],[.45,-.25,.02],[.45,.25,.02],[-.45,.25,.02]]);
  for (let i=0; i<3; i++) { const xyz=[0,0,0]; xyz[i]=1.3; ctx.strokeStyle=colors[i]; ctx.beginPath(); ctx.moveTo(...project(0,0,0)); ctx.lineTo(...project(...xyz)); ctx.stroke(); }
  ctx.fillStyle='#a6cbb4'; ctx.font='11px monospace'; ctx.textAlign='center'; ctx.fillText('NU87 + IMU / SENSOR AXES',w*.27,h-15);
  // Concept robot's lean follows sensor roll. No elapsed-time animation.
  ctx.save(); ctx.translate(w*.74,h*.7); ctx.rotate(Math.max(-.7,Math.min(.7,roll)));
  ctx.fillStyle='#334c40'; ctx.fillRect(-55,0,25,32); ctx.fillRect(30,0,25,32);
  ctx.fillStyle=state.tilt_latched?'#bb7457':'#8ac79f'; ctx.beginPath(); ctx.roundRect(-44,-82,88,93,20); ctx.fill();
  ctx.save(); ctx.translate(0,-112); ctx.rotate(Math.max(-.35,Math.min(.35,pitch)));
  ctx.fillStyle='#bfdcc5'; ctx.beginPath(); ctx.roundRect(-55,-37,110,65,22); ctx.fill();
  ctx.fillStyle='#152b21'; ctx.beginPath(); ctx.roundRect(-45,-27,90,43,15); ctx.fill();
  ctx.fillStyle=state.tilt_latched?'#ffa47d':'#a3f3ba'; for (const x of [-23,15]) ctx.fillRect(x,-12,10,state.tilt_latched?4:14);
  ctx.restore(); ctx.restore(); ctx.fillStyle='#a6cbb4'; ctx.textAlign='center'; ctx.fillText('WALLABY / ATTITUDE VIEW',w*.74,h-15);
}
function drawChart(state) {
  const {ctx,width:w,height:h}=surface('chart'), history=state.history.slice(-65);
  ctx.strokeStyle='#3c5143'; ctx.beginPath(); ctx.moveTo(0,h/2); ctx.lineTo(w,h/2); ctx.stroke();
  for(let axis=0;axis<3;axis++) { ctx.beginPath(); history.forEach((p,i)=>{const x=i/64*w,y=h/2-p.accel[axis]/2.2*(h*.45); i?ctx.lineTo(x,y):ctx.moveTo(x,y);}); ctx.strokeStyle=colors[axis]; ctx.lineWidth=1.5; ctx.stroke(); }
}
function render(s) {
  latest=s;
  $('connection').textContent=s.fresh?'NU87 I2C · LIVE':s.enabled?'실측 수신 대기 / 끊김':'실측 모드 미설정'; $('connection').className=`badge ${s.fresh?'live':'stale'}`;
  $('sample').textContent=s.packet?`SEQ ${s.packet.sample}`:'SEQ —'; $('identity').textContent=s.packet?`WHO_AM_I 0x${s.packet.whoAmI.toString(16)}`:'WHO_AM_I —';
  $('age').textContent=s.host_age_ms===null?'새 데이터 없음':`${Math.round(s.host_age_ms)} ms ago`;
  for(const key of ['roll','pitch','tilt']) $(key).textContent=`${format(s.display_orientation?.[key])}°`;
  $('axes').replaceChildren();
  for(let i=0;i<3;i++) for(const key of ['accelMg','gyroMdps']) {const row=document.createElement('div'),axis=document.createElement('b');axis.textContent='XYZ'[i];row.append(axis,document.createTextNode(s.fresh?format(s.packet[key][i]/1000,3):'—'));$('axes').append(row);}
  $('phase').textContent=s.tilt_latched?'기울기 경고 · 이동 정지':s.calibrating?'기준 자세 측정 중…':!s.fresh?'실측 연결 대기':s.reference?'자세 감시 중':'실측 연결됨 · 기준을 설정하세요';
  $('instruction').textContent=s.tilt_latched?'보드를 기준 자세로 돌린 뒤 경고 해제를 누르세요.':s.reference?'천천히 기울여 보세요. 35° 이상이 0.6초 지속되면 정지합니다.':'보드를 내려놓고 ① 기준 자세 설정을 누르세요.';
  $('reference').disabled=!s.fresh||s.calibrating||s.tilt_latched; $('clear').disabled=!s.fresh||!s.tilt_latched||!s.orientation?.gravity_valid||s.orientation.tilt>15;
  $('notice').textContent=s.error||''; $('oled').textContent=s.oled.text||'아직 전송 없음'; $('ack').textContent=s.oled.acknowledged?`드라이버 ACK${s.oled.sample===null?' · 응답':` · #${s.oled.sample}`}`:'ACK 미확인'; $('display').checked=s.display_enabled;
  document.body.classList.toggle('warning',s.tilt_latched);
  const lastReference = [...s.events].reverse().find(e=>e.kind==='reference');
  const moved = lastReference && s.history.some(p=>p.at>=lastReference.at&&p.orientation?.tilt>8);
  const steps = [Boolean(lastReference), Boolean(moved), s.events.some(e=>e.kind==='tilt'), s.events.some(e=>e.kind==='clear')];
  steps.forEach((done,i)=>$(`step${i+1}`).classList.toggle('done',done));
  $('events').replaceChildren();
  for(const event of [...s.events].reverse()) {const row=document.createElement('div'),time=document.createElement('time');time.textContent=new Date(event.at*1000).toLocaleTimeString('ko-KR',{hour12:false});row.append(time,document.createTextNode(`${event.message} · #${event.measurement?.sample??'—'}`));$('events').append(row);}
  if(!s.events.length) $('events').textContent='아직 센서 이벤트가 없습니다.';
  $('patrol').disabled=!s.fresh||!s.reference||s.motion_blocked;
  attitudeView.update(s.fresh ? s.display_orientation || s.orientation : null);
  drawChart(s); post('wallaby-imu',{state:s});
}
async function poll() {
  if(pending) return; pending=true;
  try { render(await api('/api/imu/status')); }
  catch(error) { $('notice').textContent=error.message; post('wallaby-voice-command',{command:'stop'}); $('connection').textContent='HostPC 응답 없음'; if(latest) {latest={...latest,fresh:false};attitudeView.update(null);} }
  finally { pending=false; }
}
async function action(path,value={}) { try {render(await api(path,value));} catch(error) {$('notice').textContent=error.message;} }
$('reference').onclick=()=>action('/api/imu/reference'); $('clear').onclick=()=>action('/api/imu/clear'); $('display').onchange=()=>action('/api/imu/display',{enabled:$('display').checked});
$('patrol').onclick=async()=>{try{const result=await api('/api/turn',{text:'왈라비 훈련 구역 탐색 시작해',mode:'auto'});post('wallaby-voice-command',{command:result.motion||'stop'});$('notice').textContent=result.answer;}catch(error){$('notice').textContent=error.message;}};
$('stop').onclick=()=>{post('wallaby-voice-command',{command:'stop'});action('/api/control/stop');};
$('export').onclick=()=>{if(!latest)return;const blob=new Blob([JSON.stringify({schema:'military-wallaby-live-imu/v1',captured_at:new Date().toISOString(),...latest},null,2)],{type:'application/json'});const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download='wallaby-live-imu.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);};
window.addEventListener('message',event=>{if(event.source!==frame.contentWindow||event.origin!==location.origin)return;if(event.data?.type==='wallaby-voice-ready'){ready=true;$('sim-status').textContent='실측 자세 연동 준비';}if(event.data?.type==='wallaby-voice-result')$('sim-status').textContent=`${event.data.command} · ${event.data.accepted?'수락':'거부'}`;});
setInterval(()=>post(ready&&!document.hidden?'wallaby-voice-heartbeat':'wallaby-voice-hello'),700);
document.addEventListener('visibilitychange',()=>{if(document.hidden)post('wallaby-voice-command',{command:'stop'});});
setInterval(poll,180); poll();
function animate(now) {
  if(latest) drawAttitude(latest, attitudeView.tick(now));
  requestAnimationFrame(animate);
}
requestAnimationFrame(animate);

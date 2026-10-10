import { CONFIG, SCENARIOS, Simulator } from './model.mjs';
import { createVoiceController, isParentMessage } from './voice-control.mjs';
import { arenaLayout } from './render-layout.mjs';
import { createDemoController } from './demo-control.mjs';
import { drawDemoArena, drawDemoRobot } from './demo-render.mjs';

const $ = (id) => document.getElementById(id);
const sim = new Simulator();
let paused = false;
let accumulator = 0;
let previousTime;
let lastUiUpdate = -1;
let feedbackTimer;
const held = new Set();
const history = [];
const embedded = window.parent !== window;
const demoMode = embedded && new URLSearchParams(location.search).get('demo') === '1';
let demo = null;
const alertBanner = document.createElement('div');
alertBanner.className = 'voice-alert-banner';
alertBanner.setAttribute('role', 'alert');
alertBanner.textContent = '⚠ 폭발물 감지 훈련 경고 · 왈라비 정지 · 제어탑에서 경고를 해제하세요.';
alertBanner.hidden = true;
document.body.prepend(alertBanner);
const voice = createVoiceController(sim, {
  canRun: () => !paused && !document.hidden,
  onResult: result => {
    record('voice', result);
    if (!result.accepted) feedback('현재 시뮬레이션 상태에서 음성 동작을 실행할 수 없습니다.');
    if (embedded) window.parent.postMessage({ type: 'wallaby-voice-result', ...result }, location.origin);
  },
});
if (embedded) {
  window.addEventListener('message', event => {
    if (!isParentMessage(event, window.parent, location.origin)) return;
    if (event.data.type === 'wallaby-alert' && typeof event.data.active === 'boolean') {
      setVoiceAlert(event.data.active);
    } else if (event.data.type === 'wallaby-voice-hello') {
      window.parent.postMessage({ type: 'wallaby-voice-ready' }, location.origin);
    } else if (event.data.type === 'wallaby-voice-heartbeat') voice.heartbeat();
    else if (event.data.type === 'wallaby-voice-command') {
      demo?.stop();
      release();
      $('speed').value = '1';
      accumulator = 0;
      voice.command(event.data.command);
      refresh(sim.snapshot());
    }
  });
  window.addEventListener('pagehide', () => { demo?.stop(); voice.stop('pagehide'); });
}
function setVoiceAlert(active) {
  if (active) demo?.stop();
  held.clear();
  voice.setAlert(active);
  alertBanner.hidden = !active;
  document.body.classList.toggle('voice-alert-active', active);
  for (const element of document.querySelectorAll('#dock,#welcome,#clear-fault,#reset,#scenario,[data-drive],#headYaw,#headPitch,#waistYaw')) element.disabled = active;
  refresh(sim.snapshot());
  if (embedded) window.parent.postMessage({ type: 'wallaby-alert-state', active: voice.alertActive }, location.origin);
}
const flagNames = {
  estop: 'E-stop', bumper: '범퍼 접촉', cliff: '낭떠러지',
  tilt: 'IMU 기울어짐 확정', beaconLoss: '도크 신호 유실',
  chargerFailure: '충전 실패', networkLoss: '통신 단절',
};
const stateLabels = {
  IDLE: '대기', MANUAL: '수동 주행', WELCOME: '환영 동작',
  DOCK_SEARCH: '도크 탐색', DOCK_APPROACH: '도크 접근', DOCK_ALIGN: '최종 정렬',
  DOCK_VERIFY: '충전 확인 · 5초', DOCK_RETRY: '후퇴 후 재시도',
  CHARGING: '충전 중', DOCKED_READY: '충전 완료', DOCKED_WELCOME: '도킹 중 환영',
  DOCK_FAILED: '도킹 실패', FAULT: '안전 정지',
};

function feedback(message) {
  $('feedback').textContent = message;
  $('feedback').classList.add('visible');
  clearTimeout(feedbackTimer);
  feedbackTimer = setTimeout(() => $('feedback').classList.remove('visible'), 3500);
}

function record(action, value) {
  history.push({ time: sim.snapshot().time, action, value });
}

function invoke(action) {
  if (voice.alertActive && action !== 'stop') return feedback('폭발물 감지 훈련 경고를 먼저 해제하세요.');
  voice.interrupt('manual override');
  release();
  const accepted = sim.command(action);
  record('command', action);
  if (!accepted) feedback('현재 상태에서는 실행할 수 없습니다. 정지 원인·접점·배터리를 확인하세요.');
  refresh(sim.snapshot());
}

for (const scenario of SCENARIOS) {
  const option = document.createElement('option');
  option.value = scenario.id;
  option.textContent = scenario.label;
  $('scenario').append(option);
}
for (const [name, label] of Object.entries(flagNames)) {
  const element = document.createElement('label');
  const input = document.createElement('input');
  input.type = 'checkbox';
  input.id = `fault-${name}`;
  input.addEventListener('change', () => {
    sim.setFault(name, input.checked);
    record('fault', { name, enabled: input.checked });
    refresh(sim.snapshot());
  });
  element.append(input, document.createTextNode(label));
  $('fault-controls').append(element);
}

function reset() {
  if (voice.alertActive) return;
  demo?.stop();
  voice.interrupt('reset');
  held.clear();
  sim.reset($('scenario').value);
  history.length = 0;
  record('reset', $('scenario').value);
  accumulator = 0;
  lastUiUpdate = -1;
  paused = false;
  $('pause').textContent = '일시정지';
  for (const button of document.querySelectorAll('[data-drive]')) button.classList.remove('active');
  $('scenario-description').textContent = SCENARIOS.find(s => s.id === $('scenario').value)?.description || '';
  refresh(sim.snapshot());
}
$('scenario').addEventListener('change', reset);
$('reset').addEventListener('click', reset);
$('pause').addEventListener('click', () => {
  demo?.stop();
  voice.interrupt('pause');
  release();
  paused = !paused;
  accumulator = 0;
  $('pause').textContent = paused ? '계속 재생' : '일시정지';
  record('pause', paused);
});
for (const name of ['dock', 'welcome', 'stop']) $(name).addEventListener('click', () => invoke(name));
$('clear-fault').addEventListener('click', () => invoke('clearFault'));
for (const name of ['headYaw', 'headPitch', 'waistYaw']) {
  const [minimum, maximum] = CONFIG.jointLimits[name];
  $(name).min = minimum;
  $(name).max = maximum;
  $(name).addEventListener('input', () => {
    if (voice.alertActive) return refresh(sim.snapshot());
    sim.setJoint(name, Number($(name).value));
    record('joint', { name, degrees: Number($(name).value) });
    refresh(sim.snapshot());
  });
}
$('battery').addEventListener('input', () => {
  sim.setBattery(Number($('battery').value));
  record('battery', Number($('battery').value));
  refresh(sim.snapshot());
});

function updateManual() {
  if (voice.alertActive) { held.clear(); sim.command('stop'); return; }
  const linear = (Number(held.has('forward')) - Number(held.has('backward'))) * .12;
  const angular = (Number(held.has('left')) - Number(held.has('right'))) * 1.0;
  const accepted = sim.setManual(linear, angular);
  if (!accepted && (linear || angular)) {
    held.clear();
    feedback('먼저 정지를 누르세요. 오류·도크 접촉 상태에서는 수동 주행할 수 없습니다.');
  }
  record('manual', { linear, angular });
  for (const button of document.querySelectorAll('[data-drive]')) {
    button.classList.toggle('active', held.has(button.dataset.drive));
  }
}
function release() {
  if (!held.size) return;
  held.clear();
  updateManual();
}
for (const button of document.querySelectorAll('[data-drive]')) {
  const direction = button.dataset.drive;
  button.addEventListener('pointerdown', event => {
    event.preventDefault();
    if (paused) return feedback('계속 재생을 누른 뒤 주행하세요.');
    if (voice.alertActive) return;
    voice.interrupt('manual override');
    button.setPointerCapture(event.pointerId);
    held.add(direction);
    updateManual();
  });
  const stop = () => {
    if (held.delete(direction)) updateManual();
  };
  button.addEventListener('pointerup', stop);
  button.addEventListener('pointercancel', stop);
  button.addEventListener('lostpointercapture', stop);
}
const keys = { w: 'forward', ArrowUp: 'forward', s: 'backward', ArrowDown: 'backward', a: 'left', ArrowLeft: 'left', d: 'right', ArrowRight: 'right' };
window.addEventListener('keydown', event => {
  if (event.code === 'Space') {
    event.preventDefault();
    if (!event.repeat) invoke('stop');
    return;
  }
  if (event.target.matches('input,select,textarea,button')) return;
  const direction = keys[event.key] || keys[event.key.toLowerCase()];
  if (!direction) return;
  event.preventDefault();
  if (!paused && !event.repeat && !voice.alertActive) { voice.interrupt('manual override'); held.add(direction); updateManual(); }
});
window.addEventListener('keyup', event => {
  const direction = keys[event.key] || keys[event.key.toLowerCase()];
  if (direction && held.delete(direction)) updateManual();
});
window.addEventListener('blur', () => { demo?.stop(); release(); if (embedded) voice.stop('blur'); });
document.addEventListener('visibilitychange', () => {
  if (document.hidden) demo?.stop();
  if (embedded && document.hidden) voice.stop('hidden');
  release();
  accumulator = 0;
  previousTime = undefined;
});

$('export').addEventListener('click', () => {
  const payload = {
    schema: 'micro-wallaby-kinematic-sim/v1',
    exportedAt: new Date().toISOString(),
    modelBoundary: 'Ideal planar kinematics, virtual sensors and charger; not firmware or hardware validation.',
    scenario: $('scenario').value, config: CONFIG, actions: history, snapshot: sim.snapshot(),
  };
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = `micro-wallaby-${$('scenario').value}-${new Date().toISOString().replaceAll(':', '-')}.json`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  feedback('시나리오·설정·조작·결과를 JSON으로 저장했습니다.');
});

function refresh(s) {
  const patrol = voice.patrolState;
  $('time').textContent = s.time.toFixed(2);
  $('state').textContent = patrol ? 'TRAINING_SEARCH' : s.state;
  $('state').classList.toggle('fault', ['FAULT', 'DOCK_FAILED'].includes(s.state));
  $('reason').textContent = patrol ? `훈련 구역 탐색 · 지점 ${patrol.index + 1}/4 · 가상 경로 순회 / 최대 60초` : `${stateLabels[s.state] || ''} · ${s.reason || '명령 대기'}`;
  for (const [id, value, unit, places] of [
    ['linear', s.velocity.linear, 'm/s', 3], ['angular', s.velocity.angular, 'rad/s', 2],
    ['left', s.velocity.left / CONFIG.wheelRadius, 'rad/s', 2], ['right', s.velocity.right / CONFIG.wheelRadius, 'rad/s', 2],
  ]) {
    $(id).replaceChildren(document.createTextNode(`${value.toFixed(places)} `));
    const small = document.createElement('small'); small.textContent = unit; $(id).append(small);
  }
  $('tof').textContent = Number.isFinite(s.sensors.tof) ? `${(s.sensors.tof * 1000).toFixed(0)} mm` : '범위 밖';
  $('beacon').textContent = s.sensors.beacon ? `감지 · ${(s.sensors.bearing * 180 / Math.PI).toFixed(1)}°` : '미감지';
  $('contact').textContent = `${s.sensors.contact ? '접촉' : '없음'} / ${s.sensors.charging ? '충전' : '대기'}`;
  $('attempt').textContent = `${s.attempt} / 2`;
  $('battery-value').textContent = `${s.battery.toFixed(1)}%`;
  if (document.activeElement !== $('battery')) $('battery').value = s.battery;
  $('pose').textContent = `X ${s.pose.x.toFixed(2)} m   Y ${s.pose.y.toFixed(2)} m   θ ${(s.pose.theta * 180 / Math.PI).toFixed(1)}°`;
  for (const name of ['headYaw', 'headPitch', 'waistYaw']) {
    $(`${name}-value`).textContent = `${s.joints[name].toFixed(0)}°`;
    if (document.activeElement !== $(name)) $(name).value = s.joints[name];
    $(name).disabled = !['IDLE', 'MANUAL'].includes(s.state);
  }
  for (const name of Object.keys(flagNames)) $(`fault-${name}`).checked = !!s.faults[name];
  $('events').replaceChildren();
  for (const entry of s.events.slice(-35).reverse()) {
    const li = document.createElement('li');
    const time = document.createElement('time'); time.textContent = `${entry.time.toFixed(2)}s`;
    const message = document.createElement('span'); message.textContent = entry.message;
    li.append(time, message); $('events').append(li);
  }
}

function surface(id) {
  const canvas = $(id);
  const { width, height } = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  return { ctx, width, height };
}

function arena(s) {
  if (demoMode) return drawDemoArena(surface('arena'), demo.snapshot());
  const { ctx, width, height } = surface('arena');
  const layout = arenaLayout(width, height, s.world);
  if (!layout) return;
  const { scale, offsetX, offsetY } = layout;
  const xy = (x, y) => [offsetX + x * scale, offsetY + (s.world.height - y) * scale];
  ctx.strokeStyle = '#293739'; ctx.lineWidth = 1;
  for (let x = 0; x <= s.world.width + 1e-9; x += .2) { ctx.beginPath(); ctx.moveTo(...xy(x, 0)); ctx.lineTo(...xy(x, s.world.height)); ctx.stroke(); }
  for (let y = 0; y <= s.world.height + 1e-9; y += .2) { ctx.beginPath(); ctx.moveTo(...xy(0, y)); ctx.lineTo(...xy(s.world.width, y)); ctx.stroke(); }
  ctx.strokeStyle = '#536461'; ctx.strokeRect(offsetX, offsetY, s.world.width * scale, s.world.height * scale);
  ctx.font = '9px monospace'; ctx.fillStyle = '#738882';
  ctx.fillText('Y ↑', offsetX - 1, offsetY - 9); ctx.fillText('X →', offsetX + s.world.width * scale - 26, offsetY + s.world.height * scale + 15);
  ctx.fillText('0.2 m / grid', offsetX, offsetY + s.world.height * scale + 15);
  for (const item of s.world.obstacles) {
    ctx.beginPath(); ctx.arc(...xy(item.x, item.y), item.r * scale, 0, Math.PI * 2);
    ctx.fillStyle = '#405151'; ctx.fill(); ctx.strokeStyle = '#718683'; ctx.stroke();
  }
  const dock = xy(s.world.dock.x, s.world.dock.y);
  ctx.save(); ctx.translate(...dock); ctx.rotate(-s.world.dock.theta);
  ctx.fillStyle = '#a18353'; ctx.strokeStyle = '#e7bf7a'; ctx.lineWidth = 2;
  ctx.fillRect(0, -CONFIG.dockWidth * scale / 2, CONFIG.dockDepth * scale, CONFIG.dockWidth * scale);
  ctx.beginPath(); ctx.moveTo(-.11 * scale, -.22 * scale); ctx.lineTo(0, -.14 * scale); ctx.lineTo(0, .14 * scale); ctx.lineTo(-.11 * scale, .22 * scale); ctx.stroke(); ctx.restore();
  ctx.font = '10px monospace'; ctx.fillStyle = '#e7bf7a'; ctx.fillText('DOCK', dock[0] - 28, dock[1] - .28 * scale);
  const patrol = voice.patrolState;
  if (patrol) {
    ctx.save(); ctx.beginPath();
    patrol.route.forEach((point, index) => index ? ctx.lineTo(...xy(point.x, point.y)) : ctx.moveTo(...xy(point.x, point.y)));
    ctx.closePath(); ctx.setLineDash([5, 5]); ctx.strokeStyle = '#85d9eb'; ctx.lineWidth = 1.5; ctx.stroke();
    ctx.setLineDash([]); ctx.font = '11px monospace';
    patrol.route.forEach((point, index) => {
      const position = xy(point.x, point.y);
      ctx.fillStyle = index === patrol.index ? '#85d9eb' : '#536461';
      ctx.beginPath(); ctx.arc(...position, index === patrol.index ? 5 : 3, 0, Math.PI * 2); ctx.fill();
      ctx.fillText(String(index + 1), position[0] + 8, position[1] - 8);
    });
    ctx.restore();
  }
  if (s.trail.length > 1) {
    ctx.beginPath(); s.trail.forEach((p, i) => i ? ctx.lineTo(...xy(p.x, p.y)) : ctx.moveTo(...xy(p.x, p.y)));
    ctx.strokeStyle = '#8bd6b878'; ctx.lineWidth = 2; ctx.stroke();
  }
  const position = xy(s.pose.x, s.pose.y);
  if (s.sensors.beacon) { ctx.beginPath(); ctx.moveTo(...position); ctx.lineTo(...dock); ctx.setLineDash([3, 6]); ctx.strokeStyle = '#b9a27566'; ctx.lineWidth = 1; ctx.stroke(); ctx.setLineDash([]); }
  ctx.save(); ctx.translate(...position); ctx.rotate(-s.pose.theta);
  const radius = CONFIG.radius * scale;
  ctx.beginPath(); ctx.arc(0, 0, radius + 6, -.55, .55); ctx.strokeStyle = '#9ee2c566'; ctx.lineWidth = 2; ctx.stroke();
  ctx.fillStyle = '#0b1415'; ctx.strokeStyle = '#748e84';
  for (const side of [-1, 1]) { ctx.fillRect(-radius * .65, side * radius * .85 - 3, radius * 1.3, 6); ctx.strokeRect(-radius * .65, side * radius * .85 - 3, radius * 1.3, 6); }
  ctx.beginPath(); ctx.arc(0, 0, radius, 0, Math.PI * 2); ctx.fillStyle = s.state === 'FAULT' ? '#bb7063' : '#b4d8c6'; ctx.fill();
  ctx.beginPath(); ctx.moveTo(radius * .65, 0); ctx.lineTo(-radius * .25, -radius * .4); ctx.lineTo(-radius * .25, radius * .4); ctx.closePath(); ctx.fillStyle = '#294a3e'; ctx.fill();
  ctx.restore();
  ctx.fillStyle = '#cce4da'; ctx.font = '9px monospace'; ctx.fillText('WALLABY', position[0] - 22, position[1] + radius + 20);
}

// A light, dependency-free articulated concept view. Geometry is illustrative.
function robot(s) {
  if (demoMode) return drawDemoRobot(surface('robot'), demo.snapshot());
  const { ctx, width, height } = surface('robot');
  const unit = Math.min(width / 3.1, height / 3.45);
  const radians = degrees => degrees * Math.PI / 180;
  const yaw = radians(s.joints.waistYaw + s.joints.headYaw);
  const pitch = radians(s.joints.headPitch);
  const project = ([x, y, z]) => [width / 2 + unit * (x + .18 * y), height * .77 + unit * (.19 * y - z)];
  function transformed(point, rotation, center) {
    const [x, y, z] = point;
    const py = y * Math.cos(rotation[1]) - z * Math.sin(rotation[1]);
    const pz = y * Math.sin(rotation[1]) + z * Math.cos(rotation[1]);
    return [x * Math.cos(rotation[0]) - py * Math.sin(rotation[0]) + center[0], x * Math.sin(rotation[0]) + py * Math.cos(rotation[0]) + center[1], pz + center[2]];
  }
  function poly(points, color, rotation = [0, 0], center = [0, 0, 0]) {
    ctx.beginPath(); points.forEach((point, i) => { const p = project(transformed(point, rotation, center)); i ? ctx.lineTo(...p) : ctx.moveTo(...p); });
    ctx.closePath(); ctx.fillStyle = color; ctx.fill(); ctx.strokeStyle = '#182a25'; ctx.lineWidth = 1; ctx.stroke();
  }
  function box(w, d, h, color, rotation, center) {
    poly([[-w/2,-d/2,0],[w/2,-d/2,0],[w/2,-d/2,h],[-w/2,-d/2,h]], color, rotation, center);
    poly([[w/2,-d/2,0],[w/2,d/2,0],[w/2,d/2,h],[w/2,-d/2,h]], '#829c8f', rotation, center);
    poly([[-w/2,-d/2,h],[w/2,-d/2,h],[w/2,d/2,h],[-w/2,d/2,h]], '#c8dfd0', rotation, center);
  }
  ctx.beginPath(); ctx.ellipse(width/2, height*.79, unit*.85, unit*.16, 0, 0, Math.PI*2); ctx.fillStyle='#0d161766'; ctx.fill();
  for (const side of [-1, 1]) {
    box(.18,.75,.34,'#344541',[0,0],[side*.6,0,0]);
    const angle = side < 0 ? s.wheelAngles.left : s.wheelAngles.right;
    const p=project([side*.6,-.4,.17]); ctx.strokeStyle='#6d857a'; ctx.beginPath();ctx.moveTo(p[0],p[1]);ctx.lineTo(p[0]+Math.sin(angle)*5,p[1]+Math.cos(angle)*8);ctx.stroke();
  }
  box(1.0,.6,.57,'#a6bcad',[radians(s.joints.waistYaw),0],[0,0,.15]);
  box(.18,.2,.2,'#536e60',[yaw,0],[0,0,.71]);
  const center=[0,0,.85];
  const rotation=[yaw,pitch];
  box(1.36,.32,.83,'#c9daca',rotation,center);
  for(const side of [-1,1]) {
    poly([[side*.40,-.14,.8],[side*.61,-.14,.83],[side*.66,-.12,1.45],[side*.50,-.14,1.55],[side*.38,-.14,1.1]],'#c9daca',rotation,center);
    poly([[side*.46,-.17,.9],[side*.55,-.17,.94],[side*.58,-.17,1.37],[side*.51,-.17,1.41]],'#b0b2a0',rotation,center);
  }
  poly([[-.55,-.17,.29],[.55,-.17,.29],[.55,-.17,.65],[-.55,-.17,.65]],'#426455',rotation,center);
  for(const side of [-1,1]) {
    const blink=Math.floor(s.time*2)%13===0;
    for(let row=0;row<(blink?1:3);row++)for(let col=0;col<3;col++) {
      const x=side*.28+(col-1)*.04,z=.43+row*.04;
      poly([[x,-.185,z],[x+.029,-.185,z],[x+.029,-.185,z+.029],[x,-.185,z+.029]],'#b2d691',rotation,center);
    }
  }
  poly([[-.055,-.18,.19],[.055,-.18,.19],[0,-.18,.12]],'#5d7363',rotation,center);
  ctx.font='9px monospace';ctx.fillStyle='#829b91';ctx.fillText('CONCEPT / NOT TO SCALE',width/2-66,height-10);
}

function frame(now) {
  voice.tick();
  demo?.tick();
  if (previousTime !== undefined && !paused && !document.hidden) {
    accumulator += Math.min((now - previousTime) / 1000, .1) * Number($('speed').value);
    while (accumulator >= CONFIG.dt) { sim.step(CONFIG.dt); accumulator -= CONFIG.dt; }
  }
  previousTime = now;
  const snapshot = sim.snapshot();
  arena(snapshot); robot(snapshot);
  if (now - lastUiUpdate >= 80) { refresh(snapshot); lastUiUpdate = now; }
  requestAnimationFrame(frame);
}
if (demoMode) {
  demo = createDemoController(sim, voice, { canRun: () => !paused && !document.hidden, resetModel: reset });
  window.wallabyDemo = Object.freeze({ apply: demo.apply, snapshot: demo.snapshot, reset: demo.reset });
  document.body.classList.add('wallaby-demo-mode');
  document.querySelector('.arena-panel .panel-title span').textContent = '실제 모델 궤적 · AUTO ZOOM';
  document.querySelector('.robot-panel .panel-title h3').textContent = 'Wallaby · 표정과 움직임';
  document.querySelector('.robot-panel .panel-title span').textContent = 'LIVE RESPONSE';
  document.querySelector('.robot-panel .caption').textContent = '음성·상황별 표정과 관절 반응 · 가상 시연';
}
reset();
if (embedded) window.parent.postMessage({ type: 'wallaby-voice-ready' }, location.origin);
requestAnimationFrame(frame);

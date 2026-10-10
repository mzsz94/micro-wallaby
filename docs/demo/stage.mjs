// Presentation only: every response below comes from the unchanged local API.
import { choreography, WARNING_AT, CLEAR_AT, FOUND_AT, MOCK_TARGET } from './choreography.mjs';
const $ = id => document.getElementById(id);
export const scenes = [
  {id:'01-manual-query',title:'문서를 펼칠 수 없을 때, 말로 묻다.',tag:'01 / MANUAL QUERY',
    input:'왈라비, 상황보고에는 어떤 항목을 포함해야 해?',kind:'query',
    steps:['질문 텍스트','로컬 자료 검색','근거와 함께 확인']},
  {id:'02-situation-report',title:'지나가는 상황 보고를, 남는 기록으로.',tag:'02 / SITUATION REPORT',
    input:'상황 보고. 보고자: 지원팀. 시간: 14시 20분. 위치: 동쪽 집결지. 인원: 4명. 관측: 장비 점검 완료. 조치: 대기 중. 요청: 추가 안내.',kind:'report',
    steps:['보고 텍스트','항목별 구조화','SQLite 저장 확인']},
  {id:'03-organize-orders',title:'전달받은 지시를, 확인 가능한 목록으로.',tag:'03 / ORGANIZE ORDERS',
    input:'지시 정리: 지원팀은 16시까지 집결지에서 장비를 점검하세요. 통신팀은 대기소에서 배터리를 확인하세요.',kind:'orders',
    steps:['전달받은 지시','담당·기한 추출','미확인 항목 확인']},
  {id:'04-warning-stop',title:'위험 경고에는, 움직임보다 정지가 먼저.',tag:'04 / WARNING & STOP',
    input:'왈라비 앞으로 가. → 왈라비 폭발물 감지.',kind:'alert',
    steps:['직접 이동 명령','훈련 경고 · 정지','해제 후에도 대기']},
  {id:'05-search-and-notify',title:'탐색 중 발견한 위험을, 컨트롤 타워로.',tag:'05 / SEARCH & NOTIFY',
    input:'왈라비, 훈련 구역 탐색을 시작해. 모의 폭발물을 발견하면 정지하고 컨트롤 타워에 알려 줘.',kind:'search',
    steps:['훈련 구역 탐색','모사 감지 · 즉시 정지','컨트롤 타워 수신 확인']},
];
const compactCss = `
html,body{margin:0!important;background:#18241f!important;overflow:hidden!important}
body>header,body>footer,main>.intro,.toolbar,.lower-grid,.model-notes,.feedback,.panel-title,.joint-controls,.caption,.note,.arena-footer,.map-key,.status-panel{display:none!important}
main{padding:0!important;width:100%!important;max-width:none!important}
.workspace{display:flex!important;flex-direction:column!important;gap:0!important}
.panel{border:0!important;border-radius:0!important;background:transparent!important;overflow:visible!important}
.robot-panel{order:0!important}.arena-panel{order:1!important}.robot-panel>canvas{height:166px!important;width:100%!important}
.arena-wrap{padding:0 8px!important}.arena-wrap canvas{height:162px!important;width:100%!important}
.voice-alert-banner{display:none!important}
`;
let scene, response, fired = new Set(), results = [], ready = false;
const frame = $('robot-frame');
const post = value => frame.contentWindow.postMessage(value, location.origin);
const motion = command => post({type:'wallaby-voice-command',command});
const alert = active => new Promise(resolve=>{
  const acknowledge=event=>{
    if(event.source!==frame.contentWindow||event.origin!==location.origin||event.data.type!=='wallaby-alert-state'||event.data.active!==active)return;
    window.removeEventListener('message',acknowledge);resolve();
  };
  window.addEventListener('message',acknowledge);post({type:'wallaby-alert',active});
});
window.addEventListener('message', event => {
  if(event.source === frame.contentWindow && event.origin === location.origin && event.data.type === 'wallaby-voice-ready') ready = true;
});
setInterval(()=>post({type:ready?'wallaby-voice-heartbeat':'wallaby-voice-hello'}),150);
frame.addEventListener('load',()=>{
  const style = frame.contentDocument.createElement('style');
  style.textContent=compactCss;frame.contentDocument.head.append(style);
  post({type:'wallaby-voice-hello'});
});
for(let n=0;n<22;n++)$('wave').append(document.createElement('i'));
function element(tag,text,className){const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(className)node.className=className;return node;}
async function turn(text){
  const reply=await fetch('/api/turn',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text,mode:'auto'})});
  if(!reply.ok)throw new Error(`Local turn failed: ${reply.status}`);
  const result=await reply.json();results.push(result);return result;
}
function queryResult(result){
  const source=result.sources.find(source=>source.id==='training-report');
  if(!source)throw new Error('Expected a real training-report source.');
  const quote=source.excerpt.split(/(?<=\.)\s+/).find(sentence=>sentence.includes('상황보고는'));
  if(!quote)throw new Error('Expected verbatim report guidance.');
  $('result').append(element('p',quote,'answer'),element('span',`출처 · ${source.title}`,'source'));
  $('note').textContent='개발용 훈련 자료를 검색한 결과입니다. 공식 교범은 아닙니다.';
  $('result-badge').textContent='로컬 근거 확인';
}
function reportResult(result,t){
  const data=result.report;
  const fields=[['시각',data.salute.time],['위치',data.salute.location],['규모',data.salute.size],['관측',data.salute.activity],['조치',data.situation.action],['요청',data.situation.request]];
  const grid=element('div',undefined,'fields');
  fields.forEach(([label,value],i)=>{const card=element('div',undefined,'field');card.append(element('small',label),element('strong',value));if(t<3.8+i*.35)card.classList.add('dim');grid.append(card);});
  $('result').append(grid);$('note').textContent='소속·장비는 발화에 없어 미확인으로 유지합니다.';
  $('result-badge').textContent=t>=7?'원문 + 초안 저장 확인':'SALUTE / 상황 보고 초안';
}
function ordersResult(result){
  if(result.orders.length!==2||!result.orders[1].when.includes('미확인'))throw new Error('Expected two orders and a missing second deadline.');
  for(const [i,item] of result.orders.entries()){
    const card=element('div',undefined,'order');
    const when=item.when.includes('미확인')?'기한 미확인 · 재확인 필요':item.when;
    const title=element('b',`${i+1}. ${item.who} / ${when}`,item.when.includes('미확인')?'missing':'');
    card.append(title,element('p',item.evidence));$('result').append(card);
  }
  $('note').textContent='지시는 기록만 합니다. 화면의 이동·표정은 장면 연출이며 지시 실행이 아닙니다.';
  $('result-badge').textContent='담당 · 기한 · 원문 대조';
}
async function once(name,fn){if(!fired.has(name)){fired.add(name);await fn();}}
async function load(index){
  scene=scenes[index];response=null;results=[];fired=new Set();
  document.body.classList.remove('alert-mode');await alert(false);motion('stop');
  frame.contentDocument.querySelector('#reset').click();
  frame.contentWindow.wallabyDemo?.reset();
  $('title').textContent=scene.title;$('eyebrow').textContent=scene.tag;$('scene-number').textContent=`${String(index+1).padStart(2,'0')} / ${String(scenes.length).padStart(2,'0')}`;
  scene.steps.forEach((step,i)=>$('step-'+i).textContent=step);
  return {ready,scene:scene.id};
}
async function tick(t){
  const speaking=t<2.8;
  $('input-label').textContent=speaking?'발화 자막 · 입력 연출':'입력 원문 · 텍스트 처리';
  [...$('wave').children].forEach((bar,i)=>bar.style.height=`${speaking?3+12*Math.abs(Math.sin(t*7+i*.9))*Math.abs(Math.cos(i*.35)):3}px`);
  const input=scene.kind==='alert'?(t<WARNING_AT?'왈라비 앞으로 가. (짧은 전진 명령 반복)':t<CLEAR_AT?'왈라비 폭발물 감지.':'왈라비 경고 해제.'):scene.input;
  $('transcript').textContent=scene.kind==='alert'?input:input.slice(0,Math.ceil(Math.min(1,(t+.2)/2.6)*input.length));
  $('result').replaceChildren();$('note').textContent='';$('result-badge').textContent='';
  $('result-label').textContent=t<2.8?'로컬 처리 대기':t<3.6?'로컬 자료·원문 확인 중':'왈라비 응답 · 실제 API 결과';
  if(scene.kind==='alert'){
    if(t<WARNING_AT)await once('forward-'+Math.floor(t/.75),async()=>{response=await turn('왈라비 앞으로 가');if(response.motion!=='forward')throw new Error('Forward command not classified.');motion(response.motion);});
    if(t>=.5)await once('moving',async()=>{
      const velocity=frame.contentDocument.querySelector('#linear').textContent;
      if(!Number.parseFloat(velocity))throw new Error('Expected simulated movement before warning.');
    });
    if(t>=WARNING_AT)await once('warning',async()=>{response=await turn('왈라비 폭발물 감지');if(response.alert?.active!==true)throw new Error('Warning not latched.');await alert(true);motion('stop');});
    if(t>=WARNING_AT+.25)await once('stop-verified',async()=>{if(Number.parseFloat(frame.contentDocument.querySelector('#linear').textContent)!==0)throw new Error('Robot did not stop.');});
    if(t>=5.5)await once('block',async()=>{const blocked=await turn('왈라비 앞으로 가');if(!blocked.blocked_by_alert)throw new Error('Motion was not blocked by warning.');});
    if(t>=CLEAR_AT)await once('clear',async()=>{response=await turn('왈라비 경고 해제');if(response.alert?.active!==false)throw new Error('Warning did not clear.');await alert(false);motion('stop');});
    if(t<WARNING_AT)$('result').append(element('p','직접 명령 → 전진 · 이동 궤적 기록','answer'));
    else if(t<CLEAR_AT){document.body.classList.add('alert-mode');const card=element('div','훈련 경고 · 왈라비 정지','alarm');card.append(element('small','이동 명령 차단 / 정지 상태 유지'));$('result').append(card,element('p',t>=5.5?'추가 전진 명령도 경고가 해제될 때까지 거절됩니다.':'경고를 명시적으로 해제하기 전까지 이동하지 않습니다.','alarm-note'));$('note').textContent='감지 이벤트는 훈련용 입력입니다. 실제 폭발물 탐지 센서는 없습니다.';}
    else{document.body.classList.remove('alert-mode');const card=element('div','경고 해제 · 대기','alarm safe');card.append(element('small','새 명령 전까지 정지 상태 유지'));$('result').append(card,element('p','경고를 해제해도 이전 이동 명령을 다시 실행하지 않습니다.','alarm-note'));$('note').textContent='실물 모터·스피커를 사용하지 않은 로컬 Motion Lab 검증입니다.';}
    $('result-badge').textContent=t<WARNING_AT?'직접 명령':t<CLEAR_AT?'STOP / INTERLOCK':'IDLE / 새 명령 대기';
  }else if(scene.kind==='search'){
    if(t>=FOUND_AT)await once('found',async()=>{
      const state=frame.contentWindow.wallabyDemo.snapshot();
      if(Math.hypot(state.pose.x-MOCK_TARGET.x,state.pose.y-MOCK_TARGET.y)>.25)throw new Error('Search did not reach the simulated target.');
      const warning=await turn('왈라비 폭발물 감지');
      if(warning.alert?.active!==true)throw new Error('Search warning not latched.');
      await alert(true);motion('stop');
      response=await turn(`상황 보고. 보고자: 왈라비. 시간: 14시 25분. 위치: 훈련 격자 X ${MOCK_TARGET.x.toFixed(2)} Y ${MOCK_TARGET.y.toFixed(2)}. 관측: 모의 폭발물 표식 발견. 조치: 즉시 정지 및 경고 유지. 요청: 컨트롤 타워 확인.`);
      if(response.kind!=='report')throw new Error('Discovery report not structured.');
    });
    if(t>=7)await once('tower-received',async()=>{
      const records=await(await fetch('/api/records')).json();
      if(!records.records.some(record=>record.id===response.id))throw new Error('Control tower did not receive the discovery report.');
    });
    if(t<FOUND_AT){
      const card=element('div','훈련 구역 탐색 중','alarm safe');card.append(element('small','곡선 주행 · 좌우 시선 탐색'));$('result').append(card,element('p','가상 표식에 접근하면 모사 감지 이벤트를 발생시킵니다.','alarm-note'));
      $('result-badge').textContent='SEARCH / 시뮬레이션';
    }else{
      document.body.classList.add('alert-mode');
      const card=element('div','모의 폭발물 발견 · 즉시 정지','alarm');card.append(element('small',`발견 위치 · X ${MOCK_TARGET.x.toFixed(2)} m / Y ${MOCK_TARGET.y.toFixed(2)} m`));$('result').append(card);
      const tower=element('div',undefined,'tower');tower.append(element('b',t<7?'WALLABY → 컨트롤 타워 · 보고 전달':'컨트롤 타워 · 경고 / 위치 수신 확인'),element('p',t<7?'발견 위치·정지 상태를 로컬 보고 API로 전달합니다.':'발견 위치·정지 상태 저장 확인 · 경고 유지 / 추가 지시 대기'));$('result').append(tower);
      $('result-badge').textContent=t<7?'ALERT / 보고 전달':'RECEIVED / 정지 유지';
    }
    $('note').textContent='가상 표식·모사 감지 · 컨트롤 타워는 로컬 서버이며 외부 전송은 없습니다.';
  }else{
    if(t>=2.8)await once('response',async()=>{response=await turn(scene.input);if(response.kind!==scene.kind)throw new Error(`Unexpected result: ${response.kind}`);});
    if(t>=7&&scene.kind==='report')await once('saved',async()=>{const records=await(await fetch('/api/records')).json();if(!records.records.some(record=>record.id===response.id))throw new Error('Report was not stored.');});
    if(t<3.6)$('result').append(element('p',t<2.8?'발화 내용을 텍스트로 전달합니다.':'외부 AI API 없이 로컬에서 처리합니다.','waiting'));
    else if(scene.kind==='query')queryResult(response);
    else if(scene.kind==='report')reportResult(response,t);
    else ordersResult(response);
  }
  const presentation=choreography(scene.kind,t);
  const robot=frame.contentWindow.wallabyDemo.apply(presentation);
  $('robot-state').textContent=presentation.label;
  $('robot-caption').textContent=scene.kind==='alert'?'직접 이동 명령 · 경고 후 궤적과 구동 정지':scene.kind==='search'?'모사 탐색 · 발견 위치 보고 · 관제 수신 확인':'이동·표정은 장면 연출 · API 지시 실행 아님';
  const active=scene.kind==='alert'?(t<WARNING_AT?0:t<CLEAR_AT?1:2):scene.kind==='search'?(t<FOUND_AT?0:t<7?1:2):(t<2.8?0:t<5?1:2);
  for(let i=0;i<3;i++)$('step-'+i).classList.toggle('active',i<=active);
  $('progress').style.width=`${Math.min(100,t/12*100)}%`;
  return {scene:scene.id,results,stage:t,ready,robotState:robot.state,robot,events:[...fired]};
}
window.demo={load,tick,scenes};

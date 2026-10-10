"use strict";
const $ = (id) => document.getElementById(id);
let online = false, busy = false, sending = false, staged = null, editGeneration = 0, timer;
let gatewayReady = false, kit = false, powerSupported = true, otaSupported = true;
let peerBusy = false, peers = [];
const phases = {idle:"업데이트 대기",queued:"업데이트 준비",checking_image:"전체 파일 검사 중",preflight_failed:"파일 검사 실패",entering_bootloader:"부트로더 연결 중",bootloader_unavailable:"부트로더 연결 실패",transferring:"펌웨어 전송 중",validating_rebooting:"검사 및 재부팅 중",waiting_for_app:"앱 응답 확인 중",app_responding:"재부팅 후 앱 응답 확인",app_not_confirmed:"앱 응답 확인 필요",cancelled:"전송 중단됨",transfer_failed:"전송 실패 · USB 복구 확인",invalid_image:"지원하지 않는 파일"};
async function api(path, data, binary=false) {
  const options = data === undefined ? {} : {method:"POST",headers:{"X-LCD-Client":"1","Content-Type":binary?"application/zip":"application/json"},body:binary?data:JSON.stringify(data)};
  const response = await fetch(path, {...options,signal:AbortSignal.timeout(30000)});
  const result = await response.json();
  if (!response.ok) {
    const error = new Error(result.error || "요청에 실패했습니다.");
    error.httpStatus = response.status;
    throw error;
  }
  return result;
}
function controls() {
  for (const id of ["send","live"]) $(id).disabled = !online || busy || sending || peerBusy;
  for (const id of ["power-on","power-off"]) $(id).disabled = !online || busy || sending || peerBusy || !powerSupported;
  $("update").disabled = !online || busy || !staged || !otaSupported || peerBusy;
  $("package").disabled = busy || !otaSupported;
  $("cancel").disabled = !busy || !otaSupported;
  $("scan-peers").disabled = !gatewayReady || busy || sending || peerBusy;
  $("peer-select").disabled = !gatewayReady || busy || sending || peerBusy || !peers.length;
  $("connect-peer").disabled = !gatewayReady || busy || sending || peerBusy || $("peer-select").value === "";
  if (!online || busy) { $("live").checked=false; clearTimeout(timer); }
}
function previewText() {
  const text=$("display-text").value;
  if(!kit) {$("preview").textContent=text;return;}
  const lines=["","","",""];let row=0,column=0;
  for(const char of text) {
    if(char==="\n") {row++;column=0;continue;}
    if(column===21) {row++;column=0;}
    if(row>=4) break;
    lines[row]+=char;column++;
  }
  $("preview").textContent=lines.join("\n");
}
function capabilities(state) {
  kit=state.protocol==="nu40-kit-display";
  powerSupported=!kit && state.capabilities?.power!==false;
  otaSupported=!kit && state.capabilities?.ota!==false;
  if(Number.isInteger(state.columns) && Number.isInteger(state.rows)) $("geometry").textContent=`${state.columns}자 × ${state.rows}줄`;
  $("text-hint").textContent=kit?"영문·숫자·기호, 줄바꿈 포함 최대 84자. 21자마다 자동으로 줄을 바꿉니다.":"영문·숫자·기호 지원. 연결 후 화면 크기에 맞춰 검사합니다.";
  $("capability-note").textContent=kit?"현재 NU40-KIT-DISPLAY 펌웨어는 텍스트 표시를 지원합니다. 화면 켜기·끄기는 지원하지 않습니다.":"";
  $("ota-description").textContent=kit?"현재 NU40-KIT-DISPLAY 펌웨어는 OTA를 지원하지 않습니다. 이 보드에 맞는 부트로더와 업데이트 기능이 준비되어야 합니다.":"LCD 앱이 설치된 NU40에 업데이트 파일을 Wi-Fi와 BLE로 전송합니다. 처음 설치할 때는 부트로더 연결이 필요합니다.";
  if(!otaSupported) {staged=null;$("ota-phase").textContent="현재 펌웨어에서 지원하지 않음";$("ota-result").textContent="";$("progress").value=0;$("percent").textContent="—";}
  previewText();
}
function setHop(id, state, title, detail) {
  $(`${id}-hop`).dataset.state=state;
  $(`${id}-status`).textContent=title;
  $(`${id}-detail`).textContent=detail;
}
function usableIPv4(ip) {
  if (typeof ip !== "string" || !/^\d{1,3}(\.\d{1,3}){3}$/.test(ip)) return false;
  const octets=ip.split(".").map(Number);
  return octets.every(part=>part>=0 && part<=255) && octets[0]>0 && octets[0]<224 && octets[0]!==127;
}
function networkStatus(state) {
  const preview=state.preview===true;
  const reachable=!preview && state.gatewayReachable!==false;
  const wifi=reachable && state.wifi===true;
  const address=usableIPv4(state.ip)?state.ip:null;
  setHop("bridge","ready","로컬 서버 연결됨","맥북에서 제어 화면이 응답합니다.");
  if (preview) {
    setHop("wifi","pending","Wi-Fi 설정 대기","NU87의 Wi-Fi와 게이트웨이 주소를 설정해 주세요.");
  } else if (!reachable) {
    setHop("wifi","pending","게이트웨이 연결 대기","같은 Wi-Fi 연결과 NU87의 IP를 확인해 주세요.");
  } else if (wifi && address) {
    setHop("wifi","ready","Wi-Fi 연결됨",`NU87 ${address} · 맥북에서 응답 확인`);
  } else if (wifi) {
    setHop("wifi","pending","Wi-Fi 연결 · IP 확인 중","NU87이 연결을 보고했습니다. 사용 가능한 IP를 기다립니다.");
  } else {
    setHop("wifi","pending","게이트웨이 응답 확인",`${address ? `NU87 ${address} · ` : ""}Wi-Fi 연결 상태를 확인해 주세요.`);
  }
  if (!reachable) {
    setHop("ble","pending","연결 확인 대기","NU87 연결 후 Bluetooth 상태를 확인합니다.");
  } else if (state.connected && state.lcdReady) {
    setHop("ble","ready","화면 제어 준비됨",state.peer || "NU40 앱이 응답합니다.");
  } else if (state.dfuReady) {
    setHop("ble","pending","업데이트 모드 연결됨","화면 제어는 NU40 앱 실행 후 사용할 수 있습니다.");
  } else if (state.connected) {
    setHop("ble","pending","Bluetooth 연결 · 앱 확인 중",state.peer || "화면 제어 앱의 응답을 기다립니다.");
  } else if (state.configured===false) {
    setHop("ble","pending","NU40 주소 설정 대기","연결할 NU40의 Bluetooth 주소가 필요합니다.");
  } else {
    setHop("ble","pending","NU40 연결 대기",state.peer || "NU40 전원과 Bluetooth 연결 상태를 확인해 주세요.");
  }
  $("connection").textContent=online ? "화면 제어 준비됨" : wifi && address ? "Wi-Fi 연결됨 · NU40 대기" : preview ? "Wi-Fi 설정 대기" : "무선 연결 확인 중";
  $("connection").classList.toggle("connected",wifi && Boolean(address));
  $("network-hint").textContent=online ? "Wi-Fi와 Bluetooth 연결이 준비되었습니다. 아래에서 화면을 제어할 수 있습니다." : wifi && address ? "맥북 ↔ NU87 Wi-Fi 연결이 확인되었습니다. NU40 Bluetooth 연결은 별도로 준비하면 됩니다." : "맥북과 NU87을 같은 Wi-Fi에 연결해 주세요. 맥북의 Bluetooth 설정에서 NU40을 미리 페어링할 필요는 없습니다.";
  $("route").textContent=preview ? "미리보기 모드 · 실제 보드에는 전송하지 않습니다" : `NU87 ${address || "IP 확인 전"} → NU40 ${state.peer || "주소 미설정"}`;
}
async function status() {
  try {
    const state=await api("/api/status");
    online=Boolean(state.connected && state.lcdReady); busy=Boolean(state.ota?.busy);
    gatewayReady=state.preview!==true && state.wifi===true;
    capabilities(state);
    networkStatus(state);
    if(state.ota && otaSupported) {
      $("ota-phase").textContent=phases[state.ota.phase]||state.ota.phase;
      const pct=state.ota.size?Math.min(100,Math.floor(state.ota.acknowledged/state.ota.size*100)):0;
      $("progress").value=pct; $("percent").textContent=`${pct}%`;
      if(state.ota.error) $("ota-result").textContent=`오류 ${state.ota.error} · 전원과 USB 복구 경로를 확인해 주세요.`;
      else if(state.ota.phase==="app_responding") $("ota-result").textContent="앱이 다시 응답합니다. 화면 표시도 확인해 주세요. 설치 버전은 별도 확인이 필요합니다.";
      else if(state.ota.phase==="idle") $("ota-result").textContent="";
    }
  } catch(error) {
    online=false;gatewayReady=false;
    const bridgeAnswered=Boolean(error.httpStatus);
    setHop("bridge",bridgeAnswered?"ready":"error",bridgeAnswered?"로컬 서버 연결됨":"로컬 서버 응답 없음",bridgeAnswered?"맥북 제어 서버가 응답합니다.":"맥북의 제어 서버 실행 상태를 확인해 주세요.");
    setHop("wifi","pending","게이트웨이 응답 확인 필요","NU87 전원, 같은 Wi-Fi 연결, 게이트웨이 IP를 확인해 주세요.");
    setHop("ble","pending","연결 확인 대기","NU87 응답 후 Bluetooth 상태를 확인합니다.");
    $("connection").textContent=bridgeAnswered?"NU87 연결 대기":"로컬 서버 연결 실패";
    $("connection").classList.remove("connected");
    $("network-hint").textContent=bridgeAnswered?"맥북 제어 서버는 실행 중입니다. NU87까지의 Wi-Fi 연결을 확인해 주세요.":"로컬 서버가 다시 응답하면 연결 상태를 갱신합니다.";
    $("route").textContent=error.message;
  }
  controls(); setTimeout(status,1500);
}
async function sendText() {
  if(!online || busy || sending || peerBusy) return;
  sending=true; controls();
  const generation=editGeneration, text=$("display-text").value;
  $("display-result").textContent="전송하고 있습니다…";
  try {
    const state=await api("/api/display",{text});
    $("geometry").textContent=`${state.columns}자 × ${state.rows}줄`;
    $("display-result").textContent=generation===editGeneration?"화면 반영 응답을 받았습니다.":"이전 입력 반영 완료 · 새 입력 전송 대기";
  } catch(error) {$("display-result").textContent=error.message; $("live").checked=false;}
  finally {sending=false; controls(); if($("live").checked && generation!==editGeneration) timer=setTimeout(sendText,600);}
}
$("display-text").addEventListener("input",()=>{editGeneration++;previewText();clearTimeout(timer);if($("live").checked) timer=setTimeout(sendText,600);});
$("live").addEventListener("change",()=>{if($("live").checked) sendText();});
$("send").addEventListener("click",sendText);
for(const [id,on] of [["power-on",true],["power-off",false]]) $(id).addEventListener("click",async()=>{
  if(!powerSupported)return;
  sending=true;controls();try{await api("/api/power",{on});$("display-result").textContent=on?"화면 켜기 응답을 받았습니다.":"화면 끄기 응답을 받았습니다.";}catch(e){$("display-result").textContent=e.message;}finally{sending=false;controls();}
});
$("package").addEventListener("change",async()=>{
  if(!otaSupported)return;
  const file=$("package").files[0];staged=null;controls();if(!file)return;
  $("file-name").textContent=file.name;$("file-result").textContent="대상·주소·해시 검사 중";
  try{if(file.size>761856)throw new Error("파일이 너무 큽니다.");staged=await api("/api/package",file,true);$("file-result").textContent="대상과 무결성 검사 통과";$("file-version").textContent=`${staged.version} / ${(staged.size/1024).toFixed(1)} KB`;}catch(e){$("file-result").textContent=e.message;}controls();
});
$("update").addEventListener("click",async()=>{
  if(!staged || !otaSupported)return;busy=true;controls();$("live").checked=false;
  try{await api("/api/ota",{id:staged.sha256});$("ota-result").textContent="업데이트 요청을 접수했습니다. 두 보드의 전원을 유지해 주세요.";}catch(e){busy=false;$("ota-result").textContent=e.message;controls();}
});
$("cancel").addEventListener("click",async()=>{if(!otaSupported)return;try{await api("/api/cancel",{});$("ota-result").textContent="중단을 요청했습니다. 최종 상태가 바뀔 때까지 전원을 유지해 주세요.";}catch(e){$("ota-result").textContent=e.message;}});
$("peer-select").addEventListener("change",controls);
$("scan-peers").addEventListener("click",async()=>{
  peerBusy=true;peers=[];controls();$("peer-result").textContent="NU87에서 주변 Bluetooth 보드를 확인하고 있습니다…";
  const select=$("peer-select");select.replaceChildren(new Option("보드 이름과 주소를 확인하고 선택하세요",""));
  try {
    const result=await api("/api/peers");
    peers=(result.peers||[]).filter(peer=>typeof peer.address==="string" && /^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(peer.address) && ["public","random"].includes(peer.type));
    for(const [index,peer] of peers.entries()) select.add(new Option(`${peer.name||"이름 없음"} · ${peer.address} · ${peer.type}${Number.isFinite(peer.rssi)?` · ${peer.rssi} dBm`:""}`,String(index)));
    $("peer-result").textContent=peers.length?`${peers.length}개 보드를 찾았습니다. 연결할 보드의 주소를 선택해 주세요.`:"NU87에서 검색된 보드가 없습니다. NU40의 광고 상태와 NU87의 BLE 수신 상태를 확인해 주세요.";
  } catch(error) {$("peer-result").textContent=error.message;}
  finally {peerBusy=false;controls();}
});
$("connect-peer").addEventListener("click",async()=>{
  const selected=$("peer-select").value,peer=selected===""?null:peers[Number(selected)];
  if(!peer || peerBusy)return;
  peerBusy=true;$("live").checked=false;clearTimeout(timer);controls();
  try {await api("/api/peer",{address:peer.address,type:peer.type});$("peer-result").textContent=`${peer.address} 연결을 요청했습니다. 위에서 화면 제어 준비 상태를 확인해 주세요.`;}
  catch(error) {$("peer-result").textContent=error.message;}
  finally {peerBusy=false;controls();}
});
status();

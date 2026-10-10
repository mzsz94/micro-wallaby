import { startMicrophone, fileToWav } from './audio.mjs';

const $ = id => document.getElementById(id);
const kinds = { query: '교범 질의', report: '상황 보고', orders: '명령 정리', search: '기록 검색', motion: '음성 동작', alert: '폭발물 감지 훈련 경고' };
const fields = { format: '양식', summary: '요약', size: '규모', activity: '활동', location: '위치', unit: '소속', time: '시간', equipment: '장비', reporter: '보고자', observation: '관찰', action: '조치', request: '요청', who: '담당', what: '할 일', when: '기한', place: '장소', evidence: '원문 근거', unknown_fields: '미확인 항목', missing_fields: '미확인 항목', field: '항목', quote: '원문', note: '참고' };
let status = null, statusPending = false, boardStatus = null, boardPollPending = false;
let currentResult = null, busy = false, generation = 0, turnController = null;
let mic = null, micWanted = false, micStarting = false, micStopping = false;
let boardGeneration = null, speaking = false, suppressionUntil = 0, speechFinish = null;
let boardGateGeneration = null;
let speechEpoch = 0;
let simReady = false, localVoice = null, peers = [];
const boardSeen = new Set();
const seenResults = new Set();
let alertActive = false, alertPollPending = false, alertSyncEpoch = 0, alertRequestPending = false;

function notice(message, error = false) { $('notice').textContent = message; $('notice').classList.toggle('error', error); }
function describeError(error) {
  if (error?.name === 'NotAllowedError') return '마이크 권한이 허용되지 않았습니다. 브라우저 권한을 확인하거나 텍스트를 입력해 주세요.';
  if (error?.name === 'NotFoundError') return '사용 가능한 마이크를 찾을 수 없습니다. 입력 장치 연결을 확인해 주세요.';
  if (error?.name === 'NotReadableError') return '다른 앱이 마이크를 사용 중이거나 장치를 읽을 수 없습니다.';
  return error?.message || '요청을 처리하지 못했습니다.';
}
async function api(path, { json, signal, ...options } = {}) {
  const response = await fetch(path, {
    ...options, signal: signal || AbortSignal.timeout(15000),
    ...(json === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(json) }),
  });
  const value = await response.json();
  if (!response.ok) throw new Error(value.error || `요청 실패 (${response.status})`);
  return value;
}
function postMotion(command) {
  if (!simReady && command !== 'stop') { notice('시뮬레이터가 아직 준비되지 않아 동작을 실행하지 않았습니다.', true); return; }
  $('simulator').contentWindow?.postMessage({ type: 'wallaby-voice-command', command }, location.origin);
}
window.addEventListener('message', event => {
  if (event.source !== $('simulator').contentWindow || event.origin !== location.origin || !event.data) return;
  if (event.data.type === 'wallaby-voice-ready') {
    simReady = true; $('sim-status').textContent = '시뮬레이터 준비 완료';
    $('simulator').contentWindow?.postMessage({ type: 'wallaby-alert', active: alertActive }, location.origin);
  }
  if (event.data.type === 'wallaby-voice-result') {
    const names = { stop: '정지', greet: '인사', dock: '도킹', forward: '전진', back: '후진', left: '좌회전', right: '우회전' };
    $('sim-status').textContent = `${names[event.data.command] || '명령'} ${event.data.accepted ? '실행' : '거부 · 상태 확인'}`;
  }
});
$('simulator').addEventListener('load', () => {
  simReady = false;
  $('simulator').contentWindow?.postMessage({ type: 'wallaby-voice-hello' }, location.origin);
});
setInterval(() => {
  $('simulator').contentWindow?.postMessage({ type: simReady && !document.hidden ? 'wallaby-voice-heartbeat' : 'wallaby-voice-hello' }, location.origin);
}, 800);

function updateControls() {
  const active = micWanted && !micStarting;
  $('mic-toggle').textContent = micStarting ? '연결 중…' : micStopping ? '중지 중…' : micWanted ? '마이크 끄기' : '마이크 켜기';
  $('mic-toggle').disabled = micStarting || micStopping || alertActive || (!micWanted && !status?.stt?.ready);
  $('mic-toggle').setAttribute('aria-pressed', String(micWanted));
  $('input-source').disabled = micWanted || micStarting || micStopping;
  $('mode').disabled = micWanted && $('input-source').value === 'board';
  $('send-text').disabled = busy;
  $('audio-file').disabled = busy || !status?.stt?.ready;
  $('speak-answer').disabled = !localVoice || !currentResult || busy || speaking;
  $('voice-orbit').classList.toggle('listening', active);
  $('input-state').textContent = speaking ? '응답 재생 중' : busy ? '처리 중' : active ? 'LISTENING' : '대기';
  $('voice-title').textContent = speaking ? '왈라비가 답하고 있습니다.' : busy ? '발화를 정리하고 있습니다.' : active ? '왈라비가 듣고 있습니다.' : '음성으로 시작해 보세요.';
  $('voice-hint').textContent = active ? ($('input-source').value === 'board' ? 'NU 40DK의 PDM 마이크 → BLE → Wi-Fi → 로컬 AI' : '발화가 끝나면 자동으로 처리합니다. 응답 중에는 입력을 잠시 쉽니다.') : '입력 장치를 확인하고 마이크를 켜 주세요.';
}
function badge(id, text, state) { $(id).textContent = text; $(id.replace('-status', '-dot')).className = `status-dot ${state || ''}`; }
function renderGateway(gateway) {
  badge('gateway-status', gateway.available ? 'Wi-Fi 응답 확인' : gateway.configured ? '응답 없음' : '설정 없음', gateway.available ? 'ready' : gateway.configured ? 'warn' : '');
  badge('peer-status', gateway.connected ? (gateway.ready ? 'BLE 연결 · OLED 준비' : 'BLE 연결 · 준비 중') : 'BLE 미연결', gateway.connected ? 'ready' : '');
  $('gateway-detail').textContent = gateway.error || (gateway.ready ? '선택된 키트의 OLED 서비스가 준비되었습니다.' : gateway.available ? '게이트웨이가 응답했습니다. BLE 피어 연결 상태를 확인하세요.' : '실물 연결에는 서버의 게이트웨이 주소와 로컬 인증 설정이 필요합니다.');
  $('scan-peers').disabled = !gateway.configured;
  $('connect-peer').disabled = !gateway.configured || !peers.length;
  $('send-display').disabled = !gateway.ready;
}
async function refreshStatus() {
  if (statusPending) return;
  statusPending = true;
  const alertEpoch = alertSyncEpoch;
  try {
    status = await api('/api/status');
    badge('stt-status', status.stt.ready ? '로컬 STT 준비' : '모델 설정 필요', status.stt.ready ? 'ready' : 'warn');
    $('stt-detail').textContent = status.stt.detail || status.stt.message || status.stt.error || '로컬 STT 모델의 상태를 확인하세요.';
    const llmEnabled = status.llm?.enabled || status.engine?.llm_configured;
    badge('llm-status', llmEnabled ? '로컬 LLM 설정됨' : '교범 검색 · 규칙 추출', 'ready');
    renderGateway(status.gateway || {});
    if (alertEpoch === alertSyncEpoch && !alertRequestPending) acceptAlertState(status.alert_active === true);
  } catch (error) {
    status = null;
    badge('stt-status', '서버 응답 없음', 'warn');
    badge('llm-status', '서버 응답 없음', 'warn');
    renderGateway({});
    notice(`로컬 서버에 연결할 수 없습니다. ${describeError(error)}`, true);
    postMotion('stop');
    if (micWanted) await stopMicrophone();
  } finally { statusPending = false; updateControls(); }
}

function setAlert(active) {
  alertActive = active;
  $('alert-panel').classList.toggle('active', active);
  $('alert-state').textContent = active ? '⚠ 폭발물 감지 훈련 경고 · 왈라비 정지' : '폭발물 감지 훈련 · 대기';
  $('alert-detail').textContent = active ? '이동과 마이크 입력이 차단되었습니다. 제어탑 스피커 출력 상태를 확인하고 경고를 명시적으로 해제하세요.' : '제어탑 BTN4를 누르면 감지를 모사합니다. MAX98357A 스피커 경보와 왈라비 정지를 함께 확인하세요.';
  $('simulator').contentWindow?.postMessage({ type: 'wallaby-alert', active }, location.origin);
  if (active) postMotion('stop');
  updateControls();
}
function acceptAlertState(active) {
  if (active !== alertActive) alertSyncEpoch++;
  if (active && !alertActive) {
    generation++; turnController?.abort(); turnController = null; busy = false;
    cancelSpeech();
    if (micWanted) stopMicrophone();
  }
  setAlert(active);
}
async function pollAlerts() {
  if (alertPollPending || document.hidden) return;
  alertPollPending = true;
  const epoch = alertSyncEpoch;
  try {
    const state = await api('/api/alerts');
    if (epoch !== alertSyncEpoch || alertRequestPending) return;
    acceptAlertState(state.active === true);
    if (state.device?.error && state.active) $('alert-detail').textContent = `이동 차단 유지 · 스피커 출력 미확인: ${state.device.error}`;
    if (state.last_result?.id && !seenResults.has(state.last_result.id)) await applyResult(state.last_result, generation);
  } catch (error) {
    if (alertActive) $('alert-detail').textContent = `이동 차단 유지 · 제어탑 경보 상태 미확인: ${describeError(error)}`;
  } finally { alertPollPending = false; }
}
async function requestAlert(active) {
  alertRequestPending = true; alertSyncEpoch++;
  if (active) setAlert(true);
  $('trigger-alert').disabled = $('clear-alert').disabled = true;
  try {
    await stopAll(active ? '폭발물 감지 훈련 경고를 요청합니다.' : '제어탑 경보 해제를 요청합니다.');
    await runTurn({ text: active ? '왈라비 폭발물 감지' : '왈라비 경고 해제', mode: 'auto' });
  } finally { alertRequestPending = false; $('trigger-alert').disabled = $('clear-alert').disabled = false; }
}

async function gateBoard(muted) {
  if (!micWanted || $('input-source').value !== 'board') return;
  const response = await api('/api/device-audio/gate', { json: { muted } });
  if (response.error) throw new Error(response.error);
  boardGateGeneration = response.gate_generation;
}
async function toggleMicrophone() {
  if (micWanted) { await stopAll(); return; }
  if (micStarting || micStopping) return;
  micStarting = true; micWanted = true;
  const token = generation;
  updateControls();
  try {
    if ($('input-source').value === 'board') {
      const result = await api('/api/device-audio/start', { json: { mode: $('mode').value } });
      if (result.error || !result.running) throw new Error(result.error || '키트 오디오를 시작하지 못했습니다. 연결 및 오디오 펌웨어를 확인하세요.');
      boardGeneration = result.generation;
      boardGateGeneration = result.gate_generation;
      if (!micWanted || token !== generation) { await api('/api/device-audio/stop', { json: {} }); return; }
      boardStatus = result;
      notice('NU 40DK 키트 마이크를 켰습니다. 발화가 끝나면 HostPC에서 처리합니다.');
    } else {
      const capture = await startMicrophone({
        threshold: Number($('vad-threshold').value),
        onSpeech: audio => runTurn({ audio }),
        isGated: () => !micWanted || busy || speaking || performance.now() < suppressionUntil,
        onLevel: (level, hearing) => { $('audio-level').style.width = `${Math.min(100, level * 700)}%`; $('voice-orbit').classList.toggle('hearing', hearing); },
        onEnded: () => { stopMicrophone(); postMotion('stop'); notice('입력 장치 연결이 끊겨 마이크와 동작을 중지했습니다.', true); },
      });
      if (!micWanted || token !== generation) { await capture.stop(); return; }
      mic = capture;
      notice('HostPC 마이크를 켰습니다. 침묵은 전송하지 않습니다.');
    }
  } catch (error) { micWanted = false; notice(describeError(error), true); }
  finally { micStarting = false; updateControls(); }
}
async function stopMicrophone() {
  micWanted = false; micStopping = true;
  updateControls();
  const capture = mic; mic = null;
  try {
    if (capture) await capture.stop();
    if ($('input-source').value === 'board') {
      const result = await api('/api/device-audio/stop', { json: {} });
      boardGeneration = result.generation;
      if (result.device?.ok === false && status?.gateway?.configured) throw new Error(result.device.error || '키트 마이크 정지 응답을 확인하지 못했습니다.');
    }
  } catch (error) { notice(`마이크 중지 확인 실패: ${describeError(error)}`, true); }
  finally { micStopping = false; $('audio-level').style.width = '0%'; $('voice-orbit').classList.remove('hearing'); updateControls(); }
}
function cancelSpeech() {
  speechEpoch++;
  globalThis.speechSynthesis?.cancel();
  if (speechFinish) speechFinish(false);
  speaking = false;
  suppressionUntil = performance.now() + 600;
}
async function stopAll(message = '녹음, 응답 재생, 시뮬레이션을 중지했습니다.') {
  generation++;
  turnController?.abort(); turnController = null;
  cancelSpeech();
  postMotion('stop');
  busy = false;
  notice(message);
  const stops = await Promise.allSettled([stopMicrophone(), api('/api/control/stop', { json: {} })]);
  const robotStop = stops[1];
  if (robotStop.status === 'fulfilled' && robotStop.value.robot?.error) notice(`로컬 동작은 정지했습니다. NU87 정지 응답 미확인: ${robotStop.value.robot.error}`, true);
  else if (robotStop.status === 'rejected') notice(`로컬 동작은 정지했습니다. 서버 정지 응답 미확인: ${describeError(robotStop.reason)}`, true);
}
async function pollBoard() {
  if (boardPollPending || document.hidden) return;
  boardPollPending = true;
  const token = generation;
  try {
    const lease = micWanted && $('input-source').value === 'board' && boardGeneration !== null ? `?generation=${encodeURIComponent(boardGeneration)}` : '';
    const state = await api(`/api/device-audio/status${lease}`);
    boardStatus = state;
    $('board-audio-status').textContent = state.error || (state.running ? `키트 ${state.state || '수신 중'}` : state.available ? '키트 오디오 준비' : '오디오 연결 필요');
    if (micWanted && $('input-source').value === 'board' && token === generation && !micStarting) {
      if (!state.running || state.error) {
        await stopAll(state.error || '키트 오디오 연결이 종료되어 녹음과 동작을 중지했습니다.');
        return;
      }
      if (state.generation !== boardGeneration || state.gate_generation !== boardGateGeneration || busy || speaking || performance.now() < suppressionUntil) return;
      const result = state.last_result;
      if (result?.id && !boardSeen.has(result.id)) {
        boardSeen.add(result.id);
        if (boardSeen.size > 100) boardSeen.delete(boardSeen.values().next().value);
        // Keep renewing the board lease while an answer is being spoken.
        applyResult(result, token).catch(error => notice(describeError(error), true));
      }
    }
  } catch (error) {
    $('board-audio-status').textContent = '키트 오디오 미확인';
    if (micWanted && $('input-source').value === 'board' && token === generation) await stopAll(`키트 오디오 상태를 확인할 수 없어 중지했습니다. ${describeError(error)}`);
  } finally { boardPollPending = false; }
}

function updateVoices() {
  localVoice = globalThis.speechSynthesis?.getVoices().find(voice => voice.localService === true && /^ko(?:-|_)/i.test(voice.lang)) || null;
  $('tts-status').textContent = localVoice ? `로컬 한국어 음성: ${localVoice.name}. 재생 중과 종료 후 0.6초 동안 마이크 입력을 차단합니다.` : '로컬 한국어 음성이 없습니다. OS에서 한국어 음성을 설치하면 사용할 수 있습니다.';
  $('tts-enabled').disabled = !localVoice;
  if (!localVoice) $('tts-enabled').checked = false;
  $('speak-answer').disabled = !localVoice || !currentResult || busy || speaking;
}
async function speak(text, token = generation) {
  if (!localVoice || !text || speaking || token !== generation) return;
  speaking = true;
  const epoch = ++speechEpoch;
  mic?.reset();
  updateControls();
  try {
    await gateBoard(true);
    if (token !== generation || epoch !== speechEpoch) return;
    const utterance = new SpeechSynthesisUtterance(String(text).slice(0, 2500));
    utterance.voice = localVoice; utterance.lang = localVoice.lang; utterance.rate = 1;
    await new Promise(resolve => {
      let finished = false;
      const watchdog = setTimeout(() => { globalThis.speechSynthesis.cancel(); finish(false); }, 90000);
      function finish(ok) { if (finished) return; finished = true; clearTimeout(watchdog); speechFinish = null; resolve(ok); }
      speechFinish = finish;
      utterance.onend = () => finish(true);
      utterance.onerror = () => { notice('로컬 음성을 재생하지 못했습니다. 화면 응답을 확인하세요.', true); finish(false); };
      globalThis.speechSynthesis.speak(utterance);
    });
  } catch (error) { notice(`음성 재생을 시작하지 못했습니다. ${describeError(error)}`, true); }
  finally {
    suppressionUntil = performance.now() + 600;
    await new Promise(resolve => setTimeout(resolve, 600));
    if (token === generation && epoch === speechEpoch) {
      speaking = false;
      mic?.reset();
      if (!busy) {
        try { await gateBoard(false); } catch (error) { await stopAll(`키트 마이크 입력을 복구하지 못했습니다. ${describeError(error)}`); }
      }
      updateControls();
    }
  }
}

function node(tag, text, className) { const element = document.createElement(tag); if (text !== undefined) element.textContent = text; if (className) element.className = className; return element; }
function displayValue(value) {
  if (value === null || value === undefined || value === '') return '미확인';
  if (Array.isArray(value)) return value.length ? value.map(displayValue).join('\n') : '없음';
  if (typeof value === 'object') return Object.entries(value).map(([key, item]) => `${fields[key] || key}: ${displayValue(item)}`).join('\n');
  return fields[value] || String(value);
}
function reportCard(title, values) {
  const card = node('section', undefined, 'report-card'); card.append(node('h3', title));
  const list = node('dl');
  for (const [key, value] of Object.entries(values)) { const row = node('div', undefined, 'report-row'); row.append(node('dt', fields[key] || key), node('dd', displayValue(value))); list.append(row); }
  card.append(list); return card;
}
function renderResult(result) {
  currentResult = result;
  $('result-empty').hidden = true; $('result-content').hidden = false;
  $('transcript').textContent = result.transcript || '';
  $('answer').textContent = result.answer || '응답이 없습니다.';
  $('result-kind').textContent = `${kinds[result.kind] || result.kind || 'RESULT'}${String(result.engine || '').startsWith('local-llm') ? ' · LOCAL LLM' : ''}`;
  $('result-time').textContent = formatTime(result.timestamp);
  $('report').replaceChildren();
  const report = result.report;
  if (report) {
    if (report.salute) $('report').append(reportCard('SALUTE · 표준 보고', report.salute));
    if (report.situation) $('report').append(reportCard('상황 보고', report.situation));
    if (Array.isArray(report.items)) report.items.forEach((item, i) => $('report').append(reportCard(`명령 ${i + 1}`, item)));
    const remainder = Object.fromEntries(Object.entries(report).filter(([key]) => !['salute', 'situation', 'items'].includes(key)));
    if (Object.keys(remainder).length) $('report').append(reportCard('추출 정보', remainder));
  }
  if (result.analysis && typeof result.analysis === 'object') $('report').append(reportCard('로컬 AI 분석', result.analysis));
  $('sources').replaceChildren();
  if (result.sources?.length) $('sources').append(node('p', '참고한 로컬 자료', 'source-heading'));
  for (const source of result.sources || []) { const card = node('div', undefined, 'source-card'); card.append(node('h4', source.title || source.id), node('p', source.excerpt || source.text || '')); $('sources').append(card); }
  $('device-result').textContent = result.device ? (result.device.acknowledged ? result.kind === 'alert' ? `NU40 스피커 경보 ${result.device.active ? '활성화' : '해제'} 출력 ACK 수신` : '키트 OLED 드라이버 ACK 수신 · 실제 화면은 별도 확인하세요.' : `제어탑 출력 미확인: ${result.device.error || result.device.message || '상태를 확인하세요.'}`) : '';
  if (result.robot) $('device-result').textContent += result.robot.mode === 'local-simulation' ? '\nNU87 미연결 · 로컬 동작 시뮬레이션' : result.robot.accepted ? `\nNU87 명령 수락 · ${result.robot.state || result.robot.command} · 동작은 시뮬레이션으로 표시` : `\nNU87 명령 미확인 · ${result.robot.error || '동작 정지'}`;
  updateVoices();
}
async function applyResult(result, token) {
  if (token !== generation) return;
  if (result.alert_active === true && result.kind !== 'alert') acceptAlertState(true);
  if (result.id && seenResults.has(result.id)) return;
  if (result.id) { seenResults.add(result.id); if (seenResults.size > 100) seenResults.delete(seenResults.values().next().value); }
  if (result.kind === 'alert') {
    alertSyncEpoch++;
    setAlert(result.alert?.active === true);
    if (alertActive) { cancelSpeech(); if (micWanted) stopMicrophone(); }
  }
  renderResult(result);
  notice(`${kinds[result.kind] || '발화'} 처리가 완료되었습니다. 추출 내용과 근거를 확인하세요.`);
  if (result.motion && (result.motion === 'stop' || (!alertActive && $('motion-enabled').checked)) && !document.hidden && document.hasFocus()) postMotion(result.motion);
  refreshRecords();
  if ($('tts-enabled').checked && !alertActive) await speak(result.answer, token);
}
async function runTurn({ text, audio, file, mode } = {}) {
  if (busy) { notice('이전 입력을 처리 중입니다. 잠시 기다려 주세요.'); return; }
  busy = true; cancelSpeech(); mic?.reset();
  const token = generation;
  const controller = new AbortController(); turnController = controller;
  // Inference can take longer than status calls, but it must remain bounded.
  const timeout = setTimeout(() => controller.abort(), 120000);
  updateControls(); notice(audio || file ? '로컬 음성 인식 중입니다…' : '로컬 자료를 확인하고 있습니다…');
  try {
    await gateBoard(true);
    if (file) audio = await fileToWav(file);
    if (token !== generation || controller.signal.aborted) return;
    const selectedMode = mode || $('mode').value;
    const result = audio ? await api(`/api/audio?mode=${encodeURIComponent(selectedMode)}`, { method: 'POST', headers: { 'Content-Type': 'audio/wav' }, body: audio, signal: controller.signal }) : await api('/api/turn', { json: { text, mode: selectedMode }, signal: controller.signal });
    await applyResult(result, token);
  } catch (error) { if (token === generation) notice(error.name === 'AbortError' ? '처리 제한 시간을 초과했습니다. 다시 시도해 주세요.' : describeError(error), true); }
  finally {
    clearTimeout(timeout);
    if (token === generation) {
      busy = false; turnController = null; suppressionUntil = performance.now() + 600; mic?.reset();
      try { await gateBoard(false); } catch (error) { await stopAll(`키트 마이크 입력을 복구하지 못했습니다. ${describeError(error)}`); }
      updateControls();
    }
  }
}
function formatTime(value) { const date = new Date(value); return Number.isNaN(date.getTime()) ? '' : date.toLocaleString('ko-KR'); }
let searchGeneration = 0;
async function refreshRecords() {
  const token = ++searchGeneration;
  try {
    const data = await api(`/api/records?q=${encodeURIComponent($('record-query').value)}`);
    if (token !== searchGeneration) return;
    const records = data.records || [];
    $('record-count').textContent = `${records.length} RECORDS`;
    $('record-list').replaceChildren();
    if (!records.length) $('record-list').append(node('p', '일치하는 기록이 없습니다.', 'muted'));
    for (const record of records) {
      const button = node('button', undefined, 'record-item');
      button.append(node('span', record.transcript || record.answer || '기록'), node('small', `${kinds[record.kind] || record.kind || ''} · ${formatTime(record.timestamp)}`));
      button.addEventListener('click', () => { renderResult(record); notice('저장된 기록을 열었습니다. 동작과 음성은 재실행하지 않습니다.'); });
      $('record-list').append(button);
    }
  } catch (error) { if (token === searchGeneration) $('record-list').replaceChildren(node('p', describeError(error), 'muted')); }
}

$('mic-toggle').addEventListener('click', toggleMicrophone);
$('trigger-alert').addEventListener('click', () => requestAlert(true));
$('clear-alert').addEventListener('click', () => requestAlert(false));
$('emergency-stop').addEventListener('click', () => stopAll());
$('sim-stop').addEventListener('click', () => stopAll());
$('refresh-status').addEventListener('click', () => { refreshStatus(); pollBoard(); });
$('text-form').addEventListener('submit', event => { event.preventDefault(); const text = $('text-input').value.trim(); if (text) runTurn({ text }); });
$('audio-file').addEventListener('change', event => { const file = event.target.files[0]; event.target.value = ''; if (file) runTurn({ file }); });
$('vad-threshold').addEventListener('input', () => { $('vad-value').textContent = Number($('vad-threshold').value).toFixed(3); mic?.threshold(Number($('vad-threshold').value)); });
$('input-source').addEventListener('change', updateControls);
$('motion-enabled').addEventListener('change', () => { if (!$('motion-enabled').checked) postMotion('stop'); });
$('tts-enabled').addEventListener('change', async () => {
  if ($('tts-enabled').checked) return;
  cancelSpeech(); updateControls();
  const epoch = speechEpoch;
  await new Promise(resolve => setTimeout(resolve, 600));
  if (epoch === speechEpoch && !busy && !speaking) {
    try { await gateBoard(false); } catch (error) { await stopAll(describeError(error)); }
  }
});
$('speak-answer').addEventListener('click', () => { if (!busy) speak(currentResult?.answer); });
$('search-form').addEventListener('submit', event => { event.preventDefault(); refreshRecords(); });
for (const button of document.querySelectorAll('[data-example]')) button.addEventListener('click', () => {
  const text = button.dataset.example;
  $('text-input').value = text.startsWith('상황 보고.') ? '상황 보고: 보고자 훈련팀. 시각 14시 20분. 위치 동쪽 집결지. 인원 4명 이동 완료. 장비 이상 없음. 조치 대기. 요청 추가 안내.' : text;
  if (!$('mode').disabled) $('mode').value = text.startsWith('상황 보고.') ? 'report' : text.startsWith('왈라비') ? 'auto' : 'query';
  $('text-input').focus();
});
$('export-record').addEventListener('click', () => {
  if (!currentResult) return;
  const url = URL.createObjectURL(new Blob([JSON.stringify(currentResult, null, 2)], { type: 'application/json' }));
  const link = node('a'); link.href = url; link.download = `military-wallaby-${currentResult.id || 'record'}.json`; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
});
$('scan-peers').addEventListener('click', async () => {
  $('scan-peers').disabled = true;
  try {
    const result = await api('/api/gateway/peers');
    if (result.error) throw new Error(result.error);
    peers = result.peers || [];
    $('peer-select').replaceChildren();
    for (const [i, peer] of peers.entries()) { const option = node('option', `${peer.name || 'NU40'} · ${peer.address} · ${peer.rssi} dBm`); option.value = String(i); $('peer-select').append(option); }
    if (!peers.length) { const option = node('option', '발견된 피어 없음'); option.value = ''; $('peer-select').append(option); }
    $('device-notice').textContent = `${peers.length}개 BLE 후보를 조회했습니다. 연결할 키트를 선택하세요.`;
  } catch (error) { $('device-notice').textContent = describeError(error); }
  finally { $('scan-peers').disabled = false; $('connect-peer').disabled = !peers.length; }
});
$('connect-peer').addEventListener('click', async () => {
  const peer = peers[Number($('peer-select').value)]; if (!peer) return;
  $('connect-peer').disabled = true;
  try {
    await stopAll('BLE 피어를 변경하기 전에 입력과 시뮬레이션을 중지했습니다.');
    const result = await api('/api/gateway/peer', { json: { address: peer.address, type: peer.type } });
    if (result.error) throw new Error(result.error);
    $('device-notice').textContent = result.accepted ? '피어 선택 요청이 접수되었습니다. 상태 새로고침으로 BLE 연결을 확인하세요.' : '피어 선택을 확인하지 못했습니다.';
    await refreshStatus();
  } catch (error) { $('device-notice').textContent = describeError(error); }
  finally { $('connect-peer').disabled = !peers.length; }
});
$('display-form').addEventListener('submit', async event => {
  event.preventDefault(); const text = $('display-text').value;
  if (!text || !/^[\x20-\x7e]+$/.test(text)) { $('device-notice').textContent = '표시할 내용을 ASCII 영문·숫자로 입력해 주세요.'; return; }
  $('send-display').disabled = true;
  try {
    const result = await api('/api/gateway/display', { json: { text } });
    if (result.error) throw new Error(result.error);
    $('device-notice').textContent = result.acknowledged ? 'OLED 드라이버 ACK를 받았습니다. 실제 화면은 키트에서 확인하세요.' : 'OLED 응답을 확인하지 못했습니다.';
  } catch (error) { $('device-notice').textContent = describeError(error); }
  finally { $('send-display').disabled = !status?.gateway?.ready; }
});
window.addEventListener('blur', () => postMotion('stop'));
window.addEventListener('offline', () => stopAll('연결 상태가 바뀌어 입력과 동작을 중지했습니다.'));
document.addEventListener('visibilitychange', () => { if (document.hidden) stopAll('화면이 숨겨져 입력과 동작을 중지했습니다.'); });
window.addEventListener('pagehide', () => {
  generation++; micWanted = false; turnController?.abort(); cancelSpeech(); mic?.stop(); postMotion('stop');
  fetch('/api/device-audio/stop', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}', keepalive: true }).catch(() => {});
  fetch('/api/control/stop', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}', keepalive: true }).catch(() => {});
});
globalThis.speechSynthesis?.addEventListener('voiceschanged', updateVoices);
updateVoices(); updateControls(); refreshStatus(); refreshRecords(); pollBoard(); pollAlerts();
setInterval(() => { if (!document.hidden) refreshStatus(); }, 15000);
setInterval(pollBoard, 1000);
setInterval(pollAlerts, 1000);

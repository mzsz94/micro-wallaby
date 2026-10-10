// Interactive replay of the GIF's captioned scenarios, on an isolated local API.
if (new URLSearchParams(location.search).get('play') === '1') {
  const style = document.createElement('link');
  style.rel = 'stylesheet'; style.href = '/demo/player.css'; document.head.append(style);
  document.body.classList.add('interactive');
  const controls = document.createElement('section');
  controls.className = 'demo-controls'; controls.setAttribute('aria-label', '시나리오 재생');
  controls.innerHTML = '<button id="play-all" disabled>전체 5장면 재생</button>' +
    '<label>장면<select id="play-scene" disabled></select></label>' +
    '<button id="play-one" disabled>선택 장면 재생</button>' +
    '<button id="play-stop" class="stop" disabled>■ 정지</button>' +
    '<p id="play-status" role="status" aria-live="polite">시연 화면을 준비하고 있습니다.</p>';
  document.querySelector('header').after(controls);
  const $ = id => document.getElementById(id);
  const frame = $('robot-frame');
  const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
  let running = false, ready = false, epoch = 0;

  function update() {
    for (const id of ['play-all', 'play-one', 'play-scene']) $(id).disabled = !ready || running;
    $('play-stop').disabled = !running;
  }
  function halt() {
    frame.contentWindow.wallabyDemo?.apply({ drive: { linear: 0, angular: 0 } });
    frame.contentWindow.postMessage({ type: 'wallaby-voice-command', command: 'stop' }, location.origin);
  }
  function stop(message = '정지했습니다. 다시 재생하면 선택한 장면부터 시작합니다.') {
    epoch++; halt(); $('play-status').textContent = message;
  }
  async function json(path, body) {
    const response = await fetch(path, {
      signal: AbortSignal.timeout(5000),
      ...(body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }),
    });
    if (!response.ok) throw new Error('로컬 시연 API 응답을 확인할 수 없습니다.');
    return response.json();
  }
  async function prepare(index) {
    // A previous search scene leaves a warning latched. Explicit replay starts
    // a fresh simulated scenario; merely stopping never clears that warning.
    const alarm = await json('/api/alerts');
    if (alarm.active) await json('/api/turn', { text: '왈라비 경고 해제', mode: 'auto' });
    await window.demo.load(index);
  }
  async function play(all) {
    if (!ready || running) return;
    running = true; const token = ++epoch; update();
    $('play-status').classList.remove('error');
    try {
      const indexes = all ? window.demo.scenes.map((_, index) => index) : [Number($('play-scene').value)];
      for (const index of indexes) {
        if (token !== epoch) return;
        $('play-scene').value = String(index);
        $('play-status').textContent = `${index + 1}/5 · 자막 입력과 동작 연출 / 실제 로컬 API 응답`;
        await prepare(index);
        if (token !== epoch) return;
        const started = performance.now();
        let elapsed = 0;
        while (token === epoch && elapsed < 12) {
          await window.demo.tick(elapsed);
          await delay(100);
          elapsed = (performance.now() - started) / 1000;
        }
        if (token !== epoch) return;
        await window.demo.tick(11.99);
        halt();
      }
      $('play-status').textContent = '시연 완료 · 음성·NU87·NU40 실기 시연은 http://127.0.0.1:8767/에서 진행하세요.';
    } catch (error) {
      if (token === epoch) {
        halt(); $('play-status').classList.add('error');
        $('play-status').textContent = `시연을 중단했습니다: ${error.message}`;
      }
    } finally {
      halt(); running = false; update();
    }
  }
  $('play-all').addEventListener('click', () => play(true));
  $('play-one').addEventListener('click', () => play(false));
  $('play-stop').addEventListener('click', () => stop());
  document.addEventListener('visibilitychange', () => { if (document.hidden && running) stop('화면이 숨겨져 시연을 정지했습니다.'); });
  window.addEventListener('blur', () => { if (running) stop('화면이 비활성화되어 시연을 정지했습니다.'); });
  window.addEventListener('pagehide', () => { if (running) stop(); });

  try {
    const state = await json('/api/status');
    if (state.gateway.configured || state.llm.enabled) {
      throw new Error('시나리오 전용 서버를 8768 포트에서 실행해 주세요.');
    }
    for (let attempt = 0; attempt < 80; attempt++) {
      if (window.demo && frame.contentWindow.wallabyDemo) { ready = true; break; }
      await delay(100);
    }
    if (!ready) throw new Error('시뮬레이터가 준비되지 않았습니다. 화면을 새로고침해 주세요.');
    for (const [index, scene] of window.demo.scenes.entries()) {
      const option = document.createElement('option'); option.value = String(index);
      option.textContent = `${index + 1}. ${scene.tag.split(' / ')[1]}`; $('play-scene').append(option);
    }
    await window.demo.load(0); await window.demo.tick(0);
    halt();
    frame.contentWindow.wallabyDemo.apply({ expression: 'idle', label: '재생 대기' });
    $('transcript').textContent = window.demo.scenes[0].input;
    $('input-label').textContent = '시나리오 입력 · 재생 대기';
    $('robot-state').textContent = 'READY / 재생 대기';
    $('play-status').textContent = '전체 5장면 재생을 누르세요. 약 60초 · 자막 연출 / 로컬 규칙 응답 / 가상 동작';
    update();
  } catch (error) {
    ready = false; update(); $('play-status').classList.add('error');
    $('play-status').textContent = error.message;
  }
}

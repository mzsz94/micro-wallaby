// SPDX-License-Identifier: Apache-2.0

import {
  BUTTON,
  HardwareInputSession,
  InputMux,
  RelativeDelayEstimator,
  SequenceGapTracker,
  decodeInputEvent,
  readNesRomFile,
  webSocketUrl,
} from "/controller.js";

const FRAME_INTERVAL_MS = 1000 / 60.098;
const RECONNECT_DELAY_MS = 1500;

const byId = (id) => document.getElementById(id);
const canvas = byId("screen");
const context = canvas.getContext("2d", { alpha: false });
const imageData = context.createImageData(256, 240);
const pixelBuffer = new ArrayBuffer(imageData.data.length);
const pixels8 = new Uint8ClampedArray(pixelBuffer);
const pixels32 = new Uint32Array(pixelBuffer);
const errorOutput = byId("error");
const pauseButton = byId("pause");
const invertY = byId("invert-y");

let nes;
let romLoaded = false;
let running = false;
let animationRequest = null;
let lastFrameTime = null;
let frameAccumulator = 0;
let lastSample = null;

function showError(message) {
  errorOutput.textContent = message instanceof Error ? message.message : String(message);
}

function clearError() {
  errorOutput.textContent = "";
}

function updateFrameBuffer(frameBuffer) {
  for (let index = 0; index < frameBuffer.length; index += 1) {
    pixels32[index] = 0xff000000 | frameBuffer[index];
  }
}

function drawFrame() {
  imageData.data.set(pixels8);
  context.putImageData(imageData, 0, 0);
}

function stopEmulator() {
  running = false;
  pauseButton.textContent = "계속";
  if (animationRequest !== null) {
    cancelAnimationFrame(animationRequest);
    animationRequest = null;
  }
  lastFrameTime = null;
  frameAccumulator = 0;
}

function frameLoop(time) {
  if (!running) {
    return;
  }
  animationRequest = requestAnimationFrame(frameLoop);
  if (lastFrameTime === null) {
    lastFrameTime = time;
    return;
  }

  frameAccumulator += Math.min(time - lastFrameTime, 100);
  lastFrameTime = time;
  let generated = 0;
  try {
    while (frameAccumulator >= FRAME_INTERVAL_MS && generated < 3) {
      nes.frame();
      frameAccumulator -= FRAME_INTERVAL_MS;
      generated += 1;
    }
    if (generated > 0) {
      drawFrame();
    }
  } catch (error) {
    stopEmulator();
    showError(`에뮬레이터가 중지되었습니다: ${error.message}`);
  }
}

function startEmulator() {
  if (!romLoaded || running) {
    return;
  }
  running = true;
  pauseButton.textContent = "일시정지";
  animationRequest = requestAnimationFrame(frameLoop);
}

function syncButtons(nextMask, previousMask) {
  if (!nes) {
    return;
  }
  for (let button = BUTTON.A; button <= BUTTON.RIGHT; button += 1) {
    const bit = 1 << button;
    if ((nextMask & bit) !== 0 && (previousMask & bit) === 0) {
      nes.buttonDown(1, button);
    } else if ((nextMask & bit) === 0 && (previousMask & bit) !== 0) {
      nes.buttonUp(1, button);
    }
  }
}

function assertJsnesButtonMapping() {
  for (const [name, value] of Object.entries(BUTTON)) {
    if (globalThis.jsnes.Controller[`BUTTON_${name}`] !== value) {
      throw new Error(`JSNES controller mapping mismatch for ${name}`);
    }
  }
}

try {
  if (!globalThis.jsnes?.NES || !globalThis.jsnes?.Controller) {
    throw new Error("내장 JSNES 라이브러리를 읽지 못했습니다");
  }
  assertJsnesButtonMapping();
  nes = new globalThis.jsnes.NES({
    emulateSound: false,
    onFrame: updateFrameBuffer,
    onStatusUpdate: () => {},
  });
} catch (error) {
  showError(error);
}

const mux = new InputMux(syncButtons);
const hardwareSession = new HardwareInputSession(mux, {
  staleMs: 250,
  onMaskChange(mask) {
    byId("sw-value").textContent = (mask & (1 << BUTTON.A)) !== 0 ? "눌림" : "해제";
  },
});
const delayEstimator = new RelativeDelayEstimator();

byId("rom-file").addEventListener("change", async (event) => {
  const [file] = event.target.files;
  event.target.value = "";
  if (!file || !nes) {
    return;
  }

  clearError();
  stopEmulator();
  try {
    const metadata = await readNesRomFile(file);
    nes.loadROM(metadata.bytes);
    syncButtons(mux.mask, 0);
    romLoaded = true;
    byId("empty-message").hidden = true;
    pauseButton.disabled = false;
    startEmulator();
  } catch (error) {
    romLoaded = false;
    pauseButton.disabled = true;
    showError(`ROM을 열 수 없습니다: ${error.message}`);
  }
});

pauseButton.addEventListener("click", () => {
  if (running) {
    stopEmulator();
  } else {
    startEmulator();
  }
});

const screenPointers = new Map();
function refreshScreenButtons() {
  let mask = 0;
  for (const buttonMask of screenPointers.values()) {
    mask |= buttonMask;
  }
  mux.set("screen", mask);
  for (const button of document.querySelectorAll("[data-button]")) {
    button.dataset.pressed = String((mask & (1 << Number(button.dataset.button))) !== 0);
  }
}

for (const button of document.querySelectorAll("[data-button]")) {
  const buttonMask = 1 << Number(button.dataset.button);
  button.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    screenPointers.set(event.pointerId, buttonMask);
    refreshScreenButtons();
  });
  const release = (event) => {
    screenPointers.delete(event.pointerId);
    refreshScreenButtons();
  };
  button.addEventListener("pointerup", release);
  button.addEventListener("pointercancel", release);
  button.addEventListener("lostpointercapture", release);
}

const keyButtons = new Map([
  ["KeyX", BUTTON.A],
  ["KeyZ", BUTTON.B],
  ["Enter", BUTTON.START],
  ["ShiftLeft", BUTTON.SELECT],
  ["ShiftRight", BUTTON.SELECT],
  ["ArrowUp", BUTTON.UP],
  ["ArrowDown", BUTTON.DOWN],
  ["ArrowLeft", BUTTON.LEFT],
  ["ArrowRight", BUTTON.RIGHT],
]);
const pressedKeys = new Set();

function refreshKeyboard() {
  let mask = 0;
  for (const code of pressedKeys) {
    mask |= 1 << keyButtons.get(code);
  }
  mux.set("keyboard", mask);
}

document.addEventListener("keydown", (event) => {
  if (!keyButtons.has(event.code)) {
    return;
  }
  event.preventDefault();
  pressedKeys.add(event.code);
  refreshKeyboard();
});
document.addEventListener("keyup", (event) => {
  if (!keyButtons.has(event.code)) {
    return;
  }
  event.preventDefault();
  pressedKeys.delete(event.code);
  refreshKeyboard();
});
window.addEventListener("blur", () => {
  pressedKeys.clear();
  screenPointers.clear();
  refreshKeyboard();
  refreshScreenButtons();
});

function setAxis(id, value) {
  byId(`${id}-value`).textContent = String(value);
  byId(`${id}-indicator`).style.left = `${(value + 1000) / 20}%`;
}

const sequenceGaps = new SequenceGapTracker();
let reconnects = 0;
let hasConnected = false;
let reconnectTimer = null;
let socket = null;

function releaseHardware() {
  lastSample = null;
  hardwareSession.disconnect();
  byId("ws-pill").dataset.ok = "false";
}

function scheduleReconnect() {
  if (reconnectTimer === null) {
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connectWebSocket();
    }, RECONNECT_DELAY_MS);
  }
}

function connectWebSocket() {
  releaseHardware();
  delayEstimator.reset();
  byId("ws-pill").textContent = "조이스틱 연결 중";

  socket = new WebSocket(webSocketUrl(window.location));
  socket.addEventListener("open", () => {
    if (hasConnected) {
      reconnects += 1;
      byId("reconnect-value").textContent = String(reconnects);
    }
    hasConnected = true;
    byId("ws-pill").textContent = "조이스틱 연결됨";
    byId("ws-pill").dataset.ok = "true";
  });
  socket.addEventListener("message", (event) => {
    try {
      const sample = decodeInputEvent(event.data);
      const estimatedGaps = sequenceGaps.observe(sample.seq);
      lastSample = sample;
      hardwareSession.accept(sample, { invertY: invertY.checked });
      setAxis("x", sample.x);
      setAxis("y", sample.y);
      byId("seq-value").textContent = String(sample.seq);
      byId("gap-value").textContent = String(estimatedGaps);
      byId("queue-value").textContent = `${sample.sent_ms - sample.sample_ms} ms`;
      const relativeDelay = delayEstimator.observe(performance.now(), sample.sent_ms);
      byId("transport-value").textContent = `${relativeDelay.toFixed(1)} ms`;
    } catch (error) {
      releaseHardware();
      showError(`잘못된 조이스틱 이벤트: ${error.message}`);
    }
  });
  socket.addEventListener("error", releaseHardware);
  socket.addEventListener("close", () => {
    releaseHardware();
    byId("ws-pill").textContent = "조이스틱 재연결 대기";
    scheduleReconnect();
  });
}

invertY.addEventListener("change", () => {
  if (lastSample) {
    hardwareSession.remap(lastSample, { invertY: invertY.checked });
  }
});

async function pollStatus() {
  const started = performance.now();
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const status = await response.json();
    byId("rtt-value").textContent = `${(performance.now() - started).toFixed(1)} ms`;
    byId("wifi-pill").textContent = status.wifi_ipv4 ? "Wi-Fi / DHCP 준비됨" : "Wi-Fi / DHCP 대기";
    byId("wifi-pill").dataset.ok = String(status.wifi_ipv4 === true);
    byId("sample-value").textContent = String(status.input_samples);
    byId("input-error-value").textContent = String(status.input_errors);
    byId("deadline-value").textContent = String(status.deadline_misses);
    byId("heap-value").textContent = status.heap_stats_ok
      ? `${status.heap_free_bytes} B (최저 ${status.heap_min_free_bytes} B)`
      : "측정 실패";
    byId("send-error-value").textContent = String(status.send_errors);
    byId("stale-value").textContent = String(status.stale_input_frames);
  } catch {
    byId("wifi-pill").textContent = "보드 상태 확인 실패";
    byId("wifi-pill").dataset.ok = "false";
  } finally {
    setTimeout(pollStatus, 2000);
  }
}

window.addEventListener("beforeunload", () => {
  hardwareSession.disconnect();
  socket?.close();
});

connectWebSocket();
pollStatus();

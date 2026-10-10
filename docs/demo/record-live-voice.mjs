#!/usr/bin/env node
// Record the running app with a real local WAV. Adds one demo record to its DB.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const wav = process.argv[2];
if (!wav) throw new Error('Usage: node docs/demo/record-live-voice.mjs /path/to/synthetic-ko.wav');
const origin = process.env.WALLABY_LIVE_ORIGIN || 'http://127.0.0.1:8767';
assert.ok(/^http:\/\/127\.0\.0\.1:[0-9]+$/.test(origin), 'use the local app origin');
const modulePath = process.env.WALLABY_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(resolve(modulePath)).href : 'playwright');
const build = join(root, 'build/live-voice-recording');
const media = join(root, 'docs/demo/media');
await mkdir(build, { recursive: true });
await mkdir(media, { recursive: true });
const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
let page;
try {
  // CSS only changes the recording layout. No app/API responses are replaced.
  const context = await browser.newContext({ viewport: { width: 1120, height: 900 },
    bypassCSP: true, recordVideo: { dir: build, size: { width: 1120, height: 900 } } });
  page = await context.newPage();
  const errors = [], external = [];
  page.on('pageerror', error => errors.push(String(error)));
  await page.route('**/*', route => {
    if (new URL(route.request().url()).origin !== origin) {
      external.push(route.request().url());
      return route.abort();
    }
    return route.continue();
  });
  await page.goto(origin, { waitUntil: 'networkidle' });
  await page.waitForFunction(() => !document.getElementById('audio-file').disabled);
  const initial = await page.evaluate(async () => (await fetch('/api/status')).json());
  assert.equal(initial.stt.ready, true);
  assert.equal(initial.llm.enabled, true);
  assert.equal(initial.gateway.ready, true);
  await page.addStyleTag({ content: `
    .masthead,.intro,.bottom-grid,.simulation-panel,footer,.examples,.audio-settings,.alert-panel{display:none}
    body{padding-top:152px}
    main{max-width:none;padding:0 20px 16px}.main-grid{grid-template-columns:1fr 1.15fr;gap:16px}
    .status-strip{padding:14px;margin-bottom:16px}.panel-heading{padding:16px}
    .voice-stage{padding:20px 16px}.voice-orbit{width:90px;height:90px;margin-bottom:18px}
    .voice-core{width:64px;height:64px}.voice-stage h3{font-size:17px}
    .result-scroll{height:550px;max-height:550px;min-height:0;padding:20px}
    .report-card{margin:12px 0}.report-row{padding:7px 12px}
    #recording-banner{position:fixed;z-index:10;top:12px;left:20px;right:20px;padding:16px;border:1px solid #486044;border-radius:9px;background:#1b2b20}
    #recording-banner h1{font-size:24px;letter-spacing:-.7px;margin:0 0 6px}
    #recording-banner p{margin:3px 0;color:#b1c6a7;font-size:12px}
    #recording-result{color:#b7f7a9!important;font-weight:600}
    #device-result{padding:10px;border:1px solid #486044;border-radius:5px}
  ` });
  await page.evaluate(() => {
    const banner = document.createElement('div');
    banner.id = 'recording-banner';
    banner.innerHTML = '<h1>militaryWallaby · 로컬 음성 → 실제 OLED 전송 ACK</h1>' +
      '<p>한국어 합성 WAV 입력 · 실제 Whisper + llama.cpp 실행 · 음성 오인식도 그대로 표시</p>' +
      '<p id="recording-result">상황 보고 모드 · 로컬 처리 준비</p>' +
      '<p>NU40 마이크 녹음·실제 OLED 픽셀은 이 영상의 검증 범위에 포함하지 않습니다. 영상에는 소리가 없습니다.</p>';
    document.body.prepend(banner);
  });
  await page.selectOption('#input-source', 'pc');
  await page.selectOption('#mode', 'report');
  await page.waitForTimeout(1200);
  const responsePromise = page.waitForResponse(response =>
    response.url().includes('/api/audio?mode=report') && response.request().method() === 'POST',
  { timeout: 120000 });
  await page.locator('#recording-result').evaluate(element => element.textContent = '합성 WAV 파일 입력 → 로컬 STT·LLM 처리 중');
  const started = Date.now();
  await page.setInputFiles('#audio-file', resolve(wav));
  const response = await responsePromise;
  const result = await response.json();
  assert.equal(response.status(), 200);
  assert.equal(result.engine, 'local-llm-analysis');
  assert.equal(result.device?.acknowledged, true);
  const elapsedMs = Date.now() - started;
  await page.waitForFunction(() => !document.getElementById('edit-transcript').disabled);
  await page.locator('#recording-result').evaluate((element, elapsed) => {
    element.textContent = `Whisper → Qwen → NU87 Wi-Fi → NU40 BLE ACK 완료 · ${(elapsed / 1000).toFixed(1)}초`;
  }, elapsedMs);
  await page.waitForTimeout(2500);
  await page.click('#edit-transcript');
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.waitForTimeout(2500);
  const banner = await page.locator('#recording-banner').boundingBox();
  assert.ok(banner.y >= 0 && banner.y + banner.height < 900, 'verification and scope remain visible');
  await page.screenshot({ path: join(build, 'poster.png') });
  assert.deepEqual(errors, []);
  assert.deepEqual(external, []);
  await writeFile(join(build, 'result.json'), JSON.stringify({
    input: 'synthetic Korean WAV, not human or NU40 acoustic capture',
    elapsed_ms: elapsedMs, real_models: true, matching_display_ack: true,
    visible_oled_pixels_verified: false, browser_errors: errors,
    external_browser_requests: external, result,
  }, null, 2) + '\n');
  const video = page.video();
  await context.close();
  await video.saveAs(join(build, 'live-voice.webm'));
} finally {
  await browser.close();
}
async function run(command, args) {
  await new Promise((accept, reject) => {
    const child = spawn(command, args, { stdio: ['ignore', 'ignore', 'pipe'] });
    let error = '';
    child.stderr.on('data', data => error += data);
    child.on('error', reject);
    child.on('exit', code => code === 0 ? accept() : reject(new Error(error)));
  });
}
const source = join(build, 'live-voice.webm');
const palette = join(build, 'palette.png');
await run('ffmpeg', ['-y', '-v', 'error', '-i', source, '-vf', 'fps=8,palettegen=max_colors=128', '-frames:v', '1', palette]);
await run('ffmpeg', ['-y', '-v', 'error', '-i', source, '-i', palette,
  '-lavfi', 'fps=8[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=4', '-loop', '0',
  join(media, '06-local-voice-verified.gif')]);
await run('ffmpeg', ['-y', '-v', 'error', '-i', source, '-an', '-c:v', 'libx264',
  '-pix_fmt', 'yuv420p', '-crf', '20', '-movflags', '+faststart',
  join(media, 'military-wallaby-local-voice.mp4')]);
console.log('Saved real local voice GIF and MP4; original transcript and matching OLED ACK verified.');

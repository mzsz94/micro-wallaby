#!/usr/bin/env node
// Exercise correction and voice-driven search against a real isolated server/model.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const modulePath = process.env.WALLABY_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(resolve(modulePath)).href : 'playwright');
const port = Number(process.env.WALLABY_BROWSER_TEST_PORT || 8769);
const origin = `http://127.0.0.1:${port}`;
const data = await mkdtemp(resolve(tmpdir(), 'wallaby-correction-'));
const server = spawn(process.env.WALLABY_PYTHON || 'python3', [
  '-u', '-m', 'voice_tactical.server', '--no-browser', '--port', String(port), '--data-dir', data,
], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] });
let log = '', browser;
server.stdout.on('data', value => log += value);
server.stderr.on('data', value => log += value);
try {
  let ready = false;
  for (let attempt = 0; attempt < 80; attempt++) {
    if (server.exitCode !== null) throw new Error(log);
    try { ready = (await fetch(origin + '/api/status')).ok; } catch {}
    if (ready) break;
    await delay(100);
  }
  assert.ok(ready, 'isolated test server started');
  browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
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
  await page.selectOption('#input-source', 'pc');
  await page.selectOption('#mode', 'report');
  const original = '상황 보고. 위치: 참고. 관측: 장비 점검 완료.';
  await page.fill('#text-input', original);
  await page.click('#send-text');
  await page.waitForFunction(() => document.getElementById('transcript').textContent.includes('위치: 참고'));
  await page.waitForFunction(() => !document.getElementById('edit-transcript').disabled);
  const records = async () => (await (await fetch(origin + '/api/records')).json()).records;
  const before = await records();
  assert.equal(before.length, 1);
  await page.click('#edit-transcript');
  assert.equal(await page.inputValue('#text-input'), original);
  assert.equal(await page.inputValue('#mode'), 'report');
  assert.equal((await records()).length, 1, 'copying for review does not execute or save');
  const corrected = original.replace('위치: 참고', '위치: 창고');
  await page.fill('#text-input', corrected);
  await page.click('#send-text');
  await page.waitForFunction(() => document.getElementById('transcript').textContent.includes('위치: 창고'));
  const after = await records();
  assert.equal(after.length, 2);
  assert.ok(after.some(record => record.id === before[0].id && record.transcript === original));
  assert.ok(after.some(record => record.transcript === corrected && record.kind === 'report' && record.motion === null));
  const simulator = page.frameLocator('#simulator');
  await page.selectOption('#mode', 'auto');
  await page.fill('#text-input', '훈련구역탐색 시작해.');
  await page.click('#send-text');
  await page.waitForFunction(() => document.getElementById('sim-status').textContent.includes('훈련 구역 탐색 실행'));
  await simulator.locator('#state').waitFor();
  await page.waitForFunction(() => document.getElementById('simulator').contentDocument.getElementById('state').textContent === 'TRAINING_SEARCH');
  await page.waitForFunction(() => parseFloat(document.getElementById('simulator').contentDocument.getElementById('linear').textContent) > .05);
  await delay(1800);
  const pose = await simulator.locator('#pose').textContent();
  assert.notEqual(pose, 'X 1.00 m   Y 1.20 m   θ 0.0°');
  assert.notEqual(await simulator.locator('#headYaw-value').textContent(), '0°');
  assert.ok((await page.textContent('#answer')).includes('네 지점'));
  await page.click('#sim-stop');
  await page.waitForFunction(() => document.getElementById('simulator').contentDocument.getElementById('state').textContent === 'IDLE');
  const stoppedPose = await simulator.locator('#pose').textContent();
  await delay(1000);
  assert.equal(await simulator.locator('#pose').textContent(), stoppedPose);
  await page.selectOption('#mode', 'query');
  await page.fill('#text-input', '훈련구역탐색 시작해.');
  await page.click('#send-text');
  await page.waitForFunction(() => document.getElementById('answer').textContent.includes('자동 분류'));
  assert.equal(await simulator.locator('#state').textContent(), 'IDLE');
  await page.selectOption('#mode', 'auto');
  await page.click('#send-text');
  await page.waitForFunction(() => document.getElementById('simulator').contentDocument.getElementById('state').textContent === 'TRAINING_SEARCH');
  await page.click('#trigger-alert');
  await page.waitForFunction(() => document.getElementById('simulator').contentDocument.getElementById('state').textContent === 'IDLE');
  assert.equal(await simulator.locator('.voice-alert-banner').isVisible(), true);
  await page.click('#clear-alert');
  await page.waitForFunction(() => !document.getElementById('clear-alert').disabled && !document.getElementById('send-text').disabled);
  await page.uncheck('#motion-enabled');
  await page.click('#send-text');
  await page.waitForFunction(() => document.getElementById('notice').textContent.includes('동작 시뮬레이션이 꺼져'));
  assert.equal(await simulator.locator('#state').textContent(), 'IDLE');
  assert.deepEqual(errors, []);
  assert.deepEqual(external, []);
  console.log('PASS: review copies the transcript, preserves report mode and original history, and saves only after explicit resubmission.');
  console.log('PASS: exact training-search input moves the model and scanning joints; stop/alert halt it, and explicit query/disabled motion never drive.');
} finally {
  await browser?.close();
  server.kill('SIGTERM');
  await new Promise(resolve => server.exitCode !== null ? resolve() : server.once('exit', resolve));
  await rm(data, { recursive: true, force: true });
}

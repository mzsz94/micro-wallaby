#!/usr/bin/env node
// Replay every GIF scenario through the real local API and kinematic model.
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
const port = Number(process.env.WALLABY_BROWSER_TEST_PORT || 8770);
const origin = `http://127.0.0.1:${port}`;
const data = await mkdtemp(resolve(tmpdir(), 'wallaby-replay-'));
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
  assert.ok(ready, 'isolated scenario server started');
  browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  const errors = [], external = [], seen = new Set();
  page.on('pageerror', error => errors.push(String(error)));
  await page.route('**/*', route => {
    if (new URL(route.request().url()).origin !== origin) {
      external.push(route.request().url()); return route.abort();
    }
    return route.continue();
  });
  await page.goto(origin + '/demo/?play=1', { waitUntil: 'networkidle' });
  await page.waitForFunction(() => !document.getElementById('play-all').disabled);
  const snapshot = () => page.evaluate(() => document.getElementById('robot-frame').contentWindow.wallabyDemo.snapshot());
  const waiting = await snapshot();
  await page.waitForTimeout(800);
  assert.deepEqual((await snapshot()).pose, waiting.pose, 'opening the player never starts movement');
  assert.equal((await snapshot()).velocity.linear, 0);
  await page.bringToFront();
  await page.click('#play-all');
  const deadline = Date.now() + 80000;
  while (Date.now() < deadline) {
    const status = await page.textContent('#play-status');
    const number = await page.textContent('#scene-number');
    if (!seen.has(number)) { seen.add(number); console.log('Replay ' + number); }
    assert.equal(await page.locator('#play-status').evaluate(node => node.classList.contains('error')), false, status);
    if (status.includes('시연 완료')) break;
    await delay(250);
  }
  assert.ok((await page.textContent('#play-status')).includes('시연 완료'), 'all five scenes finish');
  assert.equal(seen.size, 5);
  assert.equal(await page.textContent('#result-badge'), 'RECEIVED / 정지 유지');
  const { records } = await (await fetch(origin + '/api/records')).json();
  for (const kind of ['query', 'report', 'orders', 'alert', 'motion']) {
    assert.ok(records.some(record => record.kind === kind), 'real API record for ' + kind);
  }
  assert.ok(records.some(record => record.kind === 'report' && record.transcript.includes('모의 폭발물 표식 발견')));
  assert.equal((await (await fetch(origin + '/api/alerts')).json()).active, true);
  // Replaying after the last scene explicitly resets its simulated warning.
  await page.selectOption('#play-scene', '0');
  await page.click('#play-one');
  await page.waitForTimeout(1600);
  assert.equal((await (await fetch(origin + '/api/alerts')).json()).active, false);
  await page.click('#play-stop');
  await page.waitForFunction(() => !document.getElementById('play-all').disabled);
  const stopped = await snapshot();
  await page.waitForTimeout(1000);
  const after = await snapshot();
  assert.equal(stopped.velocity.linear, 0);
  assert.equal(stopped.velocity.angular, 0);
  assert.deepEqual(after.pose, stopped.pose);
  assert.deepEqual(after.joints, stopped.joints);
  assert.deepEqual(errors, []);
  assert.deepEqual(external, []);
  console.log('PASS: all five scenarios replay with real API records; discovery report is stored, replay resets only the synthetic warning, and stop holds pose/joints.');
} finally {
  await browser?.close();
  server.kill('SIGTERM');
  await new Promise(resolve => server.exitCode !== null ? resolve() : server.once('exit', resolve));
  await rm(data, { recursive: true, force: true });
}

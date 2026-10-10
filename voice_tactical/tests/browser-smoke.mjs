#!/usr/bin/env node
// Exercise transcript correction against an isolated real HTTP server/database.
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
  assert.deepEqual(errors, []);
  assert.deepEqual(external, []);
  console.log('PASS: review copies the transcript, preserves report mode and original history, and saves only after explicit resubmission.');
} finally {
  await browser?.close();
  server.kill('SIGTERM');
  await new Promise(resolve => server.exitCode !== null ? resolve() : server.once('exit', resolve));
  await rm(data, { recursive: true, force: true });
}

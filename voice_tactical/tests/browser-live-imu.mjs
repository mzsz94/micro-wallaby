#!/usr/bin/env node
// Read-only smoke check against the running real NU87 / NU40 application.
// No test server, mocks, injected sensor values, commands or firmware writes.
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
const modulePath = process.env.WALLABY_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(resolve(modulePath)).href : 'playwright');
const origin = process.env.WALLABY_LIVE_URL || 'http://127.0.0.1:8767';
assert.match(origin, /^http:\/\/(127\.0\.0\.1|localhost):\d+$/);
const output = resolve('build/live-imu-verification');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
const page = await browser.newPage({ viewport: { width: 1280, height: 990 } });
const errors = [];
page.on('pageerror', error => errors.push(String(error)));
try {
  await page.goto(origin + '/hardware.html');
  await page.waitForFunction(() => document.getElementById('connection').textContent.includes('LIVE'));
  await page.waitForFunction(() => document.getElementById('ack').textContent.includes('드라이버 ACK'));
  const samples = [];
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline && new Set(samples).size < 5) {
    const state = await page.evaluate(async () => (await fetch('/api/imu/status')).json());
    assert.equal(state.source, 'nu87-i2c');
    if (state.fresh) {
      assert.equal(state.packet.whoAmI, 106);
      assert.equal(state.packet.errors, 0);
      assert.ok(state.display_orientation?.gravity_valid);
      samples.push(state.packet.sample);
    }
    await delay(250);
  }
  assert.ok(new Set(samples).size >= 5, 'A changing real sensor sequence is required.');
  assert.ok(await page.locator('#simulator').contentFrame().locator('#state').textContent());
  assert.deepEqual(errors, []);
  await page.screenshot({ path: join(output, 'live-smoothed.png'), fullPage: true });
  const result = { passed: true, real_http_source: 'nu87-i2c', observed_sequences: samples,
    oled_driver_ack_visible: true, browser_errors: errors, checked_at: new Date().toISOString(),
    scope: 'Read-only live browser check. OLED pixels and physical movement require separate observation.' };
  await writeFile(join(output, 'browser-result.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify(result));
} finally { await browser.close(); }

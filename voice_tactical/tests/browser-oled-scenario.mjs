#!/usr/bin/env node
// Opt-in bench check: real NU87 HTTP input and NU40 ACKs, with deliberate UI
// commands. No mock server, sensor injection, replay or clock replacement.
// --calibrate requires the physical sensor to be stationary on the desk.
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

const modulePath = process.env.WALLABY_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(resolve(modulePath)).href : 'playwright');
const origin = process.env.WALLABY_LIVE_URL || 'http://127.0.0.1:8767';
assert.match(origin, /^http:\/\/(127\.0\.0\.1|localhost):\d+$/);
const output = resolve('build/graphics-validation/scenario');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
const context = await browser.newContext({ viewport: { width: 1280, height: 990 },
  ...(process.argv.includes('--record') ? { recordVideo: {
    dir: output, size: { width: 1280, height: 990 },
  } } : {}),
});
const page = await context.newPage();
const errors = [], checkpoints = [];
page.on('pageerror', error => errors.push(String(error)));
async function state() {
  return page.evaluate(async () => (await fetch('/api/imu/status')).json());
}
async function until(predicate, message, timeout = 25_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const current = await state();
    if (predicate(current)) return current;
    await delay(150);
  }
  throw new Error(message);
}
function record(kind, s) {
  assert.equal(s.source, 'nu87-i2c');
  assert.equal(s.packet.whoAmI, 106);
  assert.equal(s.packet.errors, 0);
  checkpoints.push({ kind, at: new Date().toISOString(), fresh: s.fresh,
    packet: s.packet, orientation: s.orientation, reference: s.reference,
    oled: s.oled, motion_blocked: s.motion_blocked });
  console.log(kind, s.packet.sample);
}
let videoPath;
try {
  await page.goto(origin + '/hardware.html');
  await until(s => s.fresh, 'Fresh physical IMU input is required.');
  if (process.argv.includes('--calibrate')) {
    await page.locator('#reference').click();
    await until(s => s.reference && !s.calibrating,
      'Stationary calibration did not complete; inspect the actual sensor.', 26_000);
  }
  const calibrated = await until(s => s.fresh && s.reference && !s.motion_blocked,
    'Set a stationary reference before running the virtual-driving check.');
  record('reference', calibrated);
  for (const view of ['face', 'horizon', 'text']) {
    await page.locator(`[data-view="${view}"]`).click();
    const current = await until(s => s.fresh && s.display_view === view &&
      s.oled.acknowledged && s.oled.sample > calibrated.packet.sample &&
      (view === 'text' ? !s.oled.scene : s.oled.scene?.view === view),
    `No matching real OLED ACK for ${view}.`);
    record(view, current);
    await page.screenshot({ path: join(output, `${view}.png`), fullPage: true });
    await delay(1800);
  }
  await page.locator('#patrol').click();
  await page.waitForFunction(() => document.querySelector('#simulator').contentDocument
    ?.getElementById('state').textContent === 'TRAINING_SEARCH', null, { timeout: 15_000 });
  const sim = page.frameLocator('#simulator');
  const startPose = await sim.locator('#pose').textContent();
  await page.locator('#simulator').scrollIntoViewIfNeeded();
  await delay(5000);
  const endPose = await sim.locator('#pose').textContent();
  assert.notEqual(endPose, startPose, 'The virtual patrol must actually change XY/heading.');
  await page.screenshot({ path: join(output, 'virtual-patrol.png'), fullPage: true });
  await page.locator('#stop').click();
  await sim.locator('#arena').click();
  const beforeManual = await sim.locator('#pose').textContent();
  await page.keyboard.down('w');
  await delay(800);
  await page.keyboard.up('w');
  const afterManual = await sim.locator('#pose').textContent();
  assert.notEqual(afterManual, beforeManual, 'Holding W in the arena must move the virtual model.');
  await page.keyboard.press('Space');
  await delay(300);
  assert.match(await sim.locator('#linear').textContent(), /^0\.000/);
  assert.match(await sim.locator('#angular').textContent(), /^0\.00/);
  await page.locator('[data-view="face"]').click();
  const final = await until(s => s.fresh && s.oled.acknowledged && s.oled.scene?.view === 'face',
    'Final face scene must be acknowledged.');
  record('final', final);
  assert.deepEqual(errors, []);
  const result = { passed: true, checked_at: new Date().toISOString(), checkpoints,
    virtual_patrol: { start_pose: startPose, end_pose: endPose, changed: true },
    manual_drive: { start_pose: beforeManual, end_pose: afterManual, changed: true },
    space_stop_checked: true, browser_errors: errors,
    generated_sensor_values: false, physical_oled_pixels_verified_by_script: false,
    scope: 'Actual I2C input and matching BLE OLED-driver ACKs; driving is a virtual model.' };
  await writeFile(join(output, 'result.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify({ passed: true, checkpoints: checkpoints.length,
    virtual_patrol_changed: true, browser_errors: errors }));
} finally {
  await page.keyboard.up('w').catch(() => {});
  await page.request.post(origin + '/api/control/stop', { data: {},
    headers: { Origin: origin } }).catch(() => {});
  const video = page.video();
  await context.close();
  if (video) videoPath = await video.path();
  await browser.close();
}
if (videoPath) console.log('Recording:', videoPath);

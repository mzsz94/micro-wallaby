#!/usr/bin/env node
// Record wall-clock browser frames and real hardware HTTP samples. No mocks,
// fake timers, sensor injection, transcript staging or canvas replacement.
import { mkdir, writeFile, copyFile } from 'node:fs/promises';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { spawn } from 'node:child_process';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const modulePath = process.env.WALLABY_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(resolve(modulePath)).href : 'playwright');
const origin = process.env.WALLABY_LIVE_URL || 'http://127.0.0.1:8767';
if (!/^http:\/\/(127\.0\.0\.1|localhost):\d+$/.test(origin)) throw new Error('Use the existing loopback hardware server.');
const seconds = Number(process.env.WALLABY_LIVE_SECONDS || 180);
if (!Number.isFinite(seconds) || seconds < 10 || seconds > 600) throw new Error('Record 10–600 real seconds.');
const directory = resolve(process.env.WALLABY_LIVE_OUTPUT || join(root, 'build/live-imu-recording'));
await mkdir(directory, { recursive: true });
async function run(command, args) {
  return new Promise((accept, reject) => {
    const process = spawn(command, args, { stdio: ['ignore', 'ignore', 'pipe'] });
    let error = ''; process.stderr.on('data', value => error += value);
    process.on('error', reject); process.on('exit', code => code ? reject(new Error(error)) : accept());
  });
}
const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
const context = await browser.newContext({ viewport: { width: 1280, height: 990 }, recordVideo: { dir: directory, size: { width: 1280, height: 990 } } });
const page = await context.newPage();
const browserErrors = [], packets = [];
page.on('pageerror', error => browserErrors.push(String(error)));
let video, result;
try {
  await page.goto(origin + '/hardware.html');
  await page.waitForFunction(() => document.getElementById('connection').textContent.includes('LIVE'), { timeout: 15_000 });
  console.log('READY: real sensor page recording; move the physical IMU, then return it to its reference pose.');
  await writeFile(join(directory, 'ready.json'), JSON.stringify({ ready: true, at: new Date().toISOString(), origin }));
  const started = performance.now(), startedAt = Date.now() / 1000;
  let clearClicked = false, clearObservedAt = null, patrolStarted = false;
  while (performance.now() - started < seconds * 1000) {
    const state = await (await fetch(origin + '/api/imu/status')).json();
    packets.push({ elapsed_ms: Math.round(performance.now() - started), fresh: state.fresh,
      packet: state.packet, orientation: state.orientation, reference: state.reference,
      tilt_latched: state.tilt_latched, motion_blocked: state.motion_blocked,
      oled: { acknowledged: state.oled.acknowledged, sample: state.oled.sample, text: state.oled.text }, events: state.events,
      robot_stop: state.robot_stop });
    if (!patrolStarted && state.reference && !state.motion_blocked && !state.tilt_latched) {
      await page.locator('#patrol').click(); patrolStarted = true;
    }
    if (state.tilt_latched && state.fresh && state.orientation?.tilt < 15 && !clearClicked) {
      await page.locator('#clear').click(); clearClicked = true;
    }
    // A button click is not proof of a successful clear. Keep recording the
    // real post-clear packet and persisted event before accepting this step.
    if (clearClicked && state.fresh && !state.tilt_latched && state.events.some(e => e.kind === 'clear' && e.at >= startedAt)) {
      clearObservedAt ??= performance.now();
      if (performance.now() - clearObservedAt > 2500 && performance.now() - started > 15_000) break;
    }
    await delay(220);
  }
  const good = packets.filter(p => p.fresh && p.packet && p.orientation?.gravity_valid);
  const sequences = good.map(p => p.packet.sample), angles = good.map(p => p.orientation.tilt);
  result = { schema: 'military-wallaby-real-hardware-recording/v1', recorded_at: new Date().toISOString(),
    hardware_source: 'NU87 I2C LSM6DS3TR-C via authenticated LAN HTTP', real_time_recording: true,
    generated_sensor_samples: false, staged_speech: false, browser_errors: browserErrors,
    sample_sequence_advanced: new Set(sequences).size > 5,
    sensor_errors: Math.max(0, ...good.map(p => p.packet.errors)),
    observed_tilt_range_degrees: angles.length ? [Math.min(...angles), Math.max(...angles)] : null,
    physical_tilt_warning_observed: packets.some(p => p.tilt_latched),
    oled_driver_ack_observed: packets.some(p => p.oled.acknowledged),
    oled_pixels_verified: false, return_and_clear_observed: clearObservedAt !== null, virtual_patrol_requested: patrolStarted,
    limitations: 'IMU attitude is real; Wallaby movement is a virtual model. No absolute yaw/location or real motor validation. OLED ACK does not verify visible pixels.', packets };
  await page.locator('#stop').click();
  await page.screenshot({ path: join(directory, 'live-imu-final.png'), fullPage: true });
  await writeFile(join(directory, 'live-imu-evidence.json'), JSON.stringify(result, null, 2) + '\n');
  video = page.video();
} finally { await context.close(); await browser.close(); }
const webm = await video.path();
await run('ffmpeg', ['-y', '-v', 'error', '-i', webm, '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '24', '-movflags', '+faststart', join(directory, 'live-imu.mp4')]);
await run('ffmpeg', ['-y', '-v', 'error', '-i', webm, '-vf', 'fps=6,scale=960:-2:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=96[p];[s1][p]paletteuse=dither=bayer', '-loop', '0', join(directory, 'live-imu.gif')]);
const verified = result.sample_sequence_advanced && result.physical_tilt_warning_observed && result.oled_driver_ack_observed && result.return_and_clear_observed && !browserErrors.length;
console.log(JSON.stringify({ verified, output: directory, sample_sequence_advanced: result.sample_sequence_advanced,
  physical_tilt_warning_observed: result.physical_tilt_warning_observed, oled_driver_ack_observed: result.oled_driver_ack_observed,
  return_and_clear_observed: result.return_and_clear_observed, observed_tilt_range_degrees: result.observed_tilt_range_degrees }));
if (process.argv.includes('--publish')) {
  if (!verified) throw new Error('Physical sequence incomplete: keeping recording in build; nothing published.');
  const media = join(root, 'docs/demo/media');
  await copyFile(join(directory, 'live-imu.mp4'), join(media, 'military-wallaby-live-imu.mp4'));
  await copyFile(join(directory, 'live-imu.gif'), join(media, '07-live-imu.gif'));
  // Keep full continuous evidence in build, and a reviewable set of unchanged
  // real samples publicly. Include a stale sample too, if one was observed.
  const { packets: all, ...summary } = result;
  const highlights = [all[0], all.at(-1), all.find(p => !p.fresh),
    all.find(p => p.orientation?.tilt > 8), all.find(p => p.orientation?.tilt >= 35),
    all.find(p => p.tilt_latched),
    all.reduce((peak, p) => (p.orientation?.tilt ?? -1) > (peak?.orientation?.tilt ?? -1) ? p : peak, null)];
  const selected = [...new Set(highlights.filter(Boolean))].sort((a, b) => a.elapsed_ms - b.elapsed_ms);
  const excerpts = selected.map(({ events, ...sample }) => sample);
  const events = [...new Map(all.flatMap(p => p.events).map(e => [e.at, e])).values()];
  await writeFile(join(root, 'docs/demo/live-imu-evidence.json'), JSON.stringify({ ...summary,
    capture_summary: { total_observations: all.length, distinct_sequences: new Set(all.map(p => p.packet?.sample)).size,
      observed_duration_ms: all.at(-1).elapsed_ms, full_continuous_capture: 'build/live-imu-recording/live-imu-evidence.json' },
    sample_excerpts: excerpts, observed_events: events }, null, 2) + '\n');
}

import test from 'node:test';
import assert from 'node:assert/strict';
import { Simulator } from './model.mjs';
import { createVoiceController } from './voice-control.mjs';
import { createDemoController } from './demo-control.mjs';

function fixture() {
  let time = 0, visible = true, callback;
  const sim = new Simulator();
  const voice = createVoiceController(sim, { now: () => time, schedule: () => 0, cancel: () => {} });
  const demo = createDemoController(sim, voice, {
    now: () => time, canRun: () => visible,
    schedule: fn => { callback = fn; return 1; }, cancel: () => { callback = null; },
  });
  return { sim, voice, demo, elapse(ms) { time += ms; demo.tick(); }, hide() { visible = false; }, fire() { callback?.(); } };
}

test('demo drive changes actual pose, wheel angles and trail, never synthetic data', () => {
  const f = fixture(), before = f.sim.snapshot();
  for (let i = 0; i < 20; i++) {
    f.demo.apply({ drive: { linear: .12, angular: .3 }, joints: { headYaw: 12 } });
    f.sim.step(.1); f.elapse(100);
  }
  const after = f.demo.snapshot(), actual = f.sim.snapshot();
  assert.deepEqual(after.pose, actual.pose);
  assert.deepEqual(after.joints, actual.joints);
  assert.deepEqual(after.wheelAngles, actual.wheelAngles);
  assert.ok(after.pose.x > before.pose.x + .15);
  assert.ok(after.pose.y > before.pose.y);
  assert.ok(after.trailLength > 15);
  assert.ok(after.distanceTraveled > .15);
});

test('null and absent drive preserve voice motion and its bounded duration', () => {
  const f = fixture();
  f.voice.command('forward');
  f.demo.apply({ expression: 'listening', drive: null });
  f.demo.apply({ expression: 'thinking' });
  f.sim.step(.1);
  assert.ok(f.sim.snapshot().velocity.linear > 0);
  assert.equal(f.demo.snapshot().demoDriving, false);
  f.elapse(901); f.voice.tick();
  assert.equal(f.sim.snapshot().velocity.linear, 0);
});

test('demo drive watchdog stops with timer delivery or delayed-frame clock', () => {
  for (const mode of ['timer', 'tick']) {
    const f = fixture();
    f.demo.apply({ drive: { linear: .12, angular: .4 } }); f.sim.step(.1);
    if (mode === 'timer') f.fire(); else f.elapse(751);
    assert.equal(f.sim.snapshot().velocity.linear, 0);
    assert.equal(f.sim.snapshot().velocity.angular, 0);
    assert.equal(f.demo.snapshot().demoDriving, false);
  }
});

test('presentation-only heartbeats cannot keep an earlier staged drive alive', () => {
  const f = fixture();
  f.demo.apply({ drive: { linear: .12, angular: 0 } });
  f.elapse(500); f.demo.apply({ expression: 'happy', drive: null });
  f.elapse(251);
  assert.equal(f.demo.snapshot().demoDriving, false);
  assert.equal(f.sim.state, 'IDLE');
});

test('alert cannot be overridden by drive, joints, expression or reset', () => {
  const f = fixture();
  f.demo.apply({ drive: { linear: .12, angular: .3 }, joints: { headYaw: 25 } }); f.sim.step(.1);
  f.voice.setAlert(true); f.demo.tick();
  const before = f.sim.snapshot();
  f.demo.apply({ expression: 'happy', drive: { linear: .15, angular: 1 }, joints: { headYaw: -50, headPitch: 30 } });
  f.demo.reset(); f.sim.step(.2);
  const after = f.demo.snapshot();
  assert.equal(after.expression, 'alert'); assert.equal(after.alertActive, true);
  assert.deepEqual(after.pose, before.pose);
  assert.deepEqual(after.joints, before.joints);
  assert.equal(after.driveAccepted, false);
  f.voice.setAlert(false); f.demo.apply({ drive: { linear: .1, angular: 0 } });
  assert.equal(f.demo.snapshot().driveAccepted, true);
});

test('inactive views and model faults retain movement and joint safety', () => {
  for (const mode of ['hidden', 'estop', 'networkLoss', 'battery']) {
    const f = fixture();
    if (mode === 'hidden') f.hide();
    else if (mode === 'battery') f.sim.setBattery(10);
    else f.sim.setFault(mode, true);
    f.demo.apply({ drive: { linear: .12, angular: 1 }, joints: { headYaw: 25 } }); f.sim.step(.1);
    assert.equal(f.demo.snapshot().driveAccepted, false, mode);
    assert.equal(f.sim.snapshot().velocity.linear, 0, mode);
    assert.equal(f.sim.snapshot().velocity.angular, 0, mode);
  }
});

test('zero drive stops and mock marker is presentation-only, detached snapshot data', () => {
  const f = fixture();
  f.demo.apply({ drive: { linear: .12, angular: 0 }, marker: { x: 1.58, y: 1.52, label: '훈련 표적', detected: false } });
  f.sim.step(.1); f.demo.apply({ drive: { linear: 0, angular: 0 } });
  assert.equal(f.sim.snapshot().velocity.linear, 0);
  const s = f.demo.snapshot(); s.marker.detected = true;
  assert.equal(f.demo.snapshot().marker.detected, false);
  assert.equal(f.sim.snapshot().world.obstacles.length, 0);
  f.demo.reset();
  assert.equal(f.demo.snapshot().marker, null);
  assert.equal(f.demo.snapshot().trailLength, 1);
});

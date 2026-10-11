import assert from 'node:assert/strict';
import test from 'node:test';
import { createLiveImuControl } from '../live-imu-control.mjs';
import { Simulator } from '../model.mjs';
const measured = () => ({ enabled: true, source: 'nu87-i2c', fresh: true, reference: [0,0,1], motion_blocked: false, packet: { sample: 1 }, orientation: { gravity_valid: true, roll: 10, pitch: -8, tilt: 13 } });
test('physical attitude changes joints, never planar position', () => {
  const sim = new Simulator(), imu = createLiveImuControl(sim), before = sim.snapshot().pose;
  assert.equal(imu.update(measured()), true);
  imu.tick();
  for (let i = 0; i < 30; i++) sim.step(.02);
  assert.equal(sim.snapshot().joints.headPitch, -8);
  assert.ok(Math.abs(sim.snapshot().joints.waistYaw - 10) < .001);
  assert.deepEqual(sim.snapshot().pose, before);
});
test('reject synthetic sources and nonfinite orientation', () => {
  const sim = new Simulator(), imu = createLiveImuControl(sim);
  assert.equal(imu.update({ ...measured(), source: 'replay' }), false);
  assert.equal(imu.update({ ...measured(), orientation: { roll: NaN, pitch: 2 } }), false);
});
test('tilt hold and stale parent stop actual kinematic movement', () => {
  let time = 0;
  const sim = new Simulator(), imu = createLiveImuControl(sim, { now: () => time });
  sim.setManual(.1, 0);
  imu.update({ ...measured(), motion_blocked: true });
  assert.equal(imu.blocked, true);
  assert.equal(sim.snapshot().state, 'IDLE');
  imu.update(measured()); sim.setManual(.1, 0);
  time = 2100; imu.tick();
  assert.equal(imu.blocked, true);
  assert.equal(sim.snapshot().state, 'IDLE');
});
test('manual movement remains stopped throughout a hardware hold', () => {
  const sim = new Simulator(), imu = createLiveImuControl(sim);
  imu.update({ ...measured(), motion_blocked: true });
  sim.setManual(.1, 0);
  imu.tick();
  assert.equal(sim.snapshot().state, 'IDLE');
});

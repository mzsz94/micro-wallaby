import test from 'node:test';
import assert from 'node:assert/strict';
import { createImuRelay } from './imu-relay.mjs';
import { Simulator } from '../../simulation/model.mjs';
import { createVoiceController } from '../../simulation/voice-control.mjs';
import { createLiveImuControl } from '../../simulation/live-imu-control.mjs';

function fixture() {
  let now = 0;
  const sim = new Simulator();
  const imu = createLiveImuControl(sim, { now: () => now });
  const voice = createVoiceController(sim, { now: () => now,
    schedule: () => 1, cancel: () => {}, canRun: () => !imu.blocked });
  const relay = createImuRelay(message => {
    if (message.type === 'wallaby-imu') imu.update(message.state);
    else voice.command(message.command);
  });
  const fresh = { source: 'nu87-i2c', enabled: true, fresh: true, motion_blocked: false,
    packet: { sample: 1 }, reference: [0, 0, 1],
    orientation: { gravity_valid: true, roll: 0, pitch: 0, tilt: 0 } };
  return { sim, imu, voice, relay, fresh, advance: ms => { now += ms; imu.tick(); voice.tick(); } };
}

test('a later measured tilt or stale snapshot cancels voice-started patrol', () => {
  for (const change of [{ motion_blocked: true, tilt_latched: true }, { fresh: false }]) {
    const f = fixture();
    f.relay.accept(f.fresh);
    assert.equal(f.voice.command('patrol'), true);
    assert.ok(f.voice.patrolState);
    f.relay.accept({ ...f.fresh, ...change });
    assert.equal(f.relay.blocked, true);
    assert.equal(f.voice.patrolState, null);
    assert.equal(f.voice.command('patrol'), false);
    f.relay.accept({ ...f.fresh, packet: { sample: 2 } });
    f.advance(100);
    assert.equal(f.voice.patrolState, null, 'a fresh sample must not resume the old patrol');
  }
});

test('server connection loss blocks existing IMU motion and replay preserves the hold', () => {
  const f = fixture();
  f.relay.accept(f.fresh);
  f.voice.command('patrol');
  f.relay.disconnected();
  f.relay.replay();
  assert.equal(f.imu.blocked, true);
  assert.equal(f.voice.patrolState, null);
});

test('sensor-disabled local demo remains usable and missing updates expire in simulator', () => {
  const f = fixture();
  assert.equal(f.relay.accept({ ...f.fresh, enabled: false }), true);
  f.relay.disconnected();
  assert.equal(f.voice.command('patrol'), true);
  f.relay.accept(f.fresh);
  f.advance(2001);
  assert.equal(f.voice.patrolState, null);
  assert.equal(f.imu.blocked, true);
});

test('explicitly disabled IMU clears a previous hold and watchdog without resuming patrol', () => {
  const f = fixture();
  f.relay.accept(f.fresh);
  f.voice.command('patrol');
  f.relay.disconnected();
  f.relay.accept({ ...f.fresh, enabled: false, motion_blocked: true });
  assert.equal(f.relay.blocked, false);
  assert.equal(f.imu.blocked, false);
  assert.equal(f.voice.patrolState, null);
  f.advance(3000);
  assert.equal(f.imu.blocked, false, 'disabled mode must remove the previous sensor watchdog');
  assert.equal(f.voice.command('patrol'), true, 'a new explicit local-demo command remains available');
  f.relay.accept({ ...f.fresh, enabled: false });
  assert.ok(f.voice.patrolState, 'repeated disabled snapshots must not keep stopping local demo');
});

test('notify only on a first blocked transition, including connection loss', () => {
  let notices = 0;
  const relay = createImuRelay(() => {}, { onBlocked: () => notices++ });
  const state = { source: 'nu87-i2c', enabled: true, fresh: true, motion_blocked: false };
  relay.accept(state);
  relay.accept({ ...state, motion_blocked: true });
  relay.accept({ ...state, motion_blocked: true });
  relay.replay();
  relay.disconnected();
  assert.equal(notices, 1);
  relay.accept(state);
  relay.disconnected();
  assert.equal(notices, 2);
  relay.accept({ ...state, enabled: false });
  relay.disconnected();
  assert.equal(notices, 2);
});

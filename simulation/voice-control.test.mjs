import test from 'node:test';
import assert from 'node:assert/strict';
import { Simulator } from './model.mjs';
import { createVoiceController, isParentMessage } from './voice-control.mjs';

function fixture() {
  let time = 0;
  let callback;
  const results = [];
  const sim = new Simulator();
  const control = createVoiceController(sim, {
    now: () => time, schedule: fn => { callback = fn; return 1; }, cancel: () => { callback = null; },
    onResult: result => results.push(result),
  });
  return { sim, control, results, elapse(ms) { time += ms; control.tick(); }, fire() { callback?.(); },
    advance(ms) {
      for (let step = 0; step < ms; step += 20) {
        time += 20; control.heartbeat(); control.tick(); sim.step(.02);
      }
    },
  };
}
test('voice drive stops within 900ms, even if timer delivery is delayed', () => {
  const f = fixture();
  assert.equal(f.control.command('forward'), true);
  f.sim.step(.1);
  assert.ok(f.sim.snapshot().velocity.linear > 0);
  f.elapse(901);
  assert.equal(f.sim.snapshot().velocity.linear, 0);
  assert.equal(f.sim.state, 'IDLE');
});
test('new command cancels the old movement deadline and timer', () => {
  const f = fixture();
  f.control.command('forward');
  f.control.command('dock');
  f.elapse(1000);
  f.fire();
  assert.equal(f.sim.state, 'DOCK_SEARCH');
});
test('parent heartbeat loss stops autonomous voice action', () => {
  const f = fixture();
  f.control.command('dock');
  f.elapse(2000);
  f.control.heartbeat();
  f.elapse(2000);
  assert.equal(f.sim.state, 'DOCK_SEARCH');
  f.elapse(501);
  assert.equal(f.sim.state, 'IDLE');
});
test('rejected command does not remove watchdog from the earlier action', () => {
  const f = fixture();
  f.control.command('dock');
  assert.equal(f.control.command('greet'), false);
  f.elapse(2501);
  assert.equal(f.sim.state, 'IDLE');
});
test('faults and invalid commands cannot bypass existing motion safety', () => {
  const f = fixture();
  f.sim.setFault('estop', true);
  assert.equal(f.control.command('forward'), false);
  assert.equal(f.control.command('greet'), false);
  assert.equal(f.control.command('clearFault'), false);
  assert.equal(f.control.command('stop'), true);
});
test('bridge accepts only the same-origin parent', () => {
  const parent = {};
  const base = { source: parent, origin: 'http://localhost:8765', data: { type: 'wallaby-voice-command' } };
  assert.equal(isParentMessage(base, parent, base.origin), true);
  assert.equal(isParentMessage({ ...base, origin: 'https://example.org' }, parent, base.origin), false);
  assert.equal(isParentMessage({ ...base, source: {} }, parent, base.origin), false);
  assert.equal(isParentMessage({ ...base, data: null }, parent, base.origin), false);
});
test('warning latches a stop and rejects movement until explicit clear', () => {
  const f = fixture();
  f.control.command('forward');
  f.sim.step(.1);
  f.control.setAlert(true);
  assert.equal(f.sim.snapshot().velocity.linear, 0);
  for (const command of ['forward', 'back', 'left', 'right', 'dock', 'greet', 'patrol']) assert.equal(f.control.command(command), false);
  assert.equal(f.control.command('stop'), true);
  f.control.setAlert(false);
  assert.equal(f.control.command('greet'), true);
});

test('training search traverses four waypoints, scans joints, and completes at rest', () => {
  const f = fixture();
  const start = { ...f.sim.pose };
  assert.equal(f.control.command('patrol'), true);
  const initial = f.control.patrolState;
  assert.equal(initial.route.length, 4);
  initial.route[0].x = -999;
  assert.ok(f.control.patrolState.route[0].x > 0, 'diagnostics cannot mutate the route');
  f.advance(2500);
  assert.ok(f.sim.pose.x > start.x + .2);
  assert.notEqual(f.sim.joints.headYaw, 0);
  assert.notEqual(f.sim.wheelAngles.left, 0);
  f.advance(50000);
  assert.equal(f.control.patrolState, null);
  assert.equal(f.results.at(-1).reason, 'patrol complete');
  assert.equal(f.sim.state, 'IDLE');
  assert.equal(f.sim.velocity.linear, 0);
  assert.equal(f.sim.velocity.angular, 0);
  assert.ok(Math.hypot(f.sim.pose.x - start.x, f.sim.pose.y - start.y) < .06);
});

test('stop and warning cancel a patrol without automatic resumption', () => {
  for (const warning of [false, true]) {
    const f = fixture();
    f.control.command('patrol'); f.advance(1000);
    if (warning) f.control.setAlert(true);
    else f.control.command('stop');
    const stopped = f.sim.snapshot();
    f.control.setAlert(false); f.advance(3000);
    assert.equal(f.control.patrolState, null);
    assert.deepEqual(f.sim.pose, stopped.pose);
    assert.deepEqual(f.sim.joints, stopped.joints);
  }
});

test('patrol heartbeat loss and deadline stop even when timers are delayed', () => {
  for (const deadline of [false, true]) {
    const f = fixture();
    f.control.command('patrol'); f.advance(1000);
    if (deadline) { f.elapse(59001); f.control.heartbeat(); }
    else f.elapse(2501);
    assert.equal(f.control.patrolState, null);
    assert.equal(f.sim.state, 'IDLE');
    assert.equal(f.sim.velocity.linear, 0);
    assert.equal(f.results.at(-1).reason, deadline ? 'duration' : 'parent disconnected');
  }
});

test('patrol respects obstacle faults and a replacement command cancels its route', () => {
  const f = fixture();
  f.sim.reset('blocked');
  f.control.command('patrol'); f.advance(10000);
  assert.equal(f.sim.state, 'FAULT');
  assert.equal(f.control.patrolState, null);
  assert.equal(f.sim.velocity.linear, 0);
  f.sim.reset();
  f.control.command('patrol'); f.advance(1000);
  assert.equal(f.control.command('dock'), true);
  assert.equal(f.control.patrolState, null);
  f.elapse(1000); f.fire();
  assert.equal(f.sim.state, 'DOCK_SEARCH');
});

test('an inactive view cancels patrol and does not resume when active again', () => {
  let running = true;
  const sim = new Simulator();
  const control = createVoiceController(sim, { canRun: () => running });
  assert.equal(control.command('patrol'), true);
  control.tick(); sim.step(.1);
  running = false; control.tick();
  const stopped = sim.snapshot();
  assert.equal(control.patrolState, null);
  assert.equal(stopped.velocity.linear, 0);
  running = true; control.tick(); sim.step(.1);
  assert.deepEqual(sim.pose, stopped.pose);
});

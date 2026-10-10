import test from 'node:test';
import assert from 'node:assert/strict';
import { Simulator } from './model.mjs';
import { createVoiceController, isParentMessage } from './voice-control.mjs';

function fixture() {
  let time = 0;
  let callback;
  const sim = new Simulator();
  const control = createVoiceController(sim, {
    now: () => time, schedule: fn => { callback = fn; return 1; }, cancel: () => { callback = null; },
  });
  return { sim, control, elapse(ms) { time += ms; control.tick(); }, fire() { callback?.(); } };
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
  for (const command of ['forward', 'back', 'left', 'right', 'dock', 'greet']) assert.equal(f.control.command(command), false);
  assert.equal(f.control.command('stop'), true);
  f.control.setAlert(false);
  assert.equal(f.control.command('greet'), true);
});

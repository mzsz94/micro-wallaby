import assert from 'node:assert/strict';
import test from 'node:test';
import { createAttitudeView } from '../attitude-view.mjs';
const angle = roll => ({ roll, pitch: 0, tilt: Math.abs(roll), gravity_valid: true });
test('display interpolates measured changes without changing source values', () => {
  const view = createAttitudeView(), original = angle(40);
  view.update(angle(0)); view.tick(0); view.update(original);
  const first = view.tick(16), next = view.tick(32);
  assert.ok(first.roll > 0 && first.roll < 10);
  assert.ok(next.roll > first.roll && next.roll < 40);
  assert.equal(original.roll, 40);
});
test('roll wraps through 180 degrees rather than spinning the other way', () => {
  const view = createAttitudeView();
  view.update(angle(179)); view.tick(0); view.update(angle(-179));
  assert.ok(view.tick(16).roll > 179);
});
test('lost measurement clears the view instead of animating made-up motion', () => {
  const view = createAttitudeView(); view.update(angle(15)); view.tick(0);
  view.update(null); assert.equal(view.tick(50), null);
});
test('reverse inversion remains bounded and takes the short path', () => {
  const view = createAttitudeView();
  view.update(angle(-179)); view.tick(0); view.update(angle(179));
  for (let now = 16; now <= 320; now += 16) {
    const roll = view.tick(now).roll;
    assert.ok(roll >= -180 && roll <= 180);
    assert.ok(Math.abs(roll) > 178);
  }
});
test('presentation responds promptly while keeping raw input unchanged', () => {
  const view = createAttitudeView(), target = angle(40);
  view.update(angle(0)); view.tick(0); view.update(target);
  assert.ok(view.tick(100).roll > 28);
  assert.equal(target.roll, 40);
});

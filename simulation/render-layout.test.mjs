import test from 'node:test';
import assert from 'node:assert/strict';
import { arenaLayout } from './render-layout.mjs';

const world = { width: 3.2, height: 2.4 };

test('hidden and not-yet-laid-out iframe canvases defer drawing', () => {
  for (const [width, height] of [[0, 390], [500, 0], [56, 390], [500, 65], [NaN, 390]]) {
    assert.equal(arenaLayout(width, height, world), null);
  }
});

test('a later visible layout has positive radii and fits the drawing area', () => {
  for (const [width, height] of [[57, 66], [350, 330], [600, 405]]) {
    const layout = arenaLayout(width, height, world);
    assert.ok(layout.scale > 0);
    assert.ok(layout.offsetX >= 28);
    assert.ok(layout.offsetX + world.width * layout.scale <= width - 28 + 1e-9);
    assert.ok(world.height * layout.scale <= height - 65 + 1e-9);
  }
});

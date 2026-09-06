// SPDX-License-Identifier: Apache-2.0

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";

import {
  BUTTON,
  HardwareInputSession,
  InputMux,
  RelativeDelayEstimator,
  SequenceGapTracker,
  decodeInputEvent,
  mapJoystick,
  readNesRomFile,
  validateNesRom,
  webSocketUrl,
} from "../controller.js";

const goodEvent = Object.freeze({
  v: 1,
  seq: 42,
  sample_seq: 84,
  sample_ms: 1234,
  sent_ms: 1237,
  x: 0,
  y: 0,
  sw: false,
  ok: true,
});

function syntheticNrom() {
  const bytes = new Uint8Array(16 + 32 * 1024 + 8 * 1024);
  bytes.set([0x4e, 0x45, 0x53, 0x1a, 2, 1, 0, 0], 0);
  bytes.fill(0xea, 16, 16 + 32 * 1024);
  bytes.set([0x4c, 0x00, 0x80], 16);
  const vectors = 16 + 32 * 1024 - 6;
  bytes.set([0x00, 0x80, 0x00, 0x80, 0x00, 0x80], vectors);
  return bytes;
}

test("protocol accepts the exact v1 frame and rejects unsafe values", () => {
  assert.deepEqual(decodeInputEvent(JSON.stringify(goodEvent)), goodEvent);
  assert.throws(() => decodeInputEvent({ ...goodEvent, v: 2 }), /version/);
  assert.throws(() => decodeInputEvent({ ...goodEvent, sample_seq: -1 }), /sample_seq/);
  assert.throws(() => decodeInputEvent({ ...goodEvent, x: 1001 }), /x and y/);
  assert.throws(() => decodeInputEvent({ ...goodEvent, sent_ms: 1200 }), /precede/);
  assert.throws(() => decodeInputEvent({ ...goodEvent, sw: 1 }), /booleans/);
});

test("joystick mapping uses hysteresis and never presses opposite directions", () => {
  const left = mapJoystick({ ...goodEvent, x: -500 }, 0, { invertY: false });
  assert.equal(left, 1 << BUTTON.LEFT);
  assert.equal(
    mapJoystick({ ...goodEvent, x: -300 }, left, { invertY: false }),
    1 << BUTTON.LEFT,
  );
  assert.equal(mapJoystick({ ...goodEvent, x: -200 }, left, { invertY: false }), 0);

  const right = mapJoystick({ ...goodEvent, x: 800 }, left, { invertY: false });
  assert.equal(right, 1 << BUTTON.RIGHT);
  assert.equal(right & (1 << BUTTON.LEFT), 0);

  const diagonal = mapJoystick(
    { ...goodEvent, x: 700, y: -700, sw: true },
    0,
    { invertY: false },
  );
  assert.equal(
    diagonal,
    (1 << BUTTON.RIGHT) | (1 << BUTTON.UP) | (1 << BUTTON.A),
  );
});

test("Y inversion changes only the vertical direction", () => {
  const normal = mapJoystick({ ...goodEvent, y: -800 }, 0, { invertY: false });
  const inverted = mapJoystick({ ...goodEvent, y: -800 }, 0, { invertY: true });
  assert.equal(normal, 1 << BUTTON.UP);
  assert.equal(inverted, 1 << BUTTON.DOWN);
});

test("input mux combines sources and emits only real button edges", () => {
  const edges = [];
  const mux = new InputMux((next, previous) => edges.push([next, previous]));
  mux.set("hardware", 1 << BUTTON.A);
  mux.set("screen", 1 << BUTTON.B);
  mux.set("screen", 1 << BUTTON.B);
  mux.release("hardware");
  mux.release("screen");
  assert.deepEqual(edges, [
    [1 << BUTTON.A, 0],
    [(1 << BUTTON.A) | (1 << BUTTON.B), 1 << BUTTON.A],
    [1 << BUTTON.B, (1 << BUTTON.A) | (1 << BUTTON.B)],
    [0, 1 << BUTTON.B],
  ]);
});

test("hardware input releases on stale timeout, invalid input and disconnect", () => {
  let pending;
  const cleared = [];
  const masks = [];
  const mux = new InputMux();
  const session = new HardwareInputSession(mux, {
    staleMs: 250,
    setTimer(callback, delay) {
      pending = { callback, delay };
      return pending;
    },
    clearTimer(timer) {
      cleared.push(timer);
    },
    onMaskChange(mask) {
      masks.push(mask);
    },
  });

  session.accept({ ...goodEvent, x: 900 });
  assert.equal(mux.mask, 1 << BUTTON.RIGHT);
  assert.equal(pending.delay, 250);
  pending.callback();
  assert.equal(mux.mask, 0);
  assert.equal(session.remap({ ...goodEvent, x: -900 }, { invertY: false }), false);
  assert.equal(mux.mask, 0);

  session.accept({ ...goodEvent, sw: true });
  session.accept({ ...goodEvent, ok: false });
  assert.equal(mux.mask, 0);

  session.accept({ ...goodEvent, sw: true });
  session.disconnect();
  assert.equal(mux.mask, 0);
  assert.equal(session.remap({ ...goodEvent, sw: true }, { invertY: false }), false);
  assert.equal(mux.mask, 0);
  assert.ok(cleared.length >= 2);
  assert.equal(masks.at(-1), 0);
});

test("sequence gaps survive reconnects and handle uint32 wrap", () => {
  const tracker = new SequenceGapTracker();
  assert.equal(tracker.observe(10), 0);
  assert.equal(tracker.observe(12), 1);

  // A reconnect does not reset this tracker; the next received attempt exposes
  // a failed send as a gap.
  assert.equal(tracker.observe(15), 3);

  const wrapped = new SequenceGapTracker();
  assert.equal(wrapped.observe(0xfffffffe), 0);
  assert.equal(wrapped.observe(0xffffffff), 0);
  assert.equal(wrapped.observe(0), 0);
  assert.equal(wrapped.observe(2), 1);

  // A firmware restart moves backwards and starts a new baseline, not a huge gap.
  assert.equal(tracker.observe(1), 3);
  assert.equal(tracker.observe(2), 3);
});

test("ROM validator accepts an in-memory 40,976-byte NROM only", () => {
  const rom = syntheticNrom();
  assert.equal(rom.byteLength, 40976);
  assert.equal(validateNesRom(rom).declaredBytes, 40976);
  assert.throws(() => validateNesRom(rom.subarray(0, -1)), /truncated/);
  assert.throws(() => validateNesRom(new Uint8Array(16)), /iNES header|PRG/);
  assert.throws(() => validateNesRom(rom, 40975), /exceeds/);
});

test("ROM file size is rejected before browser memory reads", async () => {
  let reads = 0;
  const tooLarge = {
    size: 1024 * 1024 + 1,
    async arrayBuffer() {
      reads += 1;
      return new ArrayBuffer(0);
    },
  };
  await assert.rejects(readNesRomFile(tooLarge), /exceeds/);
  assert.equal(reads, 0);

  const rom = syntheticNrom();
  const accepted = await readNesRomFile({
    size: rom.byteLength,
    async arrayBuffer() {
      reads += 1;
      return rom.buffer;
    },
  });
  assert.equal(accepted.declaredBytes, 40976);
  assert.equal(reads, 1);
});

test("vendored JSNES has the pinned checksum and runs one synthetic frame", async () => {
  const vendorUrl = new URL("../vendor/jsnes.min.js", import.meta.url);
  const vendorBytes = await readFile(vendorUrl);
  assert.equal(
    createHash("sha256").update(vendorBytes).digest("hex"),
    "84946f0dae4bcf3b2dfebe84ad3bfeb1dbcb413bd9b1a69e1f45910a95fd7b41",
  );

  await import(vendorUrl.href);
  assert.equal(globalThis.jsnes.Controller.BUTTON_A, BUTTON.A);
  assert.equal(globalThis.jsnes.Controller.BUTTON_RIGHT, BUTTON.RIGHT);
  let frames = 0;
  const emulator = new globalThis.jsnes.NES({
    emulateSound: false,
    onFrame() {
      frames += 1;
    },
  });
  emulator.loadROM(syntheticNrom());
  emulator.frame();
  assert.equal(frames, 1);
});

test("same-origin WebSocket URL and relative-delay estimator are deterministic", () => {
  assert.equal(webSocketUrl({ protocol: "http:", host: "192.0.2.1:8080" }), "ws://192.0.2.1:8080/ws/input");
  assert.equal(webSocketUrl({ protocol: "https:", host: "home.test" }), "wss://home.test/ws/input");

  const estimator = new RelativeDelayEstimator();
  assert.equal(estimator.observe(1050, 1000), 0);
  assert.equal(estimator.observe(1075, 1010), 15);
  assert.equal(estimator.observe(1040, 1000), 0);
});

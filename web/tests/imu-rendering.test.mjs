import test from "node:test";
import assert from "node:assert/strict";
import { boardBasis, boardPose, chartSamples, relativeTilt, unwrapDegrees } from "../app/imu/rendering.ts";

function readings(count) {
  return Array.from({ length: count }, (_, index) => ({
    t: index * 10,
    accel: [Math.sin(index), Math.cos(index), Math.sin(index / 3)],
    corrected: [Math.cos(index / 4), Math.sin(index / 5), Math.cos(index / 6)],
  }));
}

test("low and clear chart budgets preserve raw samples and chronological endpoints", () => {
  const raw = readings(1500);
  const original = structuredClone(raw);
  for (const budget of [120, 600]) {
    const result = chartSamples(raw, "accel", budget);
    assert.ok(result.length <= budget);
    assert.equal(result[0], raw[0]);
    assert.equal(result.at(-1), raw.at(-1));
    assert.ok(result.every((sample, index) => raw.includes(sample) && (!index || sample.t > result[index - 1].t)));
  }
  assert.deepEqual(raw, original);
});

test("short histories are retained without allocating replacement samples", () => {
  const raw = readings(100);
  assert.equal(chartSamples(raw, "accel", 120), raw);
  assert.deepEqual(chartSamples([], "corrected", 120), []);
});

test("downsampling retains sharp positive and negative extrema on all axes", () => {
  const raw = Array.from({ length: 1000 }, (_, t) => ({ t, accel: [0, 0, 0], corrected: [0, 0, 0] }));
  const peaks = [12, 29, 135, 256, 592, 993];
  for (let i = 0; i < peaks.length; i++) raw[peaks[i]].corrected[Math.floor(i / 2)] = i % 2 ? -100 : 100;
  const result = chartSamples(raw, "corrected", 120);
  for (const peak of peaks) assert.ok(result.includes(raw[peak]), `missing peak at ${peak}`);
});

test("roll crosses both sides of ±180 degrees by the short rotation", () => {
  assert.equal(unwrapDegrees(179, -179), 181);
  assert.equal(unwrapDegrees(-179, 179), -181);
  assert.equal(unwrapDegrees(181, -178), 182);
  assert.equal(unwrapDegrees(-181, 178), -182);
});

test("continuous roll remains stable across complete rotations", () => {
  assert.equal(unwrapDegrees(539, -179), 541);
  assert.equal(unwrapDegrees(-539, 179), -541);
  assert.equal(unwrapDegrees(720, 0), 720);
});

function close(actual, expected, tolerance = 1e-9) {
  assert.ok(Math.abs(actual - expected) < tolerance, `${actual} differs from ${expected}`);
}

test("saved gravity maps to +Z and zero relative tilt for arbitrary and antipodal poses", () => {
  for (const reference of [[.31, -.91, .12], [0, 1, 0], [1, 0, 0], [0, 0, 1], [0, 0, -1]]) {
    const result = relativeTilt(reference, reference);
    assert.ok(result);
    close(result.roll, 0);
    close(result.pitch, 0);
    for (let axis = 0; axis < 3; axis++) close(result.gravity[axis], axis === 2 ? 1 : 0);
  }
});

test("a vertical +Y reference preserves a known 30 degree roll and 20 degree pitch", () => {
  const radians = Math.PI / 180;
  // The inverse +Y -> +Z reference transform is a -90° X rotation.
  const roll = relativeTilt([0, Math.cos(30 * radians), -Math.sin(30 * radians)], [0, 1, 0]);
  close(roll.roll, 30);
  close(roll.pitch, 0);
  const pitch = relativeTilt([-Math.sin(20 * radians), Math.cos(20 * radians), 0], [0, 1, 0]);
  close(pitch.roll, 0);
  close(pitch.pitch, 20);
});

test("a +X reference correctly resolves combined rotations instead of subtracting Euler angles", () => {
  const roll = 25 * Math.PI / 180;
  const pitch = -40 * Math.PI / 180;
  // Inverse -90° Y alignment puts canonical gravity into this +X sensor pose.
  const gravity = [Math.cos(roll) * Math.cos(pitch), Math.sin(roll) * Math.cos(pitch), Math.sin(pitch)];
  const result = relativeTilt(gravity, [1, 0, 0]);
  close(result.roll, 25);
  close(result.pitch, -40);
});

test("relative tilt preserves raw vectors, ignores magnitude, and rejects missing gravity", () => {
  const current = [0.2, 0.3, 0.9];
  const reference = [0.1, 0.95, 0.05];
  const originals = structuredClone([current, reference]);
  const first = relativeTilt(current, reference);
  const scaled = relativeTilt(current.map((value) => value * 1.1), reference.map((value) => value * 0.9));
  close(first.roll, scaled.roll);
  close(first.pitch, scaled.pitch);
  assert.deepEqual([current, reference], originals);
  for (const invalid of [[0, 0, 0], [NaN, 0, 1], [0, Infinity, 1], [0, 1]]) {
    assert.equal(relativeTilt(invalid, reference), null);
    assert.equal(relativeTilt(current, invalid), null);
  }
});

const FLAT_CAPTURE = [.10801, .05232, 1.041732];
const USB_CAPTURE = [-.7229258, -.0906288, .7499919];

function transform(matrix, vector) {
  return [0, 1, 2].map((row) => vector[0] * matrix[row] + vector[1] * matrix[row + 4] + vector[2] * matrix[row + 8]);
}

function properRotation(matrix, expectedDeterminant = 1) {
  for (let a = 0; a < 3; a++) for (let b = 0; b < 3; b++) {
    close([0, 1, 2].reduce((sum, row) => sum + matrix[a * 4 + row] * matrix[b * 4 + row], 0), a === b ? 1 : 0);
  }
  const determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6])
    - matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2])
    + matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
  close(determinant, expectedDeterminant);
}

test("actual flat capture is zero board tilt and old USB pose keeps its measured angle", () => {
  const flat = boardPose(FLAT_CAPTURE, FLAT_CAPTURE, USB_CAPTURE);
  close(flat.roll, 0);
  close(flat.pitch, 0);
  close(flat.tilt, 0, 1e-5);
  properRotation(flat.matrix);
  const old = boardPose(USB_CAPTURE, FLAT_CAPTURE, USB_CAPTURE);
  const measuredAngle = Math.acos(USB_CAPTURE.reduce((sum, value, i) => sum + value * FLAT_CAPTURE[i], 0)
    / (Math.hypot(...USB_CAPTURE) * Math.hypot(...FLAT_CAPTURE))) * 180 / Math.PI;
  close(old.tilt, measuredAngle);
  assert.ok(old.tilt > 45 && old.tilt < 55, "the old capture must not be forced upright at 90°");
  close(old.pitch, 0);
});

test("physical USB-up, right-edge-up and inverted board rotations align gravity to world up", () => {
  const flat = [0, 0, 1];
  const usb = [0, .8, .6];
  for (const [gravity, tilt, roll, pitch] of [
    [[0, 1, 0], 90, 90, 0],
    [[-1, 0, 0], 90, 0, -90],
    [[0, 0, -1], 180, 180, 0],
  ]) {
    const pose = boardPose(gravity, flat, usb);
    close(pose.tilt, tilt);
    close(pose.roll, roll);
    close(pose.pitch, pitch);
    properRotation(pose.matrix);
    transform(pose.matrix, pose.gravity).forEach((value, index) => close(value, index === 2 ? 1 : 0));
  }
});

test("physical right and USB edges rise on the matching CSS model edges", () => {
  // Sensor axes are right-handed. Viewed from the component side with USB down,
  // the independent physical right direction is +sensor X and USB is -sensor Y.
  const flat = [0, 0, 1];
  const usb = [0, -1, 0];
  const rightUp = boardPose([1, 0, 0], flat, usb);
  rightUp.gravity.forEach((value, index) => close(value, index === 0 ? 1 : 0));
  close(rightUp.pitch, -90);
  transform(rightUp.matrix, [1, 0, 0]).forEach((value, index) => close(value, index === 2 ? 1 : 0));
  const usbUp = boardPose([0, -1, 0], flat, usb);
  usbUp.gravity.forEach((value, index) => close(value, index === 1 ? 1 : 0));
  close(usbUp.roll, 90);
  transform(usbUp.matrix, [0, 1, 0]).forEach((value, index) => close(value, index === 2 ? 1 : 0));
  properRotation(rightUp.matrix);
  properRotation(usbUp.matrix);
});

test("compound board tilt uses a proper column-major rotation, independently of camera", () => {
  for (const gravity of [[.4, -.3, Math.sqrt(.75)], [-.5, .8, -.3], [.1, .01, -.994]]) {
    const pose = boardPose(gravity, FLAT_CAPTURE, USB_CAPTURE);
    properRotation(pose.matrix);
    transform(pose.matrix, pose.gravity).forEach((value, index) => close(value, index === 2 ? 1 : 0));
  }
});

test("mount basis reflects sensor axes into CSS, preserves inputs and rejects undefined in-plane direction", () => {
  const original = structuredClone([FLAT_CAPTURE, USB_CAPTURE]);
  const basis = boardBasis(FLAT_CAPTURE, USB_CAPTURE);
  properRotation([...basis.right, 0, ...basis.usb, 0, ...basis.normal, 0, 0, 0, 0, 1], -1);
  assert.deepEqual([FLAT_CAPTURE, USB_CAPTURE], original);
  for (const flat of [[0, 0, 0], [NaN, 0, 1], [0, 1]]) assert.equal(boardPose([0, 0, 1], flat, USB_CAPTURE), null);
  assert.equal(boardBasis(FLAT_CAPTURE, FLAT_CAPTURE), null);
  assert.equal(boardBasis(FLAT_CAPTURE, FLAT_CAPTURE.map((value) => -value)), null);
  assert.equal(boardPose([0, 0, 0], FLAT_CAPTURE, USB_CAPTURE), null);
});

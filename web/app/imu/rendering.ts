/** Choose only existing measurements, retaining each axis's extrema per bucket. */
export function chartSamples<T extends { accel: readonly number[]; corrected: readonly number[] }>(samples: readonly T[], property: "accel" | "corrected", limit: number): readonly T[] {
  if (samples.length <= limit) return samples;
  const buckets = Math.max(1, Math.floor((limit - 2) / 6));
  const chosen = new Set([0, samples.length - 1]);
  const interior = samples.length - 2;
  for (let bucket = 0; bucket < buckets; bucket++) {
    const start = 1 + Math.floor(bucket * interior / buckets);
    const end = 1 + Math.floor((bucket + 1) * interior / buckets);
    for (let axis = 0; axis < 3; axis++) {
      let min = start;
      let max = start;
      for (let index = start + 1; index < end; index++) {
        if (samples[index][property][axis] < samples[min][property][axis]) min = index;
        if (samples[index][property][axis] > samples[max][property][axis]) max = index;
      }
      chosen.add(min);
      chosen.add(max);
    }
  }
  return [...chosen].sort((a, b) => a - b).map((index) => samples[index]);
}

/** Keep successive gravity-based rolls continuous across the ±180° boundary. */
export function unwrapDegrees(previous: number, current: number): number {
  return previous + ((current - previous + 180) % 360 + 360) % 360 - 180;
}

type Vector3 = [number, number, number];

function normalized(vector: readonly number[]): Vector3 | null {
  if (vector.length !== 3 || !vector.every(Number.isFinite)) return null;
  const magnitude = Math.hypot(...vector);
  if (!Number.isFinite(magnitude) || magnitude < .02) return null;
  return [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude];
}

function dot(a: readonly number[], b: readonly number[]): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function cross(a: readonly number[], b: readonly number[]): Vector3 {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

/** CSS +X is right, +Y points down toward USB, +Z is the component-face normal. */
export function boardBasis(flatGravity: readonly number[], usbGravity: readonly number[]): { right: Vector3; usb: Vector3; normal: Vector3 } | null {
  const normal = normalized(flatGravity);
  const usbPose = normalized(usbGravity);
  if (!normal || !usbPose) return null;
  const projection = dot(usbPose, normal);
  const tangent = usbPose.map((value, axis) => value - projection * normal[axis]);
  // Nearly parallel captures cannot reliably determine the in-plane USB axis.
  if (Math.hypot(...tangent) < .1) return null;
  const usb = normalized(tangent);
  if (!usb) return null;
  // CSS's right/down/front axes reflect the sensor's right-handed coordinates.
  return { right: cross(normal, usb), usb, normal };
}

/** Physical board tilt, independent of the camera. matrix is CSS column-major. */
export function boardPose(current: readonly number[], flatGravity: readonly number[], usbGravity: readonly number[]): { roll: number; pitch: number; tilt: number; gravity: Vector3; matrix: number[] } | null {
  const measured = normalized(current);
  const basis = boardBasis(flatGravity, usbGravity);
  if (!measured || !basis) return null;
  const gravity = normalized([dot(measured, basis.right), dot(measured, basis.usb), dot(measured, basis.normal)]);
  if (!gravity) return null;
  // Shortest rotation g_board -> world +Z. Heading is not measured.
  const quaternion = gravity[2] < -1 + 1e-12 ? [1, 0, 0, 0] : [gravity[1], -gravity[0], 0, 1 + gravity[2]];
  const norm = Math.hypot(...quaternion);
  const [x, y, z, w] = quaternion.map((value) => value / norm);
  return {
    roll: Math.atan2(gravity[1], gravity[2]) * 180 / Math.PI,
    pitch: Math.atan2(-gravity[0], Math.hypot(gravity[1], gravity[2])) * 180 / Math.PI,
    tilt: Math.acos(Math.max(-1, Math.min(1, gravity[2]))) * 180 / Math.PI,
    gravity,
    matrix: [
      1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
      2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
      2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
      0, 0, 0, 1,
    ],
  };
}

/**
 * Express gravity in the saved pose's frame, then estimate relative roll/pitch.
 * The shortest rotation maps reference gravity to +Z; heading is unobservable.
 * For an exactly antipodal reference, choose the deterministic 180° X rotation.
 * Measurements are never modified or replaced with the saved gravity vector.
 */
export function relativeTilt(current: readonly number[], reference: readonly number[]): { roll: number; pitch: number; gravity: Vector3 } | null {
  const gravity = normalized(current);
  const baseline = normalized(reference);
  if (!gravity || !baseline) return null;
  // Unit quaternion [x, y, z, w] for the shortest rotation baseline -> +Z.
  const quaternion = baseline[2] < -1 + 1e-12
    ? [1, 0, 0, 0]
    : [baseline[1], -baseline[0], 0, 1 + baseline[2]];
  const norm = Math.hypot(...quaternion);
  const [qx, qy, qz, qw] = quaternion.map((value) => value / norm);
  // q * gravity * inverse(q), expanded to avoid allocating rotation matrices.
  const tx = 2 * (qy * gravity[2] - qz * gravity[1]);
  const ty = 2 * (qz * gravity[0] - qx * gravity[2]);
  const tz = 2 * (qx * gravity[1] - qy * gravity[0]);
  const aligned: Vector3 = [
    gravity[0] + qw * tx + qy * tz - qz * ty,
    gravity[1] + qw * ty + qz * tx - qx * tz,
    gravity[2] + qw * tz + qx * ty - qy * tx,
  ];
  return {
    roll: Math.atan2(aligned[1], aligned[2]) * 180 / Math.PI,
    pitch: Math.atan2(-aligned[0], Math.hypot(aligned[1], aligned[2])) * 180 / Math.PI,
    gravity: aligned,
  };
}

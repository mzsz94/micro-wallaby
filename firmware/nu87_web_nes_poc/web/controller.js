// SPDX-License-Identifier: Apache-2.0

export const BUTTON = Object.freeze({
  A: 0,
  B: 1,
  SELECT: 2,
  START: 3,
  UP: 4,
  DOWN: 5,
  LEFT: 6,
  RIGHT: 7,
});

export const ALL_BUTTONS_MASK = 0xff;
export const HARDWARE_BUTTONS_MASK =
  (1 << BUTTON.A) |
  (1 << BUTTON.UP) |
  (1 << BUTTON.DOWN) |
  (1 << BUTTON.LEFT) |
  (1 << BUTTON.RIGHT);

function isIntInRange(value, minimum, maximum) {
  return Number.isInteger(value) && value >= minimum && value <= maximum;
}

export function decodeInputEvent(payload) {
  const value = typeof payload === "string" ? JSON.parse(payload) : payload;
  if (value === null || typeof value !== "object") {
    throw new TypeError("input event must be an object");
  }
  if (value.v !== 1) {
    throw new RangeError("unsupported input protocol version");
  }
  if (!isIntInRange(value.seq, 0, 0xffffffff)) {
    throw new RangeError("seq must be an unsigned 32-bit integer");
  }
  if (!isIntInRange(value.sample_seq, 0, 0xffffffff)) {
    throw new RangeError("sample_seq must be an unsigned 32-bit integer");
  }
  if (!Number.isSafeInteger(value.sample_ms) || value.sample_ms < 0) {
    throw new RangeError("sample_ms must be a non-negative safe integer");
  }
  if (!Number.isSafeInteger(value.sent_ms) || value.sent_ms < value.sample_ms) {
    throw new RangeError("sent_ms must not precede sample_ms");
  }
  if (!isIntInRange(value.x, -1000, 1000) || !isIntInRange(value.y, -1000, 1000)) {
    throw new RangeError("x and y must be integers in -1000..1000");
  }
  if (typeof value.sw !== "boolean" || typeof value.ok !== "boolean") {
    throw new TypeError("sw and ok must be booleans");
  }

  return Object.freeze({
    v: value.v,
    seq: value.seq,
    sample_seq: value.sample_seq,
    sample_ms: value.sample_ms,
    sent_ms: value.sent_ms,
    x: value.x,
    y: value.y,
    sw: value.sw,
    ok: value.ok,
  });
}

function axisButtons(value, negativeButton, positiveButton, previousMask, engage, release) {
  const negativeWasPressed = (previousMask & (1 << negativeButton)) !== 0;
  const positiveWasPressed = (previousMask & (1 << positiveButton)) !== 0;

  if (negativeWasPressed && value <= -release) {
    return 1 << negativeButton;
  }
  if (positiveWasPressed && value >= release) {
    return 1 << positiveButton;
  }
  if (value <= -engage) {
    return 1 << negativeButton;
  }
  if (value >= engage) {
    return 1 << positiveButton;
  }
  return 0;
}

export function mapJoystick(
  sample,
  previousMask = 0,
  { engage = 450, release = 250, invertX = false, invertY = true } = {},
) {
  if (!sample || sample.ok !== true) {
    return 0;
  }
  if (!(engage > release && release >= 0 && engage <= 1000)) {
    throw new RangeError("thresholds must satisfy 0 <= release < engage <= 1000");
  }

  const x = invertX ? -sample.x : sample.x;
  const y = invertY ? -sample.y : sample.y;
  let mask = sample.sw ? 1 << BUTTON.A : 0;
  mask |= axisButtons(x, BUTTON.LEFT, BUTTON.RIGHT, previousMask, engage, release);
  mask |= axisButtons(y, BUTTON.UP, BUTTON.DOWN, previousMask, engage, release);
  return mask & HARDWARE_BUTTONS_MASK;
}

export class InputMux {
  constructor(onChange = () => {}) {
    this.onChange = onChange;
    this.sources = new Map();
    this.mask = 0;
  }

  set(source, mask) {
    const nextSourceMask = Number(mask) & ALL_BUTTONS_MASK;
    this.sources.set(source, nextSourceMask);
    let nextMask = 0;
    for (const sourceMask of this.sources.values()) {
      nextMask |= sourceMask;
    }
    nextMask &= ALL_BUTTONS_MASK;
    if (nextMask === this.mask) {
      return false;
    }
    const previous = this.mask;
    this.mask = nextMask;
    this.onChange(nextMask, previous);
    return true;
  }

  release(source) {
    return this.set(source, 0);
  }
}

export class HardwareInputSession {
  constructor(
    mux,
    {
      staleMs = 250,
      setTimer = (callback, delay) => setTimeout(callback, delay),
      clearTimer = (timer) => clearTimeout(timer),
      onMaskChange = () => {},
    } = {},
  ) {
    this.mux = mux;
    this.staleMs = staleMs;
    this.setTimer = setTimer;
    this.clearTimer = clearTimer;
    this.onMaskChange = onMaskChange;
    this.timer = null;
    this.hardwareMask = 0;
  }

  accept(sample, options) {
    this.hardwareMask = mapJoystick(sample, this.hardwareMask, options);
    this.mux.set("hardware", this.hardwareMask);
    this.onMaskChange(this.hardwareMask);
    this.#arm();
    return this.hardwareMask;
  }

  remap(sample, options) {
    if (this.timer === null) {
      return false;
    }
    this.hardwareMask = mapJoystick(sample, this.hardwareMask, options);
    this.mux.set("hardware", this.hardwareMask);
    this.onMaskChange(this.hardwareMask);
    return true;
  }

  disconnect() {
    if (this.timer !== null) {
      this.clearTimer(this.timer);
      this.timer = null;
    }
    this.hardwareMask = 0;
    this.mux.release("hardware");
    this.onMaskChange(this.hardwareMask);
  }

  #arm() {
    if (this.timer !== null) {
      this.clearTimer(this.timer);
    }
    this.timer = this.setTimer(() => {
      this.timer = null;
      this.hardwareMask = 0;
      this.mux.release("hardware");
      this.onMaskChange(this.hardwareMask);
    }, this.staleMs);
  }
}

export class SequenceGapTracker {
  constructor() {
    this.previous = null;
    this.gaps = 0;
  }

  observe(sequence) {
    if (!isIntInRange(sequence, 0, 0xffffffff)) {
      throw new RangeError("sequence must be an unsigned 32-bit integer");
    }
    if (this.previous !== null) {
      const expected = (this.previous + 1) >>> 0;
      const delta = (sequence - expected) >>> 0;
      if (delta < 0x80000000) {
        this.gaps += delta;
      }
    }
    this.previous = sequence;
    return this.gaps;
  }
}

export class RelativeDelayEstimator {
  constructor() {
    this.minimumOffset = Number.POSITIVE_INFINITY;
  }

  observe(arrivalMs, sentMs) {
    if (!Number.isFinite(arrivalMs) || !Number.isFinite(sentMs)) {
      throw new TypeError("timestamps must be finite");
    }
    const offset = arrivalMs - sentMs;
    this.minimumOffset = Math.min(this.minimumOffset, offset);
    return Math.max(0, offset - this.minimumOffset);
  }

  reset() {
    this.minimumOffset = Number.POSITIVE_INFINITY;
  }
}

export function validateNesRom(input, maximumBytes = 1024 * 1024) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  if (bytes.byteLength > maximumBytes) {
    throw new RangeError(`ROM exceeds the ${maximumBytes}-byte browser limit`);
  }
  if (bytes.byteLength < 16) {
    throw new RangeError("ROM is shorter than the 16-byte iNES header");
  }
  if (bytes[0] !== 0x4e || bytes[1] !== 0x45 || bytes[2] !== 0x53 || bytes[3] !== 0x1a) {
    throw new TypeError("file does not have an iNES header");
  }
  if ((bytes[7] & 0x0c) === 0x08) {
    throw new TypeError("NES 2.0 ROMs are outside this PoC's validated subset");
  }

  const trainerBytes = bytes[6] & 0x04 ? 512 : 0;
  const prgBytes = bytes[4] * 16 * 1024;
  const chrBytes = bytes[5] * 8 * 1024;
  const declaredBytes = 16 + trainerBytes + prgBytes + chrBytes;
  if (prgBytes === 0) {
    throw new RangeError("iNES ROM declares no PRG data");
  }
  if (bytes.byteLength < declaredBytes) {
    throw new RangeError(
      `ROM is truncated: header declares ${declaredBytes} bytes, got ${bytes.byteLength}`,
    );
  }

  return Object.freeze({ bytes, declaredBytes, prgBytes, chrBytes, trainerBytes });
}

export async function readNesRomFile(file, maximumBytes = 1024 * 1024) {
  if (!file || !Number.isSafeInteger(file.size) || file.size < 0 ||
      typeof file.arrayBuffer !== "function") {
    throw new TypeError("ROM selection is not a readable File");
  }
  if (file.size > maximumBytes) {
    throw new RangeError(`ROM exceeds the ${maximumBytes}-byte browser limit`);
  }
  return validateNesRom(await file.arrayBuffer(), maximumBytes);
}

export function webSocketUrl(locationLike) {
  const scheme = locationLike.protocol === "https:" ? "wss:" : "ws:";
  return `${scheme}//${locationLike.host}/ws/input`;
}

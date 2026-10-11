// Presentation interpolation only. Never use this state for warning decisions.
export function createAttitudeView({ timeConstantMs = 80 } = {}) {
  let target = null, current = null, previous = null;
  function update(orientation) {
    if (!orientation?.gravity_valid || !['roll', 'pitch', 'tilt'].every(key => Number.isFinite(orientation[key]))) {
      target = current = null; return;
    }
    target = { ...orientation };
    if (!current) current = { ...target };
  }
  function tick(now) {
    const dt = previous === null ? 0 : Math.min(100, Math.max(0, now - previous));
    previous = now;
    if (!target || !current) return null;
    const alpha = 1 - Math.exp(-dt / timeConstantMs);
    for (const key of ['roll', 'pitch', 'tilt']) {
      let delta = target[key] - current[key];
      if (key === 'roll') delta = Math.atan2(Math.sin(delta * Math.PI / 180), Math.cos(delta * Math.PI / 180)) * 180 / Math.PI;
      current[key] += alpha * delta;
      if (key === 'roll') current[key] = ((current[key] + 180) % 360 + 360) % 360 - 180;
    }
    return { ...current };
  }
  return { update, tick };
}

import { createAttitudeView } from './attitude-view.mjs';
// Actual sensor attitude only; no position estimation or motor commands.
export function createLiveImuControl(sim, { now = () => performance.now() } = {}) {
  let received = null, blocked = false, sequence = null;
  let reference = [0, 0, 1];
  const view = createAttitudeView();
  const clamp = (v, limit) => Math.max(-limit, Math.min(limit, v));
  function update(state) {
    if (!state || state.source !== 'nu87-i2c' || state.enabled !== true) return false;
    received = now();
    blocked = state.motion_blocked === true;
    if (blocked) sim.command('stop');
    const o = state.display_orientation || state.orientation;
    if (!state.fresh || !o?.gravity_valid || !['roll', 'pitch', 'tilt'].every(key => Number.isFinite(o[key]))) { view.update(null); return false; }
    if (state.packet?.sample === sequence) return true;
    sequence = state.packet?.sample;
    reference = state.reference || [0, 0, 1];
    view.update(o);
    return true;
  }
  function tick() {
    if (received !== null && now() - received > 2000) blocked = true;
    if (blocked) { sim.command('stop'); return; }
    const o = view.tick(now());
    if (o) {
      const ref = reference;
      const baseRoll = Math.atan2(ref[1], ref[2]) * 180 / Math.PI;
      const basePitch = Math.atan2(-ref[0], Math.hypot(ref[1], ref[2])) * 180 / Math.PI;
      const delta = (o.roll - baseRoll) * Math.PI / 180;
      sim.setJoint('waistYaw', clamp(Math.atan2(Math.sin(delta), Math.cos(delta)) * 180 / Math.PI, 30));
      sim.setJoint('headPitch', clamp(o.pitch - basePitch, 25));
    }
  }
  return { update, tick, get blocked() { return blocked; } };
}

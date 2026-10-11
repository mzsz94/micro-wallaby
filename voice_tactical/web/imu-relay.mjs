// Forward cached HostPC IMU snapshots. This never requests NU87 directly.
export function createImuRelay(send, { onBlocked = () => {} } = {}) {
  let latest = null;
  function replay() {
    if (!latest) return;
    send({ type: 'wallaby-imu', state: latest });
    if (latest.motion_blocked) send({ type: 'wallaby-voice-command', command: 'stop' });
  }
  function accept(state) {
    if (!state || state.source !== 'nu87-i2c' || typeof state.enabled !== 'boolean') return false;
    const wasBlocked = latest?.motion_blocked === true;
    const disabled = latest?.enabled === true && state.enabled === false;
    latest = state.enabled ? { ...state, motion_blocked: state.motion_blocked === true || state.fresh !== true }
      : { ...state, motion_blocked: false, fresh: false, orientation: null, display_orientation: null };
    replay();
    if (disabled) send({ type: 'wallaby-voice-command', command: 'stop' });
    if (latest.motion_blocked && !wasBlocked) onBlocked();
    return true;
  }
  function disconnected() {
    if (!latest?.enabled) return;
    const wasBlocked = latest.motion_blocked;
    latest = { ...latest, fresh: false, motion_blocked: true,
      orientation: null, display_orientation: null };
    replay();
    if (!wasBlocked) onBlocked();
  }
  return { accept, disconnected, replay, get blocked() { return latest?.motion_blocked === true; } };
}

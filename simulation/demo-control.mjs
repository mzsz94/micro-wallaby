/** Presentation choreography uses the real model and the same motion safety gates. */
export function createDemoController(sim, voice, {
  now = () => performance.now(), schedule = setTimeout, cancel = clearTimeout,
  canRun = () => true, resetModel = () => sim.reset(), timeoutMs = 750,
} = {}) {
  const expressions = new Set(['idle', 'listening', 'thinking', 'happy', 'uncertain', 'alert']);
  let expression = 'idle';
  let label = 'READY';
  let accent = '#9be7c3';
  let marker = null;
  let driving = false;
  let deadline = 0;
  let timer = null;
  let driveAccepted = null;
  const motionLocked = () => voice.alertActive || !canRun();
  function disarm() {
    if (timer !== null) cancel(timer);
    timer = null;
    driving = false;
    deadline = 0;
  }
  function stop() {
    if (driving) sim.command('stop');
    disarm();
  }
  function tick() {
    if (driving && (motionLocked() || now() >= deadline || sim.snapshot().faults.networkLoss)) stop();
  }
  function snapshot() {
    const s = sim.snapshot();
    const alertActive = voice.alertActive;
    const unsafe = alertActive || s.state === 'FAULT' || s.state === 'DOCK_FAILED';
    return { ...s, expression: unsafe ? 'alert' : expression, label, accent: unsafe ? '#ff766b' : accent,
      alertActive, motionLocked: motionLocked(), demoDriving: driving, driveAccepted, marker: marker ? { ...marker } : null,
      trailLength: s.trail.length,
      distanceTraveled: s.trail.slice(1).reduce((sum, p, i) => sum + Math.hypot(p.x - s.trail[i].x, p.y - s.trail[i].y), 0),
    };
  }
  function apply(input = {}) {
    if (!input || typeof input !== 'object') return snapshot();
    if (expressions.has(input.expression)) expression = input.expression;
    if (typeof input.label === 'string') label = input.label.slice(0, 44);
    if (typeof input.accent === 'string' && /^#[\da-f]{6}$/i.test(input.accent)) accent = input.accent;
    if (input.marker === null) marker = null;
    else if (input.marker && Number.isFinite(input.marker.x) && Number.isFinite(input.marker.y)) {
      marker = { x: input.marker.x, y: input.marker.y, label: String(input.marker.label || 'MOCK TARGET').slice(0, 32), detected: input.marker.detected === true };
    }
    tick();
    // null/absent drive must never cancel or renew a voice command.
    if (input.drive && typeof input.drive === 'object') {
      const { linear, angular } = input.drive;
      if (Number.isFinite(linear) && Number.isFinite(angular)) {
        if (!linear && !angular) {
          voice.interrupt('demo stop');
          sim.command('stop');
          disarm();
          driveAccepted = true;
        } else if (motionLocked()) {
          stop();
          driveAccepted = false;
        } else {
          voice.interrupt('demo drive');
          driveAccepted = sim.setManual(linear, angular);
          if (driveAccepted) {
            disarm();
            driving = true;
            deadline = now() + timeoutMs;
            timer = schedule(stop, timeoutMs);
          }
        }
      }
    }
    if (!motionLocked() && input.joints && typeof input.joints === 'object') {
      for (const name of ['headYaw', 'headPitch', 'waistYaw']) {
        if (Number.isFinite(input.joints[name])) sim.setJoint(name, input.joints[name]);
      }
    }
    return snapshot();
  }
  function reset() {
    if (voice.alertActive) return snapshot();
    stop();
    voice.interrupt('demo reset');
    resetModel();
    expression = 'idle'; label = 'READY'; accent = '#9be7c3'; driveAccepted = null; marker = null;
    return snapshot();
  }
  return { apply, snapshot, reset, tick, stop };
}

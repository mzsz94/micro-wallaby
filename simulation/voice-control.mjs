// The bridge operates the existing kinematic model only; it has no hardware path.
const DIRECTIONS = Object.freeze({ forward: [.12, 0], back: [-.12, 0], left: [0, 1], right: [0, -1] });

export function createVoiceController(sim, {
  now = () => performance.now(), schedule = setTimeout, cancel = clearTimeout,
  onResult = () => {}, canRun = () => true,
} = {}) {
  let timer = null;
  let movementUntil = 0;
  let heartbeatAt = null;
  let active = false;
  let alert = false;
  let patrol = null;
  const clear = () => { if (timer !== null) cancel(timer); timer = null; movementUntil = 0; patrol = null; };
  function stop(reason = 'stop') {
    clear();
    active = false;
    sim.command('stop');
    onResult({ command: 'stop', accepted: true, reason });
  }
  function command(name) {
    if (name === 'stop') { stop(); return true; }
    if (alert) { onResult({ command: name, accepted: false, reason: 'alert' }); return false; }
    if (!Object.hasOwn(DIRECTIONS, name) && !['dock', 'greet', 'patrol'].includes(name)) return false;
    if (!canRun()) return false;
    let accepted;
    if (Object.hasOwn(DIRECTIONS, name)) {
      accepted = sim.setManual(...DIRECTIONS[name]);
      if (accepted) {
        clear();
        movementUntil = now() + 900;
        timer = schedule(() => stop('duration'), 900);
      }
    } else if (name === 'patrol') {
      accepted = sim.setManual(0, 0);
      if (accepted) {
        clear();
        const { pose, world } = sim.snapshot();
        const x = pose.x + (pose.x + .7 < world.width - .2 ? .7 : -.7);
        const y = pose.y + (pose.y + .45 < world.height - .2 ? .45 : -.45);
        patrol = { route: [{ x, y: pose.y }, { x, y }, { x: pose.x, y }, { x: pose.x, y: pose.y }], index: 0, startedAt: now() };
        movementUntil = now() + 60000;
        timer = schedule(() => stop('duration'), 60000);
      }
    } else {
      accepted = sim.command(name === 'greet' ? 'welcome' : 'dock');
      if (accepted) clear();
    }
    if (accepted) { active = true; heartbeatAt = now(); }
    onResult({ command: name, accepted });
    return accepted;
  }
  function heartbeat() { heartbeatAt = now(); }
  function tick() {
    if (!active) return;
    if (!canRun()) stop('inactive');
    else if (movementUntil && now() >= movementUntil) stop('duration');
    else if (heartbeatAt !== null && now() - heartbeatAt > 2500) stop('parent disconnected');
    else if (sim.snapshot().faults.networkLoss) stop('network fault');
    else if (patrol) {
      const snapshot = sim.snapshot();
      if (snapshot.state !== 'MANUAL') { stop('model state changed'); return; }
      const target = patrol.route[patrol.index];
      const dx = target.x - snapshot.pose.x, dy = target.y - snapshot.pose.y;
      if (Math.hypot(dx, dy) < .055) {
        patrol.index++;
        if (patrol.index === patrol.route.length) stop('patrol complete');
        return;
      }
      const bearing = Math.atan2(dy, dx) - snapshot.pose.theta;
      const error = Math.atan2(Math.sin(bearing), Math.cos(bearing));
      const angular = Math.max(-1.3, Math.min(1.3, error * 3));
      const linear = Math.abs(error) > .35 ? 0 : Math.min(.14, Math.hypot(dx, dy) * 1.8);
      if (!sim.setManual(linear, angular)) { stop('model rejected'); return; }
      const elapsed = now() - patrol.startedAt;
      sim.setJoint('headYaw', 30 * Math.sin(elapsed / 700));
      sim.setJoint('headPitch', -6);
      sim.setJoint('waistYaw', 8 * Math.sin(elapsed / 1400));
    }
  }
  return { command, stop, heartbeat, tick, interrupt(reason) { if (active) stop(reason); },
    setAlert(value) { alert = value === true; if (alert) stop('explosive alert'); },
    get alertActive() { return alert; },
    get patrolState() { return patrol ? { route: patrol.route.map(point => ({ ...point })), index: patrol.index } : null; },
  };
}

export function isParentMessage(event, parentWindow, origin) {
  return parentWindow !== null && event.source === parentWindow && event.origin === origin &&
    event.data !== null && typeof event.data === 'object';
}

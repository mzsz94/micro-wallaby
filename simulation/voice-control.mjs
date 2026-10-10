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
  const clear = () => { if (timer !== null) cancel(timer); timer = null; movementUntil = 0; };
  function stop(reason = 'stop') {
    clear();
    active = false;
    sim.command('stop');
    onResult({ command: 'stop', accepted: true, reason });
  }
  function command(name) {
    if (name === 'stop') { stop(); return true; }
    if (alert) { onResult({ command: name, accepted: false, reason: 'alert' }); return false; }
    if (!Object.hasOwn(DIRECTIONS, name) && !['dock', 'greet'].includes(name)) return false;
    if (!canRun()) return false;
    let accepted;
    if (Object.hasOwn(DIRECTIONS, name)) {
      accepted = sim.setManual(...DIRECTIONS[name]);
      if (accepted) {
        clear();
        movementUntil = now() + 900;
        timer = schedule(() => stop('duration'), 900);
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
  }
  return { command, stop, heartbeat, tick, interrupt(reason) { if (active) stop(reason); },
    setAlert(value) { alert = value === true; if (alert) stop('explosive alert'); },
    get alertActive() { return alert; },
  };
}

export function isParentMessage(event, parentWindow, origin) {
  return parentWindow !== null && event.source === parentWindow && event.origin === origin &&
    event.data !== null && typeof event.data === 'object';
}

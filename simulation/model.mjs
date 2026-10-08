/** Deterministic, ideal 2-D kinematic preview; not a firmware or physics emulator. */
export const CONFIG = {
  dt: 0.02,
  radius: 0.10,
  wheelRadius: 0.03,
  track: 0.15,
  maxSpeed: 0.15,
  maxAcceleration: 0.3,
  maxAngularSpeed: 1.5,
  dockApproachSpeed: 0.08,
  dockAlignSpeed: 0.03,
  dockAlignGap: 0.15,
  dockRange: 2,
  dockWidth: 0.24,
  dockDepth: 0.10,
  verifySeconds: 5,
  searchTimeout: 12,
  attemptTimeout: 60,
  maxAttempts: 2,
  retryDistance: 0.12,
  welcomeSeconds: 3,
  jointSpeed: 60,
  autoDockSoc: 25,
  criticalSoc: 15,
  targetSoc: 95,
  // Illustrative charging only; no discharge, electrical, or thermal model.
  chargeRate: 0.25,
  // Tentative visualization limits, awaiting mechanical design.
  jointLimits: { headYaw: [-60, 60], headPitch: [-25, 35], waistYaw: [-30, 30] },
};

export const SCENARIOS = [
  { id: "default", label: "기본 도킹", description: "도크 정면에서 탐색 → 접근 → 정렬 → 5초 충전 확인" },
  { id: "offset", label: "옆에서 정렬", description: "측면 오프셋과 초기 방향 오차가 있는 도킹" },
  { id: "blocked", label: "가려진 도크", description: "장애물이 beacon 시야를 가리면 탐색 timeout으로 정지" },
  { id: "low-battery", label: "배터리 24%", description: "SOC 25% 미만에서 자동 도킹 시작" },
];

const SAFETY_FLAGS = ["estop", "bumper", "cliff", "tilt"];
const FLAG_LABELS = {
  estop: "E-stop", bumper: "범퍼 감지", cliff: "낭떠러지 감지",
  tilt: "기울기 한계 초과", beaconLoss: "도크 beacon 유실",
  chargerFailure: "충전 확인 실패", networkLoss: "허브 연결 끊김",
};
const DOCK_MOTION = new Set(["DOCK_SEARCH", "DOCK_APPROACH", "DOCK_ALIGN", "DOCK_VERIFY", "DOCK_RETRY"]);
const DOCKED = new Set(["CHARGING", "DOCKED_READY", "DOCKED_WELCOME"]);
const finite = (value) => typeof value === "number" && Number.isFinite(value);
const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const wrap = (angle) => Math.atan2(Math.sin(angle), Math.cos(angle));
const distance = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);
const approach = (value, target, delta) => value + clamp(target - value, -delta, delta);

function segmentDistance(point, start, end) {
  const dx = end.x - start.x;
  const dy = end.y - start.y;
  const length2 = dx * dx + dy * dy;
  const t = length2 ? clamp(((point.x - start.x) * dx + (point.y - start.y) * dy) / length2, 0, 1) : 0;
  return Math.hypot(point.x - start.x - t * dx, point.y - start.y - t * dy);
}

function segmentSeparation(a, b, c, d) {
  const ab = { x: b.x - a.x, y: b.y - a.y };
  const cd = { x: d.x - c.x, y: d.y - c.y };
  const ac = { x: c.x - a.x, y: c.y - a.y };
  const cross = (u, v) => u.x * v.y - u.y * v.x;
  const denominator = cross(ab, cd);
  if (Math.abs(denominator) > 1e-12) {
    const t = cross(ac, cd) / denominator;
    const u = cross(ac, ab) / denominator;
    if (t >= 0 && t <= 1 && u >= 0 && u <= 1) return 0;
  }
  return Math.min(segmentDistance(a, c, d), segmentDistance(b, c, d), segmentDistance(c, a, b), segmentDistance(d, a, b));
}

/**
 * Public commands return false for rejected inputs and leave motion safe.
 * Snapshot positions/ToF are meters; theta/bearing/wheelAngles are radians.
 * velocity.left/right are wheel tangential m/s, angular is rad/s; joints are degrees.
 */
export class Simulator {
  constructor(scenario = "default") {
    this.reset(scenario);
  }

  reset(scenario = "default") {
    if (!SCENARIOS.some((entry) => entry.id === scenario)) throw new RangeError("Unknown scenario");
    this.scenario = scenario;
    this.time = 0;
    this.state = "IDLE";
    this.reason = "시작할 동작을 선택하세요";
    this.attempt = 0;
    this.world = { width: 3.2, height: 2.4, dock: { x: 2.9, y: 1.2, theta: 0 }, obstacles: [] };
    this.pose = { x: 1, y: 1.2, theta: 0 };
    if (scenario === "offset") this.pose = { x: 1.15, y: 0.65, theta: 0.7 };
    if (scenario === "blocked") this.world.obstacles = [{ x: 1.9, y: 1.2, r: 0.20 }];
    this.velocity = { linear: 0, angular: 0, left: 0, right: 0 };
    this.wheelAngles = { left: 0, right: 0 };
    this.joints = { headYaw: 0, headPitch: 0, waistYaw: 0 };
    this._jointTargets = { ...this.joints };
    this.battery = scenario === "low-battery" ? 24 : 68;
    this.faults = Object.fromEntries(Object.keys(FLAG_LABELS).map((name) => [name, false]));
    this.events = [];
    this.trail = [{ x: this.pose.x, y: this.pose.y }];
    this._manual = { linear: 0, angular: 0 };
    this._stateSince = 0;
    this._attemptSince = 0;
    this._searchElapsed = 0;
    this._contactSeconds = 0;
    this._verifySeconds = 0;
    this._staged = false;
    this._latched = false;
    this._suppressAutoDock = false;
    this._retryOrigin = null;
    this._passiveRelease = false;
    this._log("시나리오 초기화: " + SCENARIOS.find((entry) => entry.id === scenario).label);
    return this.snapshot();
  }

  _log(message) {
    this.events.push({ time: this.time, message });
    if (this.events.length > 100) this.events.shift();
  }

  _transition(state, reason) {
    if (state === this.state && reason === this.reason) return;
    this.state = state;
    this.reason = reason;
    this._stateSince = this.time;
    this._log(state + ": " + reason);
  }

  _halt() {
    this.velocity = { linear: 0, angular: 0, left: 0, right: 0 };
    this._manual = { linear: 0, angular: 0 };
  }

  _fault(reason) {
    this._latched = true;
    this._halt();
    // Torque-off freezes the visible pose rather than teleporting to neutral.
    this._jointTargets = { ...this.joints };
    this._transition("FAULT", reason + " · 원인 해제 후 오류 해제 필요");
  }

  _activeSafety() {
    const flag = SAFETY_FLAGS.find((name) => this.faults[name]);
    return flag ? FLAG_LABELS[flag] : this.battery < CONFIG.criticalSoc ? "SOC 15% 미만 · 수동 도킹 필요" : null;
  }

  setFault(name, enabled) {
    if (!Object.hasOwn(this.faults, name) || typeof enabled !== "boolean") return false;
    this.faults[name] = enabled;
    this._log(FLAG_LABELS[name] + (enabled ? " 주입" : " 해제"));
    if (enabled && SAFETY_FLAGS.includes(name)) this._fault(FLAG_LABELS[name]);
    if (enabled && name === "networkLoss" && ["IDLE", "MANUAL", "WELCOME"].includes(this.state)) {
      this._halt();
      this._jointTargets = { ...this.joints };
      this._transition("IDLE", "허브 연결 끊김 · 원격 동작 정지");
    }
    if (enabled && name === "chargerFailure" && DOCKED.has(this.state)) {
      this._halt();
      this._jointTargets = { ...this.joints };
      this._transition("DOCK_FAILED", "충전기 오류 · 모든 동작 정지");
    }
    return true;
  }

  setBattery(percent) {
    if (!finite(percent) || percent < 0 || percent > 100) return false;
    this.battery = percent;
    if (percent >= CONFIG.autoDockSoc) this._suppressAutoDock = false;
    if (percent < CONFIG.criticalSoc) this._fault("SOC 15% 미만 · 수동 도킹 필요");
    return true;
  }

  setManual(linear, angular) {
    if (!finite(linear) || !finite(angular) || this._latched || this._activeSafety() || this.faults.networkLoss) return false;
    if (this._sensors().contact || this.battery < CONFIG.autoDockSoc || DOCK_MOTION.has(this.state) || this.state === "DOCK_FAILED") return false;
    this._manual = {
      linear: clamp(linear, -CONFIG.maxSpeed, CONFIG.maxSpeed),
      angular: clamp(angular, -CONFIG.maxAngularSpeed, CONFIG.maxAngularSpeed),
    };
    this._transition("MANUAL", "수동 속도 명령");
    return true;
  }

  setJoint(name, degrees) {
    if (!Object.hasOwn(CONFIG.jointLimits, name) || !finite(degrees) || this._latched || this._activeSafety() || this.faults.networkLoss) return false;
    if (this._sensors().contact || DOCK_MOTION.has(this.state) || this.state.includes("WELCOME") || this.state === "DOCK_FAILED") return false;
    const [low, high] = CONFIG.jointLimits[name];
    this._jointTargets[name] = clamp(degrees, low, high);
    return true;
  }

  command(name) {
    if (name === "stop") {
      this._halt();
      this._jointTargets = { ...this.joints };
      this._suppressAutoDock = true;
      if (!this._latched) {
        const state = DOCKED.has(this.state) && this._sensors().contact ? this.battery >= CONFIG.targetSoc ? "DOCKED_READY" : "CHARGING" : "IDLE";
        this._transition(state, "사용자 정지 · 새 명령을 기다립니다");
      }
      return true;
    }
    if (name === "clearFault") {
      if ((!this._latched && this.state !== "DOCK_FAILED") || this._activeSafety()) return false;
      const contact = this._sensors().contact;
      if (contact && this.faults.chargerFailure) return false;
      this._latched = false;
      this._halt();
      this._suppressAutoDock = true;
      if (contact) {
        this._verifySeconds = 0;
        this._attemptSince = this.time;
        this.attempt = Math.max(1, this.attempt);
        this._transition("DOCK_VERIFY", "도크 접점에서 오류 해제 · 충전 5초 재확인");
      } else this._transition("IDLE", "오류 해제 · 새 명령을 기다립니다");
      return true;
    }
    if (this._latched || this._activeSafety() || this.faults.networkLoss) return false;
    if (name === "dock") {
      if (this._sensors().contact) return false;
      this._startDock();
      return true;
    }
    if (name === "welcome") {
      const docked = this._sensors().contact;
      if (docked && (!DOCKED.has(this.state) || this._contactSeconds < CONFIG.verifySeconds || this.faults.chargerFailure)) return false;
      if (DOCK_MOTION.has(this.state) || this.battery < CONFIG.autoDockSoc) return false;
      this._halt();
      this._transition(docked ? "DOCKED_WELCOME" : "WELCOME", docked ? "접점 안정 · 바퀴 정지 상태에서 3초 환영" : "바퀴 정지 상태에서 3초 환영");
      return true;
    }
    return false;
  }

  _startDock() {
    this._halt();
    this._jointTargets = { headYaw: 0, headPitch: 0, waistYaw: 0 };
    this.attempt = 1;
    this._attemptSince = this.time;
    this._searchElapsed = 0;
    this._staged = false;
    this._suppressAutoDock = false;
    this._transition("DOCK_SEARCH", "도크 beacon 탐색 · 시도 1/2");
  }

  _contactTarget() {
    const dock = this.world.dock;
    return { x: dock.x - CONFIG.radius * Math.cos(dock.theta), y: dock.y - CONFIG.radius * Math.sin(dock.theta) };
  }

  _sensors() {
    const { pose, world } = this;
    const dock = world.dock;
    const bearing = wrap(Math.atan2(dock.y - pose.y, dock.x - pose.x) - pose.theta);
    const occluded = world.obstacles.some((o) => segmentDistance(o, pose, dock) <= o.r);
    const beacon = !this.faults.beaconLoss && distance(pose, dock) <= CONFIG.dockRange && Math.abs(bearing) <= Math.PI / 2 && !occluded;
    const contact = distance(pose, this._contactTarget()) <= 0.006 && Math.abs(wrap(pose.theta - dock.theta)) <= Math.PI / 15;
    const dx = Math.cos(pose.theta);
    const dy = Math.sin(pose.theta);
    let rayDistance = CONFIG.dockRange + CONFIG.radius;
    for (const wall of [
      dx > 1e-9 ? (world.width - pose.x) / dx : dx < -1e-9 ? -pose.x / dx : Infinity,
      dy > 1e-9 ? (world.height - pose.y) / dy : dy < -1e-9 ? -pose.y / dy : Infinity,
    ]) rayDistance = Math.min(rayDistance, wall);
    for (const o of world.obstacles) {
      const ox = o.x - pose.x, oy = o.y - pose.y;
      const projection = ox * dx + oy * dy;
      const discriminant = o.r * o.r - (ox * ox + oy * oy - projection * projection);
      if (discriminant >= 0 && projection >= 0) rayDistance = Math.min(rayDistance, Math.max(0, projection - Math.sqrt(discriminant)));
    }
    // Dock's contact surface is a 24 cm line normal to dock.theta.
    const nx = Math.cos(dock.theta), ny = Math.sin(dock.theta);
    const denominator = dx * nx + dy * ny;
    if (denominator > 1e-9) {
      const ray = ((dock.x - pose.x) * nx + (dock.y - pose.y) * ny) / denominator;
      const lateral = -(pose.x + ray * dx - dock.x) * ny + (pose.y + ray * dy - dock.y) * nx;
      if (ray >= 0 && Math.abs(lateral) <= CONFIG.dockWidth / 2) rayDistance = Math.min(rayDistance, ray);
    }
    return {
      tof: Math.max(0, rayDistance - CONFIG.radius), beacon, bearing, contact,
      charging: contact && !this.faults.chargerFailure && ["CHARGING", "DOCKED_READY", "DOCKED_WELCOME"].includes(this.state),
    };
  }

  _driveToward(target, speed) {
    const delta = distance(this.pose, target);
    const error = wrap(Math.atan2(target.y - this.pose.y, target.x - this.pose.x) - this.pose.theta);
    return {
      linear: Math.abs(error) > 0.35 ? 0 : Math.min(speed, delta * 1.8) * Math.max(0, Math.cos(error)),
      angular: clamp(3 * error, -CONFIG.maxAngularSpeed, CONFIG.maxAngularSpeed),
    };
  }

  _behavior(dt, sensors) {
    let target = { linear: 0, angular: 0 };
    if (this.state === "MANUAL") return this._manual;
    if (DOCK_MOTION.has(this.state) && this.time - this._attemptSince > CONFIG.attemptTimeout) {
      this._halt();
      this._transition("DOCK_FAILED", "도킹 시도 시간 초과");
    }
    if (["DOCK_APPROACH", "DOCK_ALIGN"].includes(this.state) && !sensors.beacon) {
      this._halt();
      this._transition("DOCK_SEARCH", "beacon 유실 · 정지 후 재탐색");
    }
    if (this.state === "DOCK_SEARCH") {
      this._searchElapsed += dt;
      if (sensors.beacon) {
        this._searchElapsed = 0;
        this._transition("DOCK_APPROACH", "beacon 확보 · 최대 0.08 m/s 접근");
      } else if (this._searchElapsed >= CONFIG.searchTimeout) {
        this._halt();
        this._transition("DOCK_FAILED", "beacon 탐색 timeout · 도크 가시거리와 장애물을 확인하세요");
      } else target.angular = 0.7;
    } else if (this.state === "DOCK_APPROACH") {
      const contact = this._contactTarget();
      const dock = this.world.dock;
      const staging = { x: contact.x - 0.20 * Math.cos(dock.theta), y: contact.y - 0.20 * Math.sin(dock.theta) };
      if (!this._staged && distance(this.pose, staging) < 0.018) this._staged = true;
      if (!this._staged) target = this._driveToward(staging, CONFIG.dockApproachSpeed);
      else {
        const error = wrap(dock.theta - this.pose.theta);
        target = Math.abs(error) > 0.06 ? { linear: 0, angular: clamp(3 * error, -0.8, 0.8) } : this._driveToward(contact, CONFIG.dockApproachSpeed);
        if (sensors.tof <= CONFIG.dockAlignGap) {
          this._transition("DOCK_ALIGN", "도크 150 mm 이내 · 최대 0.03 m/s 정렬");
          // Enforce the tighter limit immediately, including prior wheel speed.
          this._halt();
          target = { linear: 0, angular: 0 };
        }
      }
    } else if (this.state === "DOCK_ALIGN") {
      if (sensors.contact) {
        this._halt();
        this._verifySeconds = 0;
        this._transition("DOCK_VERIFY", "접점 감지 · 바퀴 정지 후 충전 5초 확인");
      } else target = this._driveToward(this._contactTarget(), CONFIG.dockAlignSpeed);
    } else if (this.state === "DOCK_VERIFY") {
      this._halt();
      if (sensors.contact && !this.faults.chargerFailure) this._verifySeconds += dt;
      else this._verifySeconds = 0;
      if (this._verifySeconds + 1e-9 >= CONFIG.verifySeconds) this._transition("CHARGING", "5초 충전 확인 완료");
      else if (this.time - this._stateSince >= CONFIG.verifySeconds + 1) {
        if (this.attempt >= CONFIG.maxAttempts) this._transition("DOCK_FAILED", "두 번째 충전 확인 실패 · 사용자 복구 필요");
        else {
          this._retryOrigin = { ...this.pose };
          this._passiveRelease = true;
          this._transition("DOCK_RETRY", "접점 수동 해제 모델 → 12 cm 후퇴 → 마지막 재시도");
        }
      }
    } else if (this.state === "DOCK_RETRY") {
      if (this._passiveRelease) {
        // Explicit ideal assumption: an unpowered dock spring releases contact.
        // Powered reverse begins only on a subsequent tick with contact=false.
        const release = { x: this.pose.x - 0.01 * Math.cos(this.pose.theta), y: this.pose.y - 0.01 * Math.sin(this.pose.theta), theta: this.pose.theta };
        if (this._collision(this.pose, release)) this._fault("후퇴 공간 없음");
        else { this.pose = release; this._log("가정: 도크가 무동력으로 접점을 1 cm 해제"); }
        this._passiveRelease = false;
      } else if (distance(this.pose, this._retryOrigin) >= CONFIG.retryDistance) {
        this._halt();
        this.attempt += 1;
        this._attemptSince = this.time;
        this._searchElapsed = 0;
        this._staged = true;
        this._transition("DOCK_SEARCH", "후퇴 완료 · 시도 2/2");
      } else target.linear = -CONFIG.dockAlignSpeed;
    }
    return target;
  }

  _collision(start, end, curveMargin = 0) {
    // The chord's swept disk plus arc sagitta bounds the complete curved path.
    const r = CONFIG.radius + curveMargin;
    if (end.x < r || end.y < r || end.x > this.world.width - r || end.y > this.world.height - r) return true;
    if (this.world.obstacles.some((o) => segmentDistance(o, start, end) < r + o.r + 1e-6)) return true;
    const dock = this.world.dock;
    const nx = Math.cos(dock.theta), ny = Math.sin(dock.theta);
    const local = (p) => ({ x: (p.x - dock.x) * nx + (p.y - dock.y) * ny, y: -(p.x - dock.x) * ny + (p.y - dock.y) * nx });
    const a = local(start), b = local(end);
    const half = CONFIG.dockWidth / 2;
    const inside = (p) => p.x >= 0 && p.x <= CONFIG.dockDepth && Math.abs(p.y) <= half;
    if (inside(a) || inside(b)) return true;
    const corners = [{ x: 0, y: -half }, { x: CONFIG.dockDepth, y: -half }, { x: CONFIG.dockDepth, y: half }, { x: 0, y: half }];
    return corners.some((corner, i) => segmentSeparation(a, b, corner, corners[(i + 1) % 4]) < r);
  }

  _integrate(target, dt) {
    let left = target.linear - target.angular * CONFIG.track / 2;
    let right = target.linear + target.angular * CONFIG.track / 2;
    const scale = Math.max(1, Math.abs(left) / CONFIG.maxSpeed, Math.abs(right) / CONFIG.maxSpeed);
    left = approach(this.velocity.left, left / scale, CONFIG.maxAcceleration * dt);
    right = approach(this.velocity.right, right / scale, CONFIG.maxAcceleration * dt);
    const linear = (left + right) / 2;
    const angular = (right - left) / CONFIG.track;
    const theta = this.pose.theta;
    const nextTheta = theta + angular * dt;
    const next = {
      x: this.pose.x + (Math.abs(angular) < 1e-9 ? linear * Math.cos(theta) * dt : linear / angular * (Math.sin(nextTheta) - Math.sin(theta))),
      y: this.pose.y + (Math.abs(angular) < 1e-9 ? linear * Math.sin(theta) * dt : linear / angular * (Math.cos(theta) - Math.cos(nextTheta))),
      theta: wrap(nextTheta),
    };
    const curveMargin = Math.abs(angular) < 1e-9 ? 0 : Math.abs(linear / angular) * (1 - Math.cos(angular * dt / 2));
    if (this._collision(this.pose, next, curveMargin)) { this._fault("장애물 또는 경계 접촉"); return; }
    this.pose = next;
    this.velocity = { linear, angular, left, right };
    this.wheelAngles.left += left / CONFIG.wheelRadius * dt;
    this.wheelAngles.right += right / CONFIG.wheelRadius * dt;
  }

  _tick(dt) {
    this.time += dt;
    const safety = this._activeSafety();
    if (safety && !this._latched) this._fault(safety);
    let sensors = this._sensors();
    this._contactSeconds = sensors.contact ? this._contactSeconds + dt : 0;
    if (this._latched) return;
    if (this.battery < CONFIG.autoDockSoc && !this._suppressAutoDock && ["IDLE", "MANUAL", "WELCOME"].includes(this.state) && !sensors.contact) this._startDock();
    let target = this._behavior(dt, sensors);
    sensors = this._sensors();
    if (sensors.contact) { this._halt(); target = { linear: 0, angular: 0 }; }
    if (["FAULT", "DOCK_FAILED", "IDLE"].includes(this.state)) this._halt();
    else this._integrate(target, dt);
    sensors = this._sensors();
    // Contact arriving during integration must stop on the same simulation tick.
    if (sensors.contact) this._halt();
    if (DOCKED.has(this.state) && (!sensors.contact || this.faults.chargerFailure)) {
      this._halt();
      this._jointTargets = { ...this.joints };
      this._transition("DOCK_FAILED", "충전 접점 또는 충전기 오류 · 모든 동작 정지");
    }
    if (this.state.includes("WELCOME")) {
      const elapsed = this.time - this._stateSince;
      if (elapsed + 1e-9 >= CONFIG.welcomeSeconds) {
        this._jointTargets = { headYaw: 0, headPitch: 0, waistYaw: 0 };
        if (this.state === "DOCKED_WELCOME") {
          // End bounded docked torque immediately, without an extra return phase.
          this._jointTargets = { ...this.joints };
          this._transition(this.battery >= CONFIG.targetSoc ? "DOCKED_READY" : "CHARGING", "환영 완료 · 표현 관절 정지");
        } else this._transition("IDLE", "환영 완료");
      } else {
        const fade = Math.sin(Math.PI * elapsed / CONFIG.welcomeSeconds);
        this._jointTargets = {
          headYaw: 15 * Math.sin(elapsed * 2 * Math.PI) * fade,
          headPitch: 10 * Math.sin(elapsed * Math.PI) * fade,
          waistYaw: 12 * Math.sin(elapsed * 2 * Math.PI + 0.6) * fade,
        };
      }
    }
    const jointsEnabled = !this._latched && this.state !== "DOCK_FAILED" && (!sensors.contact || this.state === "DOCKED_WELCOME");
    if (jointsEnabled) {
      for (const name of Object.keys(this.joints)) {
        const [low, high] = CONFIG.jointLimits[name];
        const targetAngle = clamp(this._jointTargets[name], Math.max(low, this.state === "DOCKED_WELCOME" ? -15 : low), Math.min(high, this.state === "DOCKED_WELCOME" ? 15 : high));
        this.joints[name] = approach(this.joints[name], targetAngle, CONFIG.jointSpeed * dt);
      }
    }
    if (sensors.charging) {
      this.battery = Math.min(100, this.battery + CONFIG.chargeRate * dt);
      if (this.battery >= CONFIG.targetSoc && this.state === "CHARGING") this._transition("DOCKED_READY", "목표 SOC 도달 · 도크 대기");
    }
    if (distance(this.pose, this.trail.at(-1)) > 0.008) {
      this.trail.push({ x: this.pose.x, y: this.pose.y });
      if (this.trail.length > 900) this.trail.shift();
    }
  }

  step(dt = CONFIG.dt) {
    if (!finite(dt) || dt <= 0 || dt > 1) throw new RangeError("dt must be finite and in (0, 1]");
    const steps = Math.ceil(dt / CONFIG.dt);
    for (let i = 0; i < steps; i += 1) this._tick(dt / steps);
    return this.snapshot();
  }

  snapshot() {
    return JSON.parse(JSON.stringify({
      time: this.time, state: this.state, reason: this.reason, attempt: this.attempt,
      pose: this.pose, velocity: this.velocity, wheelAngles: this.wheelAngles,
      joints: this.joints, battery: this.battery, faults: this.faults,
      sensors: this._sensors(), world: this.world, events: this.events, trail: this.trail,
    }));
  }
}

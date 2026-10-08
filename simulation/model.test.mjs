import test from "node:test";
import assert from "node:assert/strict";
import { CONFIG, SCENARIOS, Simulator } from "./model.mjs";

function runFor(sim, seconds) {
  for (let i = 0; i < Math.ceil(seconds / CONFIG.dt); i += 1) sim.step();
  return sim.snapshot();
}

function until(sim, predicate, seconds = 70) {
  for (let i = 0; i < seconds / CONFIG.dt; i += 1) {
    const snapshot = sim.step();
    if (predicate(snapshot)) return snapshot;
  }
  assert.fail(`Condition not reached; state=${sim.state}, reason=${sim.reason}`);
}

function close(actual, expected, tolerance = 1e-8) {
  assert.ok(Math.abs(actual - expected) <= tolerance, `${actual} differs from ${expected}`);
}

for (const scenario of ["default", "offset"]) {
  test(`${scenario} docks, halts wheels on contact, and verifies five seconds of charging`, () => {
    const sim = new Simulator(scenario);
    sim.setJoint("headYaw", 60);
    runFor(sim, 1);
    assert.equal(sim.command("dock"), true);
    const states = new Set();
    let verifyStart;
    const end = until(sim, (s) => {
      states.add(s.state);
      if (s.state === "DOCK_VERIFY" && verifyStart === undefined) verifyStart = s.time;
      if (s.sensors.contact) {
        assert.equal(s.velocity.left, 0);
        assert.equal(s.velocity.right, 0);
      }
      if (s.state === "DOCK_ALIGN") assert.ok(Math.abs(s.velocity.linear) <= CONFIG.dockAlignSpeed + 1e-9);
      return s.state === "CHARGING";
    });
    assert.ok(states.has("DOCK_APPROACH"));
    assert.ok(states.has("DOCK_ALIGN"));
    assert.ok(states.has("DOCK_VERIFY"));
    assert.ok(end.time - verifyStart >= CONFIG.verifySeconds - 1e-8);
    assert.equal(end.attempt, 1);
    assert.equal(end.sensors.contact, true);
    assert.equal(end.sensors.charging, true);
    assert.ok(Math.abs(end.pose.y - end.world.dock.y) < 0.03);
    assert.ok(Math.abs(end.pose.theta) < Math.PI / 15);
    close(end.joints.headYaw, 0);
  });
}

test("blocked LOS and injected beacon loss time out without forward motion", () => {
  for (const mode of ["blocked", "injected"]) {
    const sim = new Simulator(mode === "blocked" ? "blocked" : "default");
    if (mode === "injected") sim.setFault("beaconLoss", true);
    const start = sim.snapshot().pose;
    sim.command("dock");
    const end = until(sim, (s) => s.state === "DOCK_FAILED", 15);
    close(end.pose.x, start.x);
    close(end.pose.y, start.y);
    assert.equal(end.velocity.linear, 0);
    assert.equal(end.velocity.angular, 0);
    assert.equal(end.attempt, 1);
  }
});

test("beacon loss during approach stops translation and may reacquire locally", () => {
  const sim = new Simulator();
  sim.command("dock");
  runFor(sim, 2);
  sim.setFault("beaconLoss", true);
  sim.step();
  assert.equal(sim.state, "DOCK_SEARCH");
  assert.equal(sim.velocity.linear, 0);
  sim.setFault("beaconLoss", false);
  assert.equal(until(sim, (s) => s.state === "CHARGING").state, "CHARGING");
});

test("charger failure has exactly one contact-release/backoff and two attempts", () => {
  const sim = new Simulator();
  sim.setFault("chargerFailure", true);
  sim.command("dock");
  const end = until(sim, (s) => {
    if (s.sensors.contact) assert.equal(Math.abs(s.velocity.left) + Math.abs(s.velocity.right), 0);
    return s.state === "DOCK_FAILED";
  });
  assert.equal(end.attempt, 2);
  assert.equal(end.sensors.charging, false);
  assert.equal(end.events.filter((e) => e.message.startsWith("DOCK_RETRY:")).length, 1);
  assert.equal(end.events.filter((e) => e.message.startsWith("DOCK_VERIFY:")).length, 2);
  assert.equal(end.velocity.linear, 0);
  const pose = end.pose;
  runFor(sim, 20);
  assert.deepEqual(sim.pose, pose);
});

test("clearing charging failure during retry permits second-attempt charging", () => {
  const sim = new Simulator();
  sim.setFault("chargerFailure", true);
  sim.command("dock");
  until(sim, (s) => s.state === "DOCK_RETRY");
  sim.setFault("chargerFailure", false);
  const end = until(sim, (s) => s.state === "CHARGING");
  assert.equal(end.attempt, 2);
});

for (const flag of ["estop", "bumper", "cliff", "tilt"]) {
  test(`${flag} immediately freezes all motion and requires cause clear plus explicit clear`, () => {
    const sim = new Simulator();
    sim.setManual(0.1, 0.2);
    sim.setJoint("headYaw", 60);
    runFor(sim, 0.2);
    sim.setFault(flag, true);
    const stopped = sim.snapshot();
    assert.equal(stopped.state, "FAULT");
    assert.equal(stopped.velocity.linear, 0);
    assert.equal(sim.command("clearFault"), false);
    assert.equal(sim.command("stop"), true);
    assert.equal(sim.state, "FAULT");
    assert.equal(sim.command("dock"), false);
    assert.equal(sim.setJoint("headYaw", 0), false);
    assert.equal(sim.setManual(0.1, 0), false);
    sim.setFault(flag, false);
    runFor(sim, 0.5);
    assert.equal(sim.state, "FAULT");
    assert.deepEqual(sim.pose, stopped.pose);
    assert.deepEqual(sim.joints, stopped.joints);
    assert.equal(sim.command("clearFault"), true);
    runFor(sim, 0.5);
    assert.deepEqual(sim.pose, stopped.pose);
    assert.equal(sim.setManual(-0.1, 0), true);
    assert.notEqual(runFor(sim, 0.5).pose.x, stopped.pose.x);
  });
}

test("network disconnect stops remote wheel/joint motion without automatic resume", () => {
  const sim = new Simulator();
  sim.setManual(0.1, 0);
  sim.setJoint("headYaw", 60);
  runFor(sim, 0.1);
  sim.setFault("networkLoss", true);
  const stopped = sim.snapshot();
  assert.equal(stopped.velocity.linear, 0);
  assert.equal(sim.command("dock"), false);
  runFor(sim, 1);
  assert.deepEqual(sim.pose, stopped.pose);
  assert.deepEqual(sim.joints, stopped.joints);
  sim.setFault("networkLoss", false);
  runFor(sim, 1);
  assert.deepEqual(sim.pose, stopped.pose);
  assert.deepEqual(sim.joints, stopped.joints);
  const jointOnly = new Simulator();
  jointOnly.setJoint("headYaw", 60);
  jointOnly.step();
  jointOnly.setFault("networkLoss", true);
  const angle = jointOnly.joints.headYaw;
  runFor(jointOnly, 1);
  assert.equal(jointOnly.joints.headYaw, angle);
});

test("autonomous docking continues locally after network loss", () => {
  const sim = new Simulator();
  sim.command("dock");
  runFor(sim, 1);
  sim.setFault("networkLoss", true);
  assert.equal(until(sim, (s) => s.state === "CHARGING").sensors.charging, true);
});

test("SOC thresholds prioritize docking and latch critical battery stop", () => {
  const sim = new Simulator();
  sim.setBattery(25);
  sim.step();
  assert.equal(sim.state, "IDLE");
  sim.setBattery(24.99);
  sim.step();
  assert.equal(sim.state, "DOCK_APPROACH");
  assert.equal(sim.setManual(0.1, 0), false);
  sim.setBattery(15);
  sim.step();
  assert.notEqual(sim.state, "FAULT");
  sim.setBattery(14.99);
  assert.equal(sim.state, "FAULT");
  assert.equal(sim.command("clearFault"), false);
  sim.setBattery(40);
  assert.equal(sim.state, "FAULT");
  assert.equal(sim.command("clearFault"), true);
  sim.step();
  assert.equal(sim.state, "IDLE");
  const low = new Simulator("low-battery");
  assert.equal(until(low, (s) => s.state === "CHARGING").attempt, 1);
});

test("user stop stays stopped despite low SOC until a new dock command", () => {
  const sim = new Simulator("low-battery");
  runFor(sim, 1);
  sim.command("stop");
  const pose = { ...sim.pose };
  runFor(sim, 2);
  assert.equal(sim.state, "IDLE");
  assert.deepEqual(sim.pose, pose);
  assert.equal(sim.command("dock"), true);
});

test("wheel kinematics, acceleration and joint bounds are enforced", () => {
  const sim = new Simulator();
  sim.setManual(100, 0);
  const first = sim.step();
  close(first.velocity.linear, CONFIG.maxAcceleration * CONFIG.dt);
  runFor(sim, 0.6);
  close(sim.velocity.linear, CONFIG.maxSpeed);
  close(sim.velocity.left, sim.velocity.right);
  close(sim.pose.y, 1.2);
  close(sim.pose.theta, 0);
  close(sim.wheelAngles.left * CONFIG.wheelRadius, sim.pose.x - 1);
  sim.command("stop");
  const x = sim.pose.x;
  sim.setManual(0, 100);
  runFor(sim, 1);
  close(sim.pose.x, x);
  close(sim.pose.y, 1.2);
  assert.ok(sim.pose.theta > 0);
  close(sim.velocity.left, -sim.velocity.right);
  close(sim.velocity.angular, (sim.velocity.right - sim.velocity.left) / CONFIG.track);
  sim.setManual(100, 100);
  runFor(sim, 0.7);
  assert.ok(Math.abs(sim.velocity.left) <= CONFIG.maxSpeed);
  assert.ok(Math.abs(sim.velocity.right) <= CONFIG.maxSpeed);
  sim.command("stop");
  sim.setJoint("headYaw", 999);
  sim.setJoint("headPitch", -999);
  sim.setJoint("waistYaw", 999);
  runFor(sim, 2);
  assert.deepEqual(sim.joints, { headYaw: 60, headPitch: -25, waistYaw: 30 });
});

test("large requested steps still cannot tunnel through obstacles or walls", () => {
  const obstacleSim = new Simulator("blocked");
  obstacleSim.setManual(0.15, 0);
  for (let i = 0; i < 12; i += 1) obstacleSim.step(1);
  assert.equal(obstacleSim.state, "FAULT");
  const obstacle = obstacleSim.world.obstacles[0];
  assert.ok(Math.hypot(obstacleSim.pose.x - obstacle.x, obstacleSim.pose.y - obstacle.y) >= obstacle.r + CONFIG.radius);
  const wallSim = new Simulator();
  wallSim.setManual(-0.15, 0);
  for (let i = 0; i < 12; i += 1) wallSim.step(1);
  assert.equal(wallSim.state, "FAULT");
  assert.ok(wallSim.pose.x >= CONFIG.radius);
  assert.equal(wallSim.velocity.linear, 0);
  wallSim.command("clearFault");
  assert.equal(wallSim.setManual(0.1, 0), true);
  runFor(wallSim, 1);
  assert.equal(wallSim.state, "MANUAL");
});

test("docked welcome is limited to three seconds and ±15 degrees with wheels stopped", () => {
  const sim = new Simulator();
  sim.command("dock");
  until(sim, (s) => s.state === "DOCK_VERIFY");
  assert.equal(sim.command("welcome"), false);
  until(sim, (s) => s.state === "CHARGING");
  assert.equal(sim.setManual(-0.1, 0), false);
  assert.equal(sim.setJoint("headYaw", 20), false);
  const pose = { ...sim.pose };
  const start = sim.time;
  assert.equal(sim.command("welcome"), true);
  const end = until(sim, (s) => {
    assert.deepEqual(s.pose, pose);
    assert.equal(s.velocity.linear, 0);
    for (const angle of Object.values(s.joints)) assert.ok(Math.abs(angle) <= 15);
    return s.state === "CHARGING";
  }, 4);
  assert.ok(end.time - start <= CONFIG.welcomeSeconds + CONFIG.dt);
  sim.command("welcome");
  runFor(sim, 0.5);
  sim.setFault("chargerFailure", true);
  const joints = { ...sim.joints };
  assert.equal(sim.state, "DOCK_FAILED");
  runFor(sim, 1);
  assert.deepEqual(sim.joints, joints);
});

test("clearFault preserves healthy charging and re-verifies recovered faults at contact", () => {
  const sim = new Simulator();
  sim.command("dock");
  until(sim, (s) => s.state === "CHARGING");
  assert.equal(sim.command("clearFault"), false);
  assert.equal(sim.state, "CHARGING");
  sim.setFault("estop", true);
  sim.setFault("estop", false);
  assert.equal(sim.command("clearFault"), true);
  assert.equal(sim.state, "DOCK_VERIFY");
  assert.equal(sim.snapshot().sensors.charging, false);
  const recoveryTime = sim.time;
  until(sim, (s) => s.state === "CHARGING");
  assert.ok(sim.time - recoveryTime >= CONFIG.verifySeconds - 1e-8);
  sim.setFault("chargerFailure", true);
  assert.equal(sim.command("clearFault"), false);
  sim.setFault("chargerFailure", false);
  assert.equal(sim.command("clearFault"), true);
  assert.equal(until(sim, (s) => s.state === "CHARGING").sensors.charging, true);
});

test("manual motion cannot cross the dock body outside the contact alignment window", () => {
  const sim = new Simulator();
  // Place the fixture beside the docking centerline, while still in front of its body.
  sim.pose = { x: 2.5, y: 1.28, theta: 0 };
  sim.setManual(0.15, 0);
  runFor(sim, 4);
  assert.equal(sim.state, "FAULT");
  assert.ok(sim.pose.x <= sim.world.dock.x - CONFIG.radius);
  assert.equal(sim.snapshot().sensors.contact, false);
  assert.equal(sim.velocity.linear, 0);
});

test("invalid public inputs are rejected without corrupting finite state", () => {
  const sim = new Simulator();
  for (const value of [NaN, Infinity, -Infinity, "0", null, undefined]) {
    assert.equal(sim.setBattery(value), false);
    assert.equal(sim.setManual(value, 0), false);
    assert.equal(sim.setManual(0, value), false);
    assert.equal(sim.setJoint("headYaw", value), false);
  }
  assert.equal(sim.setBattery(-1), false);
  assert.equal(sim.setBattery(101), false);
  assert.equal(sim.setJoint("unknown", 0), false);
  assert.equal(sim.setFault("unknown", true), false);
  assert.equal(sim.setFault("estop", "true"), false);
  assert.equal(sim.command("unknown"), false);
  for (const dt of [NaN, Infinity, 0, -1, 2]) assert.throws(() => sim.step(dt), RangeError);
  assert.throws(() => sim.reset("unknown"), RangeError);
  assert.equal(sim.state, "IDLE");
  assert.equal(sim.time, 0);
  assert.equal(sim.snapshot().pose.x, 1);
});

test("snapshots are detached JSON data and fixed-step runs are deterministic", () => {
  const a = new Simulator();
  const b = new Simulator();
  a.command("dock"); b.command("dock");
  runFor(a, 33); runFor(b, 33);
  assert.deepEqual(a.snapshot(), b.snapshot());
  const snapshot = a.snapshot();
  snapshot.world.obstacles.push({ x: 1, y: 1, r: 10 });
  snapshot.pose.x = 999;
  assert.notEqual(a.pose.x, 999);
  assert.equal(a.world.obstacles.length, 0);
  assert.equal(SCENARIOS.length, 4);
});

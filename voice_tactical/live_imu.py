"""Live NU87 I2C -> local observations -> BLE OLED, with no replay fallback."""

from __future__ import annotations

from collections import deque
from copy import deepcopy
import math
import threading
import time

from .oled_scene import scene_command


def valid_packet(packet):
    if not isinstance(packet, dict) or packet.get("source") != "nu87-i2c":
        return False
    if any(packet.get(key) is not True for key in ("configured", "ready", "fresh")):
        return False
    if packet.get("whoAmI") not in (0x69, 0x6A):
        return False
    for key in ("sample", "ageMs", "uptimeMs", "errors"):
        if type(packet.get(key)) is not int or packet[key] < 0:
            return False
    if packet["sample"] == 0 or packet["ageMs"] > 500:
        return False
    for key, limit in (("accelMg", 2200), ("gyroMdps", 300_000)):
        values = packet.get(key)
        if not isinstance(values, list) or len(values) != 3:
            return False
        if any(type(value) is not int or abs(value) > limit for value in values):
            return False
    return True


class LiveImu:
    def __init__(self, gateway, engine, *, enabled=False, clock=time.monotonic):
        self.gateway, self.engine, self.enabled, self.clock = gateway, engine, enabled, clock
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.worker = None
        self.reference = None
        self.monitoring = False
        self.reference_samples = None
        self.reference_started = None
        self.last_received = None
        self.last_uptime = None
        self.last_sequence = None
        self.over_since = None
        self.latched = False
        self.history = deque(maxlen=100)
        self.events = deque(maxlen=30)
        self.packet = None
        self.orientation = None
        self.display_orientation = None
        self.oled = {"acknowledged": False, "visual_verified": False}
        self.error = None
        self.last_display = 0
        self.display_enabled = True
        self.last_display_state = None
        self.display_hold_until = 0
        self.robot_stop = None
        self.was_blocked = False
        # Keep TEXT until a compatible WG1 firmware is installed / selected.
        self.display_view = "text"
        self.virtual_motion = "stop"
        self.virtual_motion_until = 0

    def start(self):
        if self.enabled and self.worker is None:
            self.worker = threading.Thread(target=self._run, name="live-imu", daemon=True)
            self.worker.start()

    def _event(self, kind, message):
        event = {
            "kind": kind,
            "message": message,
            "at": time.time(),
            "measurement": deepcopy(self.packet),
            "orientation": deepcopy(self.orientation),
            "reference": deepcopy(self.reference),
        }
        self.events.append(event)
        self.engine.record_sensor_event(event)

    def feed(self, packet):
        with self.lock:
            if not valid_packet(packet):
                self.error = (
                    packet.get("error", "IMU 응답 또는 새 측정값을 확인하지 못했습니다.")
                    if isinstance(packet, dict)
                    else "IMU 응답 형식을 확인하지 못했습니다."
                )
                self.orientation = None
                self.display_orientation = None
                self.over_since = None
                self.reference_samples = None
                return False
            if self.last_uptime is not None and packet["uptimeMs"] < self.last_uptime:
                self.reference = None
                self.reference_samples = None
                self.last_sequence = None
                self.latched = False
                self.over_since = None
                self._event("reboot", "NU87 재부팅: 기준 자세를 다시 설정하세요.")
            self.last_uptime = packet["uptimeMs"]
            if packet["sample"] == self.last_sequence:
                return False
            now = self.clock()
            dt = None if self.last_received is None else now - self.last_received
            if self.last_received is not None and now - self.last_received > 1.5:
                self.over_since = None
            self.last_received, self.last_sequence = now, packet["sample"]
            self.packet = deepcopy(packet)
            self.error = None
            accel = [value / 1000 for value in packet["accelMg"]]
            gyro = [value / 1000 for value in packet["gyroMdps"]]
            norm = math.hypot(*accel)
            stable_gravity = 0.75 <= norm <= 1.25
            if stable_gravity:
                unit = [value / norm for value in accel]
                reference = self.reference or [0, 0, 1]
                tilt = math.degrees(
                    math.acos(max(-1, min(1, sum(a * b for a, b in zip(unit, reference)))))
                )
                roll = math.degrees(math.atan2(accel[1], accel[2]))
                pitch = math.degrees(math.atan2(-accel[0], math.hypot(accel[1], accel[2])))
                self.orientation = {
                    "roll": roll,
                    "pitch": pitch,
                    "tilt": tilt,
                    "gravity_valid": True,
                    "absolute_heading": None,
                }
                # Only the view/OLED is filtered. Warning thresholds, history,
                # event evidence and raw axes always use the current sample.
                if self.display_orientation is None or dt is None or dt > 1.5:
                    self.display_orientation = dict(self.orientation)
                else:
                    alpha = 1 - math.exp(-max(0, dt) / 0.24)
                    for key in ("roll", "pitch", "tilt"):
                        delta = self.orientation[key] - self.display_orientation[key]
                        if key == "roll":
                            delta = math.degrees(
                                math.atan2(
                                    math.sin(math.radians(delta)), math.cos(math.radians(delta))
                                )
                            )
                        self.display_orientation[key] += alpha * delta
            else:
                self.over_since = None
                self.orientation = {
                    "roll": None,
                    "pitch": None,
                    "tilt": None,
                    "gravity_valid": False,
                    "absolute_heading": None,
                }
                self.display_orientation = None
            self.history.append(
                {
                    "at": time.time(),
                    "sample": packet["sample"],
                    "accel": accel,
                    "gyro": gyro,
                    "orientation": deepcopy(self.orientation),
                }
            )
            if self.reference_samples is not None:
                if not stable_gravity or math.hypot(*gyro) > 12:
                    self.reference_samples = None
                    self.error = "기준 자세 설정 중 움직임이 큽니다. 내려놓고 다시 눌러 주세요."
                else:
                    self.reference_samples.append(accel)
                    samples = self.reference_samples
                    if len(samples) >= 12 and now - self.reference_started >= 2:
                        mean = [sum(row[i] for row in samples) / len(samples) for i in range(3)]
                        spread = max(
                            math.sqrt(
                                sum((row[i] - mean[i]) ** 2 for row in samples) / len(samples)
                            )
                            for i in range(3)
                        )
                        if spread > 0.025:
                            self.error = (
                                "기준 자세가 안정되지 않았습니다. 보드를 내려놓고 다시 설정하세요."
                            )
                        else:
                            self.reference = [value / math.hypot(*mean) for value in mean]
                            self.monitoring = True
                            self._event(
                                "reference", "실측 중력으로 기준 자세 설정 · 기울기 감시 시작"
                            )
                        self.reference_samples = None
                    elif now - self.reference_started > 20:
                        self.reference_samples = None
                        self.error = "기준 자세 설정 시간이 초과됐습니다."
            if self.reference and stable_gravity and not self.latched:
                if self.orientation["tilt"] >= 35:
                    if self.over_since is None:
                        self.over_since = now
                    elif now - self.over_since >= 0.6:
                        self.latched = True
                        self._event("tilt", "실측 기울기 35° 이상 지속 · 가상 왈라비 정지")
                else:
                    self.over_since = None
            return True

    def snapshot(self):
        with self.lock:
            age = None if self.last_received is None else (self.clock() - self.last_received) * 1000
            fresh = age is not None and age <= 1500 and self.error is None
            return {
                "enabled": self.enabled,
                "source": "nu87-i2c",
                "fresh": fresh,
                "host_age_ms": age,
                "packet": deepcopy(self.packet),
                "orientation": deepcopy(self.orientation) if fresh else None,
                "display_orientation": deepcopy(self.display_orientation) if fresh else None,
                "reference": deepcopy(self.reference),
                "calibrating": self.reference_samples is not None,
                "tilt_latched": self.latched,
                "motion_blocked": bool(
                    self.enabled
                    and (
                        self.latched
                        or not fresh
                        or not self.orientation
                        or not self.orientation["gravity_valid"]
                        or (self.monitoring and self.reference is None)
                    )
                ),
                "history": list(self.history),
                "events": list(self.events),
                "oled": deepcopy(self.oled),
                "error": self.error,
                "display_enabled": self.display_enabled,
                "display_view": self.display_view,
                "robot_stop": deepcopy(self.robot_stop),
                "boundary": (
                    "Actual IMU axes; gravity attitude estimate, "
                    "no absolute position/yaw or real motors."
                ),
            }

    def calibrate(self):
        with self.lock:
            if not self.enabled or not self.snapshot()["fresh"]:
                raise ValueError("새 IMU 실측값이 들어오는 상태에서 기준 자세를 설정하세요.")
            if self.latched:
                raise ValueError("기울기 경고를 먼저 해제하세요.")
            self.reference_samples = []
            self.reference_started = self.clock()
            self.reference = None
            self.over_since = None
            self.error = None
        return self.snapshot()

    def clear(self):
        with self.lock:
            state = self.snapshot()
            if (
                not state["fresh"]
                or not self.reference
                or not self.orientation
                or self.orientation["tilt"] is None
                or self.orientation["tilt"] > 15
            ):
                raise ValueError("실측 기울기가 기준 자세의 15° 이내일 때 해제할 수 있습니다.")
            self.latched = False
            self.over_since = None
            self._event("clear", "실측 자세 복귀 확인 · 기울기 경고 해제")
        return self.snapshot()

    def set_display(self, enabled, view=None):
        if type(enabled) is not bool:
            raise ValueError("표시 설정은 true 또는 false여야 합니다.")
        if view is not None and view not in ("text", "face", "horizon"):
            raise ValueError("OLED 화면은 text, face, horizon 중 선택하세요.")
        with self.lock:
            self.display_enabled = enabled
            if view is not None:
                self.display_view = view
            self.last_display_state = None
            self.display_hold_until = 0
        return self.snapshot()

    def set_virtual_motion(self, command):
        """Last requested browser patrol, never a measurement of real motors."""
        with self.lock:
            self.virtual_motion = "patrol" if command == "patrol" else "stop"
            self.virtual_motion_until = self.clock() + 60
            self.last_display_state = None

    def hold_display(self, text, result):
        """Leave a requested voice/report summary visible between live updates."""
        with self.lock:
            self.display_hold_until = self.clock() + 8
            self.oled = {
                **result,
                "text": text,
                "sample": None,
                "source": "requested-summary",
                "at": time.time(),
                "visual_verified": False,
            }

    def _run(self):
        while not self.stop.is_set():
            try:
                self.feed(self.gateway.imu_status())
                state = self.snapshot()
                state["alarm_active"] = bool(self.engine.alert_active)
                mode = (
                    "SIM ALERT"
                    if state["alarm_active"]
                    else "TILT - STOP"
                    if state["tilt_latched"]
                    else "LIVE IMU"
                    if state["fresh"]
                    else "IMU LINK STALE"
                )
                now = self.clock()
                if state["motion_blocked"] and not self.was_blocked:
                    result = self.gateway.robot_command("stop")
                    with self.lock:
                        self.robot_stop = result
                        self.virtual_motion = "stop"
                self.was_blocked = state["motion_blocked"]
                can_display = now >= self.display_hold_until or mode != "LIVE IMU"
                if (
                    self.display_enabled
                    and can_display
                    and (now - self.last_display >= 1.2 or mode != self.last_display_state)
                ):
                    self.last_display, self.last_display_state = now, mode
                    orientation = state["display_orientation"]
                    if orientation and orientation["gravity_valid"]:
                        text = (
                            f"{mode}\nR{orientation['roll']:+.0f} P{orientation['pitch']:+.0f}\n"
                            f"Tilt {orientation['tilt']:.0f} deg\nSeq {state['packet']['sample']}"
                        )
                    else:
                        text = f"{mode}\nNo valid attitude\nCheck IMU / WiFi"
                    scene = None
                    if state["display_view"] != "text":
                        motion = (
                            self.virtual_motion if now < self.virtual_motion_until else "stop"
                        )
                        text, scene = scene_command(state, state["display_view"], motion)
                    result = self.gateway.display(text)
                    with self.lock:
                        self.oled = {
                            **result,
                            "text": text,
                            "sample": state["packet"]["sample"] if state["packet"] else None,
                            "at": time.time(),
                            "visual_verified": False,
                            "scene": scene,
                        }
            except (OSError, RuntimeError, ValueError) as exc:
                with self.lock:
                    self.error = str(exc)
            self.stop.wait(0.12)

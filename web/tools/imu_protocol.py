"""Parse NU40-DK and NU87 IMU consoles without inventing measurements.

The firmware acquires at about 100 Hz and prints only about 10 samples/second.
``sampleHz`` is the acquisition rate reported by its ten-second health counter,
not the console line rate. Callers own USB connection state and synchronization.
"""

from collections import deque
from copy import deepcopy
import math
import re
import time


_ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
_CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")
_PROMPT = re.compile(r"^(?:(?:uart|usb|shell):~\$\s*)+")
_LOG = re.compile(r"^(?:\[\d+:\d+:\d+(?:[.,]\d+)+\]\s*)?<(inf|wrn|err|dbg)>\s+([\w.-]+):\s*(.*)$")
_HEX = r"0[xX][0-9a-fA-F]{2}"
_FLOAT = r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"
_VECTOR = rf"({_FLOAT}),({_FLOAT}),({_FLOAT})"
_SAMPLE = re.compile(rf"sample=(\d+)\s+accel_g={_VECTOR}\s+gyro_dps={_VECTOR}\s+corrected_dps={_VECTOR}")
_PROBE = re.compile(rf"probe addr=({_HEX}) WHO_AM_I=({_HEX}) profile=(\S+) init=(-?\d+) stage=(\S+)")
_PROBE_FAILED = re.compile(rf"probe addr=({_HEX}) WHO_AM_I=unavailable read_error=(-?\d+) stage=(\S+)")
_READY = re.compile(rf"IMU ready: profile=(\S+) addr=({_HEX}); sensor=\d+ Hz poll=\d+ Hz")
_STATUS = re.compile(rf"ready=([01]) address=({_HEX}) profile=(\S+) last_error=(-?\d+) stage=(\S+)")
_HEALTH = re.compile(r"health: ready=([01]) fresh=(\d+) delta_10s=(\d+) stale=(\d+) errors=(\d+) late=(\d+)")
_COUNTERS = re.compile(r"fresh=(\d+) stale=(\d+) errors=(\d+) late=(\d+) reconnects=(\d+) age_ms=(-?\d+)")
_CALIBRATION = re.compile(rf"calibrating=([01]) count=(\d+) bias_valid=([01]) bias_dps={_VECTOR} storage=(-?\d+)")


class ImuProtocol:
    """Bounded, JSON-ready state obtained from complete firmware log lines."""

    def __init__(self):
        self.sensor = {
            "ready": False,
            "address": None,
            "whoAmI": None,
            "profile": None,
            "lastError": None,
            "stage": None,
            "sda": None,
            "scl": None,
            "fresh": 0,
            "errors": 0,
            "stale": 0,
            "sampleHz": 0,
            "biasValid": False,
        }
        self.samples = deque(maxlen=300)
        self.logs = deque(maxlen=100)
        self.firmware = None

    def snapshot(self):
        """Return detached data so clients cannot alter parser history."""
        return deepcopy({
            "sensor": self.sensor,
            "samples": list(self.samples),
            "logs": list(self.logs),
            "firmware": self.firmware,
        })

    def feed(self, line: str, now_ms: int | None = None) -> bool:
        """Apply one line; return whether state or relevant log history changed.

        Overlong or malformed measurements are discarded in their entirety.
        Timestamps are host wall-clock time, never the MCU uptime prefix.
        """
        if not isinstance(line, str) or len(line) > 4096:
            return False
        line = _CONTROL.sub("", _ANSI.sub("", line)).strip()
        line = _PROMPT.sub("", line).strip()
        if not line:
            return False
        level = "info"
        envelope = _LOG.fullmatch(line)
        if envelope:
            code, module, line = envelope.groups()
            if module not in ("nu40_imu", "nu87_imu"):
                return False
            level = {"wrn": "warn", "err": "error"}.get(code, "info")
        timestamp = now_ms if isinstance(now_ms, int) and not isinstance(now_ms, bool) else time.time_ns() // 1_000_000
        sensor = self.sensor
        match = _SAMPLE.fullmatch(line)
        if match:
            numbers = [float(value) for value in match.groups()[1:]]
            if not all(math.isfinite(value) for value in numbers):
                return False
            seq = int(match[1])
            self.samples.append({
                "t": timestamp, "seq": seq,
                "accel": numbers[:3], "gyro": numbers[3:6],
                "corrected": numbers[6:9],
            })
            sensor.update(ready=True, fresh=seq, lastError=0, stage="sample-ready")
            return True
        # A corrupt measurement must never be downgraded into a useful event.
        if line.startswith("sample="):
            return False

        match = re.fullmatch(r"(?:NU40-DK|NU87) IMU (v[\w.-]+)(?: \(([^)]+)\)|: SDA=(?:P0\.26|PA26) SCL=(?:P0\.27|PA25) I2C=\d+ Hz)?", line)
        if match:
            if match[2] or self.firmware is None:
                self.firmware = line.split(": SDA=", 1)[0]
        elif re.fullmatch(r"BARAM-NU40 app=0x[0-9a-fA-F]+; SDA=P0\.26 SCL=P0\.27 I2C=\d+ Hz; USB CDC", line):
            pass  # Pins are configuration, not measured SDA/SCL logic levels.
        elif re.fullmatch(r"NU87 app=0x[0-9a-fA-F]+; SDA=PA26 SCL=PA25 I2C=\d+ Hz; UART=1500000 baud", line):
            pass
        elif (match := _PROBE_FAILED.fullmatch(line)):
            sensor.update(address=match[1].lower(), whoAmI=None, profile=None,
                          lastError=int(match[2]), stage=match[3], ready=False, sampleHz=0)
            level = "warn"
        elif (match := _PROBE.fullmatch(line)):
            error = int(match[4])
            sensor.update(address=match[1].lower(), whoAmI=match[2].lower(),
                          profile=match[3], lastError=error, stage=match[5], ready=error == 0)
            if error:
                sensor["sampleHz"] = 0
                level = "warn"
        elif (match := _READY.fullmatch(line)):
            sensor.update(profile=match[1], address=match[2].lower(), ready=True, lastError=0, stage="ready")
        elif (match := re.fullmatch(r"(?:I2C idle before probe:|bus_levels:) SDA=([01]) SCL=([01]) \((?:expected 1/1|idle should be 1/1)\)", line)):
            sensor.update(sda=int(match[1]), scl=int(match[2]))
            if match[1] != "1" or match[2] != "1":
                level = "warn"
        elif (match := _STATUS.fullmatch(line)):
            sensor.update(ready=match[1] == "1", address=match[2].lower(),
                          profile=match[3], lastError=int(match[4]), stage=match[5])
            if not sensor["ready"]:
                sensor["sampleHz"] = 0
        elif (match := re.fullmatch(rf"WHO_AM_I=({_HEX}|unavailable)(?: \(no successful ID read\))?", line)):
            sensor["whoAmI"] = None if match[1] == "unavailable" else match[1].lower()
        elif (match := _HEALTH.fullmatch(line)):
            sensor.update(ready=match[1] == "1", fresh=int(match[2]),
                          sampleHz=int(match[3]) / 10 if match[1] == "1" else 0,
                          stale=int(match[4]), errors=int(match[5]), late=int(match[6]))
        elif (match := _COUNTERS.fullmatch(line)):
            sensor.update(zip(("fresh", "stale", "errors", "late", "reconnects", "ageMs"),
                              map(int, match.groups())))
        elif (match := _CALIBRATION.fullmatch(line)):
            bias = [float(value) for value in match.groups()[3:6]]
            if not all(math.isfinite(value) for value in bias):
                return False
            sensor.update(calibrating=match[1] == "1", calibrationCount=int(match[2]),
                          biasValid=match[3] == "1", bias=bias, storageError=int(match[7]))
        elif (match := re.fullmatch(r"Calibration load result=(-?\d+) bias_valid=([01]) \(missing record is normal\)", line)):
            sensor["biasValid"] = match[2] == "1"
        elif (match := re.fullmatch(rf"Calibration accepted: count=(\d+) bias_dps={_VECTOR}; use imu save", line)):
            bias = [float(value) for value in match.groups()[1:]]
            if not all(math.isfinite(value) for value in bias):
                return False
            sensor.update(biasValid=True, calibrating=False, calibrationCount=int(match[1]), bias=bias)
        elif (match := re.fullmatch(r"Calibration clear result=(-?\d+)", line)):
            if int(match[1]) in (0, -2):  # Firmware treats a missing record as cleared.
                sensor.update(biasValid=False, calibrating=False, bias=[0.0, 0.0, 0.0])
        elif line == "Calibration started: keep sensor still for ~10 seconds":
            sensor["calibrating"] = True
        elif re.fullmatch(r"Calibration (?:cancelled: timeout or missing samples|rejected: motion/noise/range count=\d+)", line):
            sensor["calibrating"] = False
            level = "warn"
        elif (match := re.fullmatch(r"Sample failed rc=(-?\d+) stage=(\S+)", line)):
            sensor.update(lastError=int(match[1]), stage=match[2], calibrating=False)
            level = "error"
        elif line == "Sensor stalled; scheduling reinitialization":
            sensor.update(ready=False, sampleHz=0, calibrating=False)
            level = "warn"
        elif line == "I2C controller not ready; retrying in 2 seconds":
            sensor.update(ready=False, whoAmI=None, lastError=-19,
                          stage="i2c-controller-not-ready", sampleHz=0)
            level = "error"
        elif line == "No ready IMU; retrying in 2 seconds. Shell remains available.":
            sensor.update(ready=False, sampleHz=0)
            level = "warn"
        elif re.fullmatch(r"(?:Commands: imu status \| retry \| stream on/off \| calibrate \| save \| clear|Reconnected same profile/address; current RAM bias retained|Calibration save result=-?\d+|Sample stream (?:on|off)|Request queued|A command is already pending|Command rejected: IMU not ready; use imu status|Save rejected: first complete calibration)", line):
            if "rejected" in line or line == "A command is already pending":
                level = "warn"
        else:
            return False
        self.logs.append({"t": timestamp, "level": level, "text": line})
        return True

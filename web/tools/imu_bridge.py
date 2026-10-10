#!/usr/bin/env python3
"""Local-only NU40/NU87 USB serial -> JSON/SSE bridge. Requires pyserial 3.5."""
import argparse
import json
import math
import queue
import signal
import threading
import time
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

import serial
from serial.tools import list_ports
from imu_protocol import ImuProtocol

ALLOWED_COMMANDS = {"imu status", "imu retry", "imu stream on", "imu stream off"}
BRIDGE_HEADER = "nu40-local-v1"
BOARD_PROFILES = {
    "nu40": {
        "vid": 0x2FE3, "pid": 0x0004,
        "device": {"board": "nu40", "name": "NU40-DK", "mcu": "nRF52840",
                   "pins": {"sda": "P0.26", "scl": "P0.27"}, "baud": 115200},
    },
    "nu87": {
        "vid": 0x10C4, "pid": 0xEA60,
        "device": {"board": "nu87", "name": "NU-87", "mcu": "RTL8720DF",
                   "pins": {"sda": "PA26", "scl": "PA25"}, "baud": 1500000},
    },
}


def load_orientation_reference(path, board, serial_number):
    """Load an explicit stationary pose for exactly one NU87; never auto-zero."""
    if path is None:
        return None
    record = json.loads(Path(path).read_text(encoding="utf-8"))
    if (not isinstance(record, dict) or board != "nu87"
            or record.get("board") != board or record.get("serialNumber") != serial_number):
        raise ValueError("기준 자세 파일의 NU87 보드와 USB 식별번호가 현재 설정과 일치해야 합니다.")
    gravity = record.get("gravity")
    label = record.get("label")
    if (not isinstance(gravity, list) or len(gravity) != 3
            or any(isinstance(value, bool) or not isinstance(value, (int, float))
                   or not math.isfinite(value) for value in gravity)
            or not 0.5 <= math.hypot(*gravity) <= 1.5):
        raise ValueError("기준 자세 gravity는 g 단위의 유효한 정지 가속도 3축이어야 합니다.")
    if not isinstance(label, str) or not label.strip() or len(label) > 80:
        raise ValueError("기준 자세 label은 1~80자의 설명이어야 합니다.")
    reference = {"gravity": list(gravity), "label": label.strip()}
    if "flatGravity" in record:
        flat = record["flatGravity"]
        if (not isinstance(flat, list) or len(flat) != 3
                or any(isinstance(value, bool) or not isinstance(value, (int, float))
                       or not math.isfinite(value) for value in flat)
                or not 0.5 <= math.hypot(*flat) <= 1.5):
            raise ValueError("flatGravity는 바닥에 놓고 측정한 g 단위 가속도 3축이어야 합니다.")
        normal = [value / math.hypot(*flat) for value in flat]
        usb_pose = [value / math.hypot(*gravity) for value in gravity]
        projection = sum(value * axis for value, axis in zip(usb_pose, normal))
        if math.hypot(*(value - projection * axis for value, axis in zip(usb_pose, normal))) < .1:
            raise ValueError("바닥 자세와 USB-C 기준의 차이가 너무 작아 보드 축을 정할 수 없습니다.")
        reference["flatGravity"] = list(flat)
        reference["label"] = "바닥 기준 · USB-C 축 정렬"
    if "gyroBiasDps" in record or record.get("calibrationSource") == "host":
        bias = record.get("gyroBiasDps")
        if (record.get("calibrationSource") != "host" or not isinstance(bias, list) or len(bias) != 3
                or any(isinstance(value, bool) or not isinstance(value, (int, float))
                       or not math.isfinite(value) or abs(value) > 5 for value in bias)):
            raise ValueError("맥북 보정은 calibrationSource=host와 각 축 ±5°/s 이내의 gyroBiasDps 3축이 필요합니다.")
        reference["gyroBiasDps"] = list(bias)
    return reference


class Bridge:
    def __init__(self, serial_number=None, board="nu40", orientation_reference=None):
        if board not in BOARD_PROFILES:
            raise ValueError("지원하지 않는 보드입니다. nu40 또는 nu87을 선택하세요.")
        if board == "nu87" and (not isinstance(serial_number, str) or not serial_number.strip()):
            raise ValueError("NU87은 다른 CP2102 장치와 구분하도록 --serial-number를 지정해야 합니다.")
        self.profile = BOARD_PROFILES[board]
        self.device = deepcopy(self.profile["device"])
        self.serial_number = serial_number
        reference = load_orientation_reference(orientation_reference, board, serial_number)
        self.orientation_reference = {key: reference[key] for key in ("gravity", "label", "flatGravity") if key in reference} if reference else None
        self.calibration = {"source": "host", "label": "맥북 보정 적용", "gyroBiasDps": reference["gyroBiasDps"]} if reference and "gyroBiasDps" in reference else None
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.commands = queue.Queue(maxsize=8)
        self.protocol = ImuProtocol()
        self.connection = {"state": "connecting", "port": None, "firmware": None,
                           "message": f"{self.device['name']} USB 포트를 찾고 있습니다."}
        self.updated_at = int(time.time() * 1000)
        self.revision = 0

    def set_connection(self, state, port, message):
        with self.lock:
            self.connection.update(state=state, port=port, message=message)
            if state != "connected":
                self.protocol.sensor["ready"] = False
                self.protocol.sensor["sampleHz"] = 0
            self.updated_at = int(time.time() * 1000)
            self.revision += 1

    def feed(self, line):
        with self.lock:
            if self.protocol.feed(line):
                self.updated_at = int(time.time() * 1000)
                self.revision += 1

    def snapshot(self, after_revision=None):
        with self.lock:
            if after_revision == self.revision:
                return None
            result = self.protocol.snapshot()
            if self.calibration:
                bias = self.calibration["gyroBiasDps"]
                for sample in result["samples"]:
                    sample["corrected"] = [value - offset for value, offset in zip(sample["gyro"], bias)]
            connection = dict(self.connection)
            connection["firmware"] = result.pop("firmware")
            return {**result, "connection": connection, "device": deepcopy(self.device),
                    "orientationReference": deepcopy(self.orientation_reference), "calibration": deepcopy(self.calibration), "updatedAt": self.updated_at,
                    "revision": self.revision}

    def enqueue(self, command):
        if command not in ALLOWED_COMMANDS:
            raise ValueError("지원하지 않는 명령입니다.")
        if self.device["board"] == "nu87":
            raise ConnectionError("이 펌웨어에서는 보드 명령을 사용할 수 없습니다.")
        with self.lock:
            if self.connection["state"] != "connected":
                raise ConnectionError(f"{self.device['name']} USB 연결을 먼저 확인해 주세요.")
        self.commands.put_nowait(command)

    def find_port(self):
        ports = [p for p in list_ports.comports()
                 if p.vid == self.profile["vid"] and p.pid == self.profile["pid"]
                 and (not self.serial_number or p.serial_number == self.serial_number)]
        if len(ports) > 1:
            raise RuntimeError(f"일치하는 {self.device['name']} 포트가 여러 개입니다. USB 연결과 --serial-number를 확인하세요.")
        return ports[0].device if ports else None

    def run(self):
        while not self.stop.is_set():
            try:
                port = self.find_port()
                if not port:
                    self.set_connection("disconnected", None, f"{self.device['name']}를 USB로 연결해 주세요.")
                    self.stop.wait(1)
                    continue
                self.set_connection("connecting", port, f"{self.device['name']} USB 콘솔에 연결 중입니다.")
                with serial.Serial(port, self.device["baud"], timeout=0.1, write_timeout=1, exclusive=True) as device:
                    with self.lock:
                        self.protocol = ImuProtocol()
                    while not self.commands.empty():
                        self.commands.get_nowait()
                    self.set_connection("connected", port, "USB 연결됨 · 센서 응답을 확인합니다.")
                    supports_commands = self.device["board"] == "nu40"
                    if supports_commands:
                        device.write(b"\rimu stream on\rimu status\r")
                    pending = b""
                    next_status = time.monotonic() + 10
                    while not self.stop.is_set():
                        # Block for the first byte only; drain available bytes without
                        # waiting another 100 ms for a 4 KB buffer to fill.
                        data = device.read(max(1, min(device.in_waiting, 4096)))
                        if data:
                            pending += data.replace(b"\r", b"\n")
                            while b"\n" in pending:
                                line, pending = pending.split(b"\n", 1)
                                if line.strip():
                                    self.feed(line.decode("utf-8", "replace"))
                            pending = pending[-8192:]
                        try:
                            command = self.commands.get_nowait()
                        except queue.Empty:
                            command = None
                        if command:
                            device.write(("\r" + command + "\r").encode())
                        if supports_commands and time.monotonic() >= next_status:
                            device.write(b"\rimu status\r")
                            next_status = time.monotonic() + 10
            except (serial.SerialException, OSError, RuntimeError) as error:
                self.set_connection("error", None, f"USB 연결을 확인해 주세요: {error}")
                self.stop.wait(1)
        self.set_connection("disconnected", None, "연결 서비스가 종료됐습니다.")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def allowed(self):
        host = self.headers.get("Host", "").split(":")[0]
        return host in ("127.0.0.1", "localhost") and self.headers.get("X-IMU-Bridge") == BRIDGE_HEADER

    def send_json(self, value, status=200):
        data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self.allowed():
            return self.send_json({"error": "Local dashboard only"}, 403)
        path = urlsplit(self.path).path
        if path == "/status":
            return self.send_json(self.server.bridge.snapshot())
        if path != "/events":
            return self.send_json({"error": "Not found"}, 404)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        try:
            self.wfile.write(b"retry: 2000\n\n")
            previous_revision = None
            last_sent = time.monotonic()
            while not self.server.bridge.stop.is_set():
                snapshot = self.server.bridge.snapshot(after_revision=previous_revision)
                if snapshot is not None:
                    data = json.dumps(snapshot, ensure_ascii=False, allow_nan=False,
                                      separators=(",", ":"))
                    self.wfile.write(("data: " + data + "\n\n").encode())
                    self.wfile.flush()
                    previous_revision = snapshot["revision"]
                    last_sent = time.monotonic()
                elif time.monotonic() - last_sent >= 10:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    last_sent = time.monotonic()
                self.server.bridge.stop.wait(0.05)
        except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
            pass
        self.close_connection = True

    def do_POST(self):
        if not self.allowed():
            return self.send_json({"error": "Local dashboard only"}, 403)
        if urlsplit(self.path).path != "/command":
            return self.send_json({"error": "Not found"}, 404)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 256:
                return self.send_json({"error": "Invalid body length"}, 400)
            body = json.loads(self.rfile.read(length))
            self.server.bridge.enqueue(body.get("command"))
            self.send_json({"ok": True})
        except (ValueError, AttributeError, TypeError):
            self.send_json({"error": "지원하지 않는 명령입니다."}, 400)
        except ConnectionError as error:
            self.send_json({"error": str(error)}, 409)
        except queue.Full:
            self.send_json({"error": "이전 명령을 처리 중입니다."}, 429)

    def log_message(self, *_args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--board", choices=BOARD_PROFILES, default="nu40", help="보드 종류 (기본: nu40)")
    parser.add_argument("--serial-number", help="USB 식별번호; nu87에서는 필수")
    parser.add_argument("--orientation-reference", help="현재 NU87의 USB-C/바닥 기준 자세 JSON 파일")
    args = parser.parse_args()
    try:
        bridge = Bridge(args.serial_number, board=args.board, orientation_reference=args.orientation_reference)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    server.bridge = bridge
    worker = threading.Thread(target=bridge.run, daemon=True)
    worker.start()

    def stop(*_args):
        bridge.stop.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    print(f"{bridge.device['name']} serial bridge: http://127.0.0.1:{args.port} "
          f"({bridge.device['baud']} baud; local dashboard only)", flush=True)
    try:
        server.serve_forever()
    finally:
        bridge.stop.set()
        server.server_close()
        worker.join(timeout=2)


if __name__ == "__main__":
    main()

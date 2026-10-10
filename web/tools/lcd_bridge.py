#!/usr/bin/env python3
"""Local LCD controls and immutable firmware source for the NU87 Wi-Fi gateway."""
from __future__ import annotations
import argparse
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import threading
import time
from lcd_package import MAX_PACKAGE, validate_package

ASSETS = Path(__file__).with_name("lcd")
RESULTS = {1: "잘못된 화면 명령", 2: "보드가 다른 명령을 처리 중입니다", 3: "LCD I²C 통신 오류",
           4: "LCD 모델과 배선 설정이 필요합니다", 5: "검증한 부트로더의 OTA 진입 기능이 꺼져 있습니다"}
KIT_PROTOCOL = "nu40-kit-display"


def validate_kit_text(text: str) -> None:
    """Match ble_display_split_lines in the NU40-KIT-DISPLAY protocol."""
    if not text:
        raise ValueError("표시할 문자를 입력해 주세요. 화면을 비우려면 공백을 입력하세요.")
    row = column = 0
    for char in text:
        if char == "\n":
            row += 1
            column = 0
            continue
        if column == 21:
            row += 1
            column = 0
        if row >= 4:
            raise ValueError("이 화면은 자동 줄바꿈을 포함해 21자 × 4줄까지 표시합니다.")
        column += 1


def read_gateway_token(config_path: Path) -> str:
    """Use the firmware setup parser; keep credentials out of command arguments."""
    helper = Path(__file__).resolve().parents[2] / "firmware/nu87_lcd_gateway/configure.py"
    spec = importlib.util.spec_from_file_location("lcd_gateway_config", helper)
    if spec is None or spec.loader is None:
        raise ValueError("게이트웨이 설정 도구를 찾을 수 없습니다.")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.read_api_token(config_path)


class Controller:
    def __init__(self, gateway: str | None, token: str, port: int = 8080, *,
                 serve_firmware_lan: bool = False):
        self.gateway, self.token, self.port = gateway, token, port
        self.serve_firmware_lan = serve_firmware_lan
        self.lock = threading.Lock()
        self.images: dict[str, tuple[dict, bytes, float]] = {}
        self.sequence = 0

    def request(self, method: str, path: str, body: bytes | None = None) -> bytes:
        if not self.gateway:
            raise ValueError("NU87 주소가 설정되지 않았습니다. --gateway로 시작해 주세요.")
        connection = http.client.HTTPConnection(self.gateway, self.port, timeout=12)
        try:
            connection.request(method, path, body, {"Authorization": "Bearer " + self.token,
                                                   "Content-Type": "application/octet-stream"})
            response = connection.getresponse()
            content = response.read(4097)
            if len(content) > 4096:
                raise ValueError("게이트웨이 응답이 너무 큽니다.")
            if response.status not in (200, 202):
                raise ValueError(f"게이트웨이 오류 {response.status}: {content.decode(errors='replace')}")
            return content
        finally:
            connection.close()

    def command(self, opcode: int, payload: bytes = b"") -> dict:
        self.sequence = (self.sequence + 1) & 0xFFFF
        header = struct.pack("<BBH", 1, opcode, self.sequence)
        status = self.request("POST", "/v1/lcd", header + payload)
        if len(status) != 8 or status[:4] != header:
            raise ValueError("화면 명령과 응답 번호가 일치하지 않습니다.")
        if status[4]:
            raise ValueError(RESULTS.get(status[4], "알 수 없는 화면 오류"))
        return {"ready": bool(status[5] & 1), "dfuCapable": bool(status[5] & 2),
                "columns": status[6], "rows": status[7]}

    def status(self) -> dict:
        state = json.loads(self.request("GET", "/v1/status"))
        if not isinstance(state, dict):
            raise ValueError("게이트웨이 상태 응답이 올바르지 않습니다.")
        return state

    def require_legacy_feature(self, feature: str) -> None:
        state = self.status()
        capabilities = state.get("capabilities", {})
        if state.get("protocol") == KIT_PROTOCOL or (isinstance(capabilities, dict) and capabilities.get(feature) is False):
            label = "전원 제어" if feature == "power" else "펌웨어 OTA"
            raise ValueError(f"현재 NU40-KIT-DISPLAY 펌웨어는 {label}를 지원하지 않습니다.")

    def power(self, on: bool) -> dict:
        with self.lock:
            self.require_legacy_feature("power")
            return self.command(3, bytes([int(on)]))

    def cancel_ota(self) -> dict:
        with self.lock:
            self.require_legacy_feature("ota")
            self.request("POST", "/v1/ota/cancel", b"")
            return {"accepted": True}

    def peers(self) -> dict:
        result = json.loads(self.request("GET", "/v1/peers"))
        if not isinstance(result, dict) or not isinstance(result.get("peers"), list):
            raise ValueError("Bluetooth 검색 응답이 올바르지 않습니다.")
        return result

    def select_peer(self, address: str, address_type: str) -> dict:
        if not isinstance(address, str) or not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", address):
            raise ValueError("올바른 Bluetooth 주소를 선택해 주세요.")
        if address_type not in ("random", "public"):
            raise ValueError("Bluetooth 주소 유형을 확인해 주세요.")
        with self.lock:
            result = json.loads(self.request("POST", "/v1/peer", f"{address.upper()} {address_type}".encode("ascii")))
        if not isinstance(result, dict):
            raise ValueError("Bluetooth 연결 요청 응답이 올바르지 않습니다.")
        return result

    def display(self, text: str) -> dict:
        if not isinstance(text, str) or len(text) > 84 or any(c != "\n" and not 32 <= ord(c) <= 126 for c in text):
            raise ValueError("영문·숫자·기호만 입력할 수 있습니다. 한글 폰트는 지원하지 않습니다.")
        with self.lock:
            gateway_state = self.status()
            if gateway_state.get("protocol") == KIT_PROTOCOL:
                validate_kit_text(text)
                if not gateway_state.get("connected") or not gateway_state.get("lcdReady"):
                    raise ValueError("NU40 Bluetooth 연결과 화면 제어 준비 상태를 확인해 주세요.")
                state = json.loads(self.request("POST", "/v1/display", text.encode("ascii")))
                if not isinstance(state, dict) or state.get("ready") is not True or state.get("columns") != 21 or state.get("rows") != 4:
                    raise ValueError("NU40 화면 반영 응답을 확인하지 못했습니다.")
                return state
            state = self.command(0)
            columns, rows = state["columns"], state["rows"]
            if not state["ready"] or not 1 <= columns <= 20 or not 1 <= rows <= 4:
                raise ValueError("LCD가 준비되지 않았습니다. 모델·배선·전원을 확인해 주세요.")
            lines = text.split("\n")
            if len(lines) > rows or any(len(line) > columns for line in lines):
                raise ValueError(f"이 화면은 {columns}자 × {rows}줄까지 표시합니다.")
            # Write every cell including spaces: no flickering clear and no stale
            # trailing characters. A later error reports failure, never success.
            for row in range(rows):
                line = (lines[row] if row < len(lines) else "").ljust(columns)
                for col in range(0, columns, 14):
                    self.command(2, bytes((row, col)) + line[col:col + 14].encode("ascii"))
            return state

    def stage(self, package: bytes) -> dict:
        manifest, image = validate_package(package)
        with self.lock:
            now = time.monotonic()
            self.images = {k: v for k, v in self.images.items() if now - v[2] < 3600}
            if len(self.images) >= 4 and manifest["sha256"] not in self.images:
                raise ValueError("준비된 파일이 4개입니다. 사용하지 않는 브리지를 재시작해 주세요.")
            self.images[manifest["sha256"]] = (manifest, image, now)
        return manifest

    def start_ota(self, image_id: str, source_port: int) -> dict:
        # LCD control only needs outbound connections. OTA additionally needs
        # the NU87 to fetch this Mac's immutable image over the LAN, which is
        # disabled unless the operator explicitly enables its listener.
        if not self.serve_firmware_lan:
            raise ValueError("펌웨어 LAN 제공이 꺼져 있습니다. OTA를 사용하려면 --serve-firmware-lan 옵션으로 브리지를 다시 시작해 주세요.")
        with self.lock:
            if image_id not in self.images:
                raise ValueError("먼저 업데이트 파일을 검사해 주세요.")
            manifest, image, staged_time = self.images[image_id]
            now = time.monotonic()
            if now - staged_time >= 3600:
                raise ValueError("파일 준비 시간이 만료됐습니다. 파일을 다시 선택해 주세요.")
            # The KIT app uses a different bootloader layout. Never send the
            # legacy LCD/DFU protocol to it, even through direct API requests.
            self.require_legacy_feature("ota")
            # Pin an immutable source for another hour before entering DFU;
            # the gateway's entire session is bounded to 30 minutes.
            self.images[image_id] = (manifest, image, now)
            state = self.command(0)
            if not state["dfuCapable"]:
                raise ValueError("현재 NU40 앱에 OTA 진입 기능이 없습니다. 첫 설치는 USB 또는 별도 BLE DFU 연결이 필요합니다.")
            descriptor = struct.pack("<4sIIHH", b"LCD1", manifest["size"], manifest["version"],
                                     manifest["crc16"], source_port) + image_id.encode("ascii")
            self.request("POST", "/v1/ota", descriptor)
            return {"accepted": True}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    @property
    def controller(self) -> Controller:
        return self.server.controller

    def log_message(self, format, *args):
        # No tokens, firmware identifiers or user display text in request logs.
        pass

    def send(self, status: int, content: bytes, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Connection", "close")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; connect-src 'self'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(content)
        self.close_connection = True

    def json(self, status: int, value):
        self.send(status, json.dumps(value, ensure_ascii=False).encode())

    def local_request(self, mutation=False) -> bool:
        port = self.server.server_port
        hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if not ipaddress.ip_address(self.client_address[0]).is_loopback or self.headers.get("Host") not in hosts:
            self.json(403, {"error": "제어 화면은 이 Mac에서만 열 수 있습니다."}); return False
        origin = self.headers.get("Origin")
        if origin and origin not in {"http://" + host for host in hosts}:
            self.json(403, {"error": "다른 사이트에서 보낸 요청입니다."}); return False
        if mutation and self.headers.get("X-LCD-Client") != "1":
            self.json(403, {"error": "제어 화면에서 요청해 주세요."}); return False
        return True

    def do_GET(self):
        if self.path.startswith("/firmware/"):
            return self.firmware()
        if not self.local_request(): return
        try:
            if self.path == "/api/status":
                if not self.controller.gateway:
                    return self.json(200, {"configured": False, "connected": False, "preview": True})
                return self.json(200, self.controller.status())
            if self.path == "/api/peers":
                return self.json(200, self.controller.peers())
            assets = {"/": ("index.html", "text/html; charset=utf-8"),
                      "/style.css": ("style.css", "text/css"), "/app.js": ("app.js", "text/javascript")}
            if self.path not in assets: return self.json(404, {"error": "Not found"})
            name, kind = assets[self.path]
            return self.send(200, (ASSETS / name).read_bytes(), kind)
        except (OSError, ValueError, http.client.HTTPException) as error:
            return self.json(502, {"error": str(error)})

    def firmware(self):
        # This single read-only route is available to the configured gateway.
        if not self.controller.serve_firmware_lan:
            return self.json(403, {"error": "Firmware LAN serving is disabled"})
        if self.client_address[0] != self.controller.gateway:
            return self.json(403, {"error": "Gateway only"})
        match = re.fullmatch(r"/firmware/([a-f0-9]{64})\.bin", self.path)
        entry = self.controller.images.get(match[1]) if match else None
        if not entry or time.monotonic() - entry[2] >= 3600:
            return self.json(404, {"error": "Image expired"})
        image = entry[1]
        byte_range = re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers.get("Range", ""))
        if not byte_range: return self.json(416, {"error": "Range required"})
        start, end = map(int, byte_range.groups())
        if not 0 <= start <= end < len(image) or end - start >= 4096:
            return self.json(416, {"error": "Invalid range"})
        # Exact finite framing consumed by the small Zephyr source client.
        self.wfile.write((f"HTTP/1.1 206 Partial Content\r\nContent-Length: {end-start+1}\r\n"
                          f"Content-Range: bytes {start}-{end}/{len(image)}\r\nConnection: close\r\n\r\n").encode())
        self.wfile.write(image[start:end+1]); self.close_connection = True

    def do_POST(self):
        if not self.local_request(mutation=True): return
        try:
            lengths = self.headers.get_all("Content-Length", [])
            if len(lengths) != 1 or not re.fullmatch(r"\d+", lengths[0]) or self.headers.get("Transfer-Encoding"):
                raise ValueError("Content-Length가 필요합니다.")
            length = int(lengths[0])
            maximum = MAX_PACKAGE if self.path == "/api/package" else 1024
            if not 0 <= length <= maximum: raise ValueError("요청이 너무 큽니다.")
            data = self.rfile.read(length)
            if len(data) != length: raise ValueError("전송이 중단됐습니다.")
            if self.path == "/api/package":
                return self.json(200, self.controller.stage(data))
            body = json.loads(data)
            if not isinstance(body, dict): raise ValueError("객체가 필요합니다.")
            if self.path == "/api/display":
                return self.json(200, self.controller.display(body.get("text")))
            if self.path == "/api/peer":
                return self.json(202, self.controller.select_peer(body.get("address"), body.get("type")))
            if self.path == "/api/power":
                if type(body.get("on")) is not bool: raise ValueError("on은 참/거짓이어야 합니다.")
                return self.json(200, self.controller.power(body["on"]))
            if self.path == "/api/ota":
                image_id = body.get("id")
                if not isinstance(image_id, str): raise ValueError("파일 ID가 필요합니다.")
                return self.json(202, self.controller.start_ota(image_id, self.server.server_port))
            if self.path == "/api/cancel":
                return self.json(202, self.controller.cancel_ota())
            self.json(404, {"error": "Not found"})
        except (OSError, ValueError, http.client.HTTPException) as error:
            self.json(422, {"error": str(error)})


def main(argv: list[str] | None = None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gateway", help="NU87 isolated LAN IPv4 address; omit for UI preview")
    parser.add_argument("--gateway-port", type=int, default=8080)
    parser.add_argument("--gateway-config", type=Path,
                        help="Read API token from private gateway local.conf instead of environment")
    parser.add_argument("--serve-firmware-lan", action="store_true",
                        help="Explicitly enable LAN firmware downloads for OTA; LCD controls stay restricted to this Mac")
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args(argv)
    if args.serve_firmware_lan and not args.gateway:
        parser.error("--serve-firmware-lan은 --gateway 주소가 필요합니다.")
    token = os.environ.get("NU87_GATEWAY_TOKEN", "")
    if args.gateway_config:
        try:
            token = read_gateway_token(args.gateway_config)
        except (OSError, ValueError):
            parser.error("비공개 게이트웨이 설정에서 유효한 API 토큰을 읽을 수 없습니다.")
    if args.gateway:
        ip = ipaddress.IPv4Address(args.gateway)
        if not ip.is_private or ip.is_loopback or ip.is_multicast or ip.is_unspecified:
            parser.error("--gateway는 테스트 LAN의 사설 IPv4 주소여야 합니다.")
        if not 16 <= len(token) <= 128 or not token.isascii() or any(ord(c) < 33 or ord(c) > 126 for c in token):
            parser.error("NU87_GATEWAY_TOKEN 환경변수에 16~128자 토큰을 설정해 주세요.")
    if not 1024 <= args.port <= 65535: parser.error("--port 범위는 1024~65535입니다.")
    # Configuring an outbound gateway never implicitly exposes a LAN listener.
    bind_host = "0.0.0.0" if args.serve_firmware_lan else "127.0.0.1"
    server = ThreadingHTTPServer((bind_host, args.port), Handler)
    server.controller = Controller(args.gateway, token, args.gateway_port,
                                   serve_firmware_lan=args.serve_firmware_lan)
    print(f"LCD control: http://127.0.0.1:{args.port} ({'gateway configured' if args.gateway else 'preview, no hardware commands'})", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()

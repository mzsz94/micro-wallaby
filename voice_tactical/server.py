"""Loopback-only voice lab server. Run: python3 -m voice_tactical.server."""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
from pathlib import Path
import threading
from urllib.parse import parse_qs, unquote, urlsplit
import webbrowser

from .audio import LocalTranscriber
from .engine import Engine
from .gateway import Gateway, summarize_result
from .device_audio import DeviceAudio

ROOT = Path(__file__).resolve().parent
MODES = {"auto", "query", "report", "orders", "search"}
MAX_AUDIO_BYTES = 16_000 * 2 * 30 + 4096


class RequestError(Exception):
    def __init__(self, status: int, message: str):
        self.status, self.message = status, message


class VoiceApp:
    def __init__(self, engine, transcriber, gateway, *, auto_display=False):
        self.engine = engine
        self.transcriber = transcriber
        self.gateway = gateway
        self.auto_display = auto_display
        # A single turn at a time keeps CPU inference bounded and reports ordered.
        self.turn_lock = threading.Lock()
        self.device_audio = DeviceAudio(self)
        self._alert_lock = threading.RLock()
        self._alarm_poll_lock = threading.Lock()
        self._alarm_epoch = 0
        self._alert_active = bool(getattr(engine, "alert_active", False))
        self._alarm_last_key = None
        self._alarm_pending = None
        self._alarm_result = None
        self._alarm_device = {"available": False, "acknowledged": False}
        self._command_generation = 0
        self._command_lock = threading.Lock()

    @property
    def alert_active(self):
        with self._alert_lock:
            return self._alert_active or bool(getattr(self.engine, "alert_active", False))

    def _alarm_write(self, active):
        try:
            result = self.gateway.alarm(active)
            return result if isinstance(result, dict) else {"ok": False, "acknowledged": False, "error": "올바른 경보 응답이 없습니다."}
        except (RuntimeError, ValueError):
            return {"ok": False, "acknowledged": False, "error": "NU40 경보 출력 응답을 확인할 수 없습니다."}

    def _robot_write(self, command):
        if not self.gateway.configured:
            return {"configured": False, "accepted": False, "mode": "local-simulation"}
        try:
            return self.gateway.robot_command(command)
        except (RuntimeError, ValueError):
            return {"ok": False, "accepted": False, "error": "NU87 로봇 명령 응답을 확인할 수 없습니다."}

    def stop_motion(self):
        # Invalidate inference before waiting for an earlier network operation.
        with self._alert_lock:
            self._command_generation += 1
            generation = self._command_generation
        with self._command_lock:
            robot = self._robot_write("stop")
        return {"stopped": True, "generation": generation, "robot": robot}

    def _apply_alert(self, result):
        # Serialize host clear against reconnect reassertion, so a stale poll
        # cannot re-enable the speaker after a confirmed clear request.
        with self._alarm_poll_lock:
            requested = result["alert"]["active"]
            with self._alert_lock:
                self._alarm_epoch += 1
                self._alarm_pending = None
                self._alarm_result = None
                self._alert_active = requested or self.gateway.configured
            device = self._alarm_write(requested)
            clear_confirmed = device.get("acknowledged") is True and device.get("active") is False
            if not requested and self.gateway.configured and not clear_confirmed:
                result = self.engine.record_device_alert(True, device.get("eventCounter"), source="clear-unconfirmed")
                with self._alert_lock:
                    self._alert_active = True
                result["alert"]["clear_requested"] = True
                result["answer"] = "NU40 경보 해제 응답을 확인하지 못해 이동 차단을 유지합니다. 연결을 확인한 뒤 다시 해제하세요."
            elif not requested:
                with self._alert_lock:
                    self._alert_active = False
            result["device"] = device
            with self._alert_lock:
                self._alarm_device = device
                self._alarm_result = dict(result)
                self._alarm_epoch += 1
            return result

    def alerts(self):
        """Observe physical BTN4 even when microphone capture is disabled.

        Physical state latches immediately, including while model inference owns
        turn_lock. Device event recording takes the engine's short database lock
        and never invokes the local LLM.
        """
        if self._alarm_poll_lock.acquire(blocking=False):
            try:
                with self._alert_lock:
                    epoch = self._alarm_epoch
                if self.gateway.configured:
                    try:
                        device = self.gateway.alarm_status()
                    except (RuntimeError, ValueError):
                        device = {"available": False, "acknowledged": False, "error": "NU40 경보 상태를 확인할 수 없습니다."}
                else:
                    device = {"available": False, "acknowledged": False, "error": "게이트웨이가 설정되지 않아 로컬 시뮬레이션만 사용합니다."}
                with self._alert_lock:
                    if epoch == self._alarm_epoch:
                        self._alarm_device = device
                        if device.get("available") and device.get("active") is True:
                            # Source can become "command" after an OLED/alarm
                            # ACK; the button event counter remains authoritative.
                            key = (device.get("eventCounter"),)
                            if device.get("eventCounter", 0) > 0 and key != self._alarm_last_key:
                                self._alert_active = True
                                self._alarm_pending = key
                                self._alarm_result = None
                            elif not self.alert_active:
                                self._alert_active = True
                                self._alarm_pending = key
                                self._alarm_result = None
                with self._alert_lock:
                    key = self._alarm_pending
                    if key is not None:
                        result = self.engine.record_device_alert(True, key[0], source="nu40-btn4" if key[0] else "nu40-state")
                        result["device"] = self._alarm_device
                        self._alarm_result = result
                        self._alarm_last_key = key
                        self._alarm_pending = None
                # A rebooted control tower must not silently clear a persisted
                # host warning. Reassert the speaker latch until explicit clear.
                if device.get("available") and device.get("active") is False and self.alert_active:
                    with self._alert_lock:
                        reassert = epoch == self._alarm_epoch
                    if reassert:
                        confirmed = self._alarm_write(True)
                        with self._alert_lock:
                            if epoch == self._alarm_epoch:
                                self._alarm_device = confirmed
            finally:
                self._alarm_poll_lock.release()
        with self._alert_lock:
            return {"active": self.alert_active, "last_result": self._alarm_result,
                    "device": self._alarm_device, "pending_record": self._alarm_pending is not None}

    def status(self):
        engine = self.engine.status()
        return {
            "stt": self.transcriber.status(),
            "engine": engine,
            "llm": engine.get("llm", {"enabled": engine.get("llm_configured", False)}),
            "gateway": self.gateway.status(),
            "alert_active": self.alert_active,
            "capabilities": {
                "inference": "host-pc-local", "microphone": "nu40-pdm",
                "motion": "simulation", "nu40_microphone_stream": True,
                "kit_ota": False, "voice_firmware_ota": "mcuboot-smp", "auto_display": self.auto_display,
                "audio": {"sample_rate": 16000, "channels": 1, "max_seconds": 30},
            },
        }

    def turn(self, *, text=None, audio=None, mode="auto"):
        if not isinstance(mode, str) or mode not in MODES:
            raise RequestError(400, "지원하지 않는 처리 모드입니다.")
        # These exact UI/voice emergency phrases are already deterministic
        # engine paths and must not wait behind a lengthy LLM inference.
        emergency = audio is None and mode == "auto" and isinstance(text, str) and text.strip() in {
            "왈라비 정지", "정지", "왈라비 폭발물 감지", "왈라비 경고 해제",
        }
        owns_turn = self.turn_lock.acquire(blocking=False)
        if not owns_turn and not emergency:
            raise RequestError(409, "이전 발화를 처리 중입니다. 완료 후 다시 말해 주세요.")
        with self._alert_lock:
            if emergency:
                self._command_generation += 1
            command_generation = self._command_generation
        try:
            if audio is not None:
                text = self.transcriber.transcribe(audio)
                if not text.strip():
                    raise RequestError(422, "인식된 음성이 없습니다. 다시 말해 주세요.")
            if not isinstance(text, str) or not text.strip() or len(text) > 8000:
                raise RequestError(400, "발화는 1~8000자의 문자열이어야 합니다.")
            result = self.engine.process(text, mode=mode)
            if result.get("kind") == "alert":
                result = self._apply_alert(result)
            if self.alert_active and result.get("motion") not in (None, "stop"):
                result["motion"] = "stop"
                result["answer"] = "폭발물 감지 훈련 경고가 활성화되어 이동을 차단했습니다. 경고를 명시적으로 해제하세요."
            if result.get("motion"):
                with self._command_lock:
                    with self._alert_lock:
                        cancelled = command_generation != self._command_generation
                    if cancelled or (self.alert_active and result["motion"] != "stop"):
                        result["motion"] = "stop"
                        result["motion_cancelled"] = True
                    robot = self._robot_write(result["motion"])
                    result["robot"] = robot
                    if self.gateway.configured and robot.get("accepted") is not True:
                        result["motion"] = "stop"
                        result["answer"] += "\nNU87 명령 수락을 확인하지 못해 시뮬레이션을 정지합니다."
            # A physical BTN4 event can arrive while robot acknowledgement is
            # in flight. Re-check immediately before handing motion to the UI.
            if self.alert_active and result.get("motion") not in (None, "stop"):
                result["motion"] = "stop"
            result["alert_active"] = self.alert_active
            if self.auto_display and result.get("kind") != "alert":
                # Display failure must not discard the local report or block motion forever.
                result["device"] = self.gateway.display(summarize_result(result))
            return result
        finally:
            if owns_turn:
                self.turn_lock.release()


class VoiceHandler(BaseHTTPRequestHandler):
    server_version = "MilitaryWallaby/1.0"
    app: VoiceApp
    web_root = ROOT / "web"
    simulation_root = ROOT.parent / "simulation"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, format, *args):
        # Paths can contain a transcript query; never write request bodies/queries to logs.
        pass

    def _check_local(self):
        port = self.server.server_address[1]
        allowed = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if self.headers.get("Host") not in allowed:
            raise RequestError(403, "localhost에서 접속해 주세요.")
        origins = self.headers.get_all("Origin", [])
        if len(origins) > 1 or (origins and origins[0] not in {f"http://{h}" for h in allowed}):
            raise RequestError(403, "다른 사이트에서 보낸 요청은 허용하지 않습니다.")
        if self.headers.get("Sec-Fetch-Site") == "cross-site":
            raise RequestError(403, "다른 사이트에서 보낸 요청은 허용하지 않습니다.")

    def _send(self, status, body, content_type="application/json; charset=utf-8"):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-src 'self'; frame-ancestors 'self'; object-src 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(body)

    def _body(self, limit):
        if self.headers.get("Transfer-Encoding"):
            raise RequestError(400, "chunked 요청은 지원하지 않습니다.")
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isdigit():
            raise RequestError(411, "Content-Length가 필요합니다.")
        size = int(lengths[0])
        if not 0 < size <= limit:
            raise RequestError(413, "입력 파일 또는 발화가 너무 큽니다.")
        data = self.rfile.read(size)
        if len(data) != size:
            raise RequestError(400, "요청 본문이 완성되지 않았습니다.")
        return data

    def _json_body(self):
        if self.headers.get_content_type() != "application/json":
            raise RequestError(415, "application/json 형식이 필요합니다.")
        try:
            value = json.loads(self._body(40_000))
        except (ValueError, UnicodeError) as error:
            raise RequestError(400, "JSON 형식을 확인해 주세요.") from error
        if not isinstance(value, dict):
            raise RequestError(400, "JSON 객체가 필요합니다.")
        return value

    def _dispatch(self, method):
        self._check_local()
        url = urlsplit(self.path)
        path, query = unquote(url.path), parse_qs(url.query)
        if method == "GET":
            if path == "/api/status":
                return self._send(200, self.app.status())
            if path == "/api/device-audio/status":
                lease = query.get("generation", [""])[0]
                if lease and (not lease.isascii() or not lease.isdecimal() or len(lease) > 20):
                    raise RequestError(400, "올바른 마이크 세션 번호가 필요합니다.")
                return self._send(200, self.app.device_audio.status(touch=bool(lease), generation=int(lease) if lease else None))
            if path == "/api/alerts":
                return self._send(200, self.app.alerts())
            if path == "/api/records":
                q = query.get("q", [""])[0]
                if len(q) > 1000:
                    raise RequestError(400, "검색어가 너무 깁니다.")
                records = self.app.engine.search(q) if q else self.app.engine.history()
                return self._send(200, {"records": records})
            if path == "/api/gateway/status":
                return self._send(200, self.app.gateway.status())
            if path == "/api/gateway/peers":
                return self._send(200, self.app.gateway.peers())
            if path.startswith("/api/"):
                raise RequestError(404, "API를 찾을 수 없습니다.")
            return self._asset(path)
        if path == "/api/audio":
            if self.headers.get_content_type() not in {"audio/wav", "audio/x-wav"}:
                raise RequestError(415, "16kHz mono PCM16 WAV 형식이 필요합니다.")
            result = self.app.turn(audio=self._body(MAX_AUDIO_BYTES), mode=query.get("mode", ["auto"])[0])
            return self._send(200, result)
        if path == "/api/turn":
            body = self._json_body()
            return self._send(200, self.app.turn(text=body.get("text"), mode=body.get("mode", "auto")))
        if path == "/api/control/stop":
            self._json_body()
            return self._send(200, self.app.stop_motion())
        if path == "/api/device-audio/start":
            body = self._json_body()
            return self._send(200, self.app.device_audio.start(body.get("mode", "auto")))
        if path == "/api/device-audio/stop":
            self._json_body()
            return self._send(200, self.app.device_audio.stop())
        if path == "/api/device-audio/gate":
            body = self._json_body()
            return self._send(200, self.app.device_audio.gate(body.get("muted")))
        if path == "/api/gateway/display":
            body = self._json_body()
            text = body.get("text")
            if not isinstance(text, str):
                raise RequestError(400, "화면 문자열이 필요합니다.")
            return self._send(200, self.app.gateway.display(text))
        if path == "/api/gateway/peer":
            body = self._json_body()
            return self._send(200, self.app.gateway.select_peer(body.get("address"), body.get("type")))
        raise RequestError(404, "API를 찾을 수 없습니다.")

    def _asset(self, path):
        if path.startswith("/simulation/"):
            root, relative = self.simulation_root, path[len("/simulation/"):]
        else:
            root = self.web_root
            relative = path[len("/web/"):] if path.startswith("/web/") else path.lstrip("/")
        relative = relative or "index.html"
        file = (root / relative).resolve()
        # No model/database/source-code exposure, directory listings, or symlink escape.
        if not file.is_relative_to(root.resolve()) or file.suffix not in {".html", ".css", ".js", ".mjs", ".svg", ".png", ".ico"} or not file.is_file():
            raise RequestError(404, "파일을 찾을 수 없습니다.")
        mime = "text/javascript" if file.suffix == ".mjs" else mimetypes.guess_type(file.name)[0] or "application/octet-stream"
        return self._send(200, file.read_bytes(), mime)

    def _handle(self, method):
        try:
            self._dispatch(method)
        except RequestError as error:
            self._send(error.status, {"error": error.message})
        except ValueError as error:
            self._send(400, {"error": str(error)})
        except RuntimeError:
            self._send(503, {"error": "로컬 모델 또는 장치를 사용할 수 없습니다. 설정과 상태를 확인해 주세요."})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        except Exception:
            self._send(500, {"error": "처리하지 못했습니다. 로컬 모델과 저장 폴더 상태를 확인해 주세요."})

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")


def create_server(app, port=8767):
    handler = type("ConfiguredVoiceHandler", (VoiceHandler,), {"app": app})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.daemon_threads = True
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--data-dir", type=Path, default=ROOT / "data")
    parser.add_argument("--knowledge-dir", type=Path)
    parser.add_argument("--stt-model", type=Path, default=ROOT / "models" / "whisper-small", help="Local faster-whisper/CTranslate2 model directory")
    parser.add_argument("--llm-url", help="Loopback OpenAI-compatible local server base URL")
    parser.add_argument("--llm-model", help="Already-loaded local model name")
    parser.add_argument("--llm-structured", action="store_true", help="Use JSON schema with a compatible local LLM server")
    parser.add_argument("--gateway", help="NU87 private IPv4 address[:port]")
    parser.add_argument("--gateway-config", type=Path, help="Private local.conf; token is never sent to the browser")
    parser.add_argument("--auto-display", action="store_true", help="Send ASCII processing summaries to the selected NU40")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    try:
        app = VoiceApp(
            Engine(args.data_dir, knowledge_dir=args.knowledge_dir, llm_url=args.llm_url,
                   llm_model=args.llm_model, llm_structured=args.llm_structured),
            LocalTranscriber(args.stt_model),
            Gateway(host=args.gateway, config_path=args.gateway_config),
            auto_display=args.auto_display,
        )
        server = create_server(app, args.port)
    except (OSError, ValueError) as error:
        parser.exit(1, f"서버를 시작할 수 없습니다: {error}\n")
    url = f"http://127.0.0.1:{args.port}/"
    print(f"Military Wallaby: {url}\n입력: NU40 PDM · 추론: HostPC 로컬 · 종료: Ctrl+C", flush=True)
    if not args.no_browser:
        threading.Timer(0.3, webbrowser.open, args=(url,)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if app.device_audio.status(touch=False)["running"]:
            app.device_audio.stop()
        server.server_close()


if __name__ == "__main__":
    main()

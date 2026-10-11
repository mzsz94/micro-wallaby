"""Small, optional NU87 adapter for the existing NU40 KIT display protocol.

The gateway firmware owns BLE sequence matching. A successful display response
means the gateway received the matching OLED-driver ACK, not that a person saw
the pixels. This module does not expose the incompatible legacy OTA protocol.
"""

from __future__ import annotations

import http.client
import importlib.util
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import select
import threading
import time

KIT_PROTOCOL = "nu40-kit-display"
_ADDRESS = re.compile(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\Z")
_NETWORKS = tuple(ipaddress.ip_network(value) for value in (
    "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "127.0.0.0/8", "::1/128", "fc00::/7",
))
_LIMITATIONS = {
    "ota": False,
    "microphone_stream": False,
    "power": False,
    "note": "Baseline KIT firmware needs the optional audio/SMP upgrade; check audio and SMP availability.",
}


class _GatewayError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def _endpoint(value: str) -> tuple[str, int]:
    """Accept literal LAN/loopback IPs only; never resolve an external name."""
    if not isinstance(value, str) or value != value.strip() or "%" in value:
        raise ValueError("Gateway host must be a private or loopback numeric IP, optionally with a port.")
    host, port = value, 8080
    if value.startswith("["):
        match = re.fullmatch(r"\[([^]]+)\](?::([0-9]+))?", value)
        if not match:
            raise ValueError("Invalid gateway IP or port.")
        host, port = match[1], int(match[2] or 8080)
    elif value.count(":") == 1:
        host, port_text = value.split(":")
        if not re.fullmatch(r"[0-9]{1,5}", port_text):
            raise ValueError("Invalid gateway port.")
        port = int(port_text)
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        raise ValueError("Gateway host must be a private or loopback numeric IP.") from None
    if not any(address.version == network.version and address in network for network in _NETWORKS):
        raise ValueError("Gateway host must be a private or loopback numeric IP.")
    if not 1 <= port <= 65535:
        raise ValueError("Invalid gateway port.")
    return str(address), port


def _read_token(config_path: str | Path) -> str:
    # Reuse the existing Kconfig parser: escaped quotes, backslashes, duplicate
    # settings and '# ... is not set' have exactly the firmware tool's meaning.
    helper = Path(__file__).resolve().parents[1] / "firmware/nu87_lcd_gateway/configure.py"
    spec = importlib.util.spec_from_file_location("voice_gateway_config", helper)
    if spec is None or spec.loader is None:
        raise ValueError("Gateway configuration parser is unavailable.")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        return module.read_api_token(Path(config_path))
    except (OSError, ValueError):
        # Do not propagate paths, config content, response bodies or credentials.
        raise ValueError("Cannot read a valid gateway token from the local configuration.") from None


def _validate_text(text: str) -> None:
    if not isinstance(text, str) or not 1 <= len(text) <= 84:
        raise _GatewayError("invalid_text", "Display text must contain 1 to 84 ASCII bytes.")
    row = column = 0
    for char in text:
        if char == "\n":
            row += 1
            column = 0
            continue
        if not 32 <= ord(char) <= 126:
            raise _GatewayError("invalid_text", "KIT display supports printable ASCII and newlines only.")
        if column == 21:
            row += 1
            column = 0
        if row >= 4:
            raise _GatewayError("invalid_text", "Display text exceeds 21 columns by 4 rows.")
        column += 1


def summarize_result(result: dict) -> str:
    """Return an ASCII status card, never attempt to render Korean glyphs."""
    kind = result.get("kind", "result")
    labels = {"query": "MANUAL ANSWER", "report": "REPORT SAVED", "orders": "ORDERS ORGANIZED",
              "motion": "VOICE COMMAND", "search": "RECORD SEARCH", "error": "PLEASE RETRY"}
    lines = ["WALLABY VOICE", labels.get(kind, "VOICE RESULT")]
    motion = result.get("motion")
    if isinstance(motion, dict):
        motion = motion.get("id", motion.get("name"))
    if isinstance(motion, str):
        ascii_motion = re.sub(r"[^A-Za-z0-9 _-]", "", motion).strip()
        if ascii_motion:
            lines.append("SIM: " + ascii_motion[:16].upper())
    lines.append("LOCAL / OFFLINE")
    # Three newlines count toward the 84-byte wire limit too.
    return "\n".join(line[:20] for line in lines[:4])


class Gateway:
    """Optional LAN gateway. Runtime failures are returned as nonfatal results.

    ``configured`` describes this adapter's host and credential configuration;
    ``peer_configured`` describes the firmware's selected BLE peer. ``available``
    means a valid HTTP status was received, while ``ready`` requires a connected
    KIT peer and a subscribed display service. No method retries a display write.
    """

    def __init__(self, host: str | None = None, token: str | None = None,
                 config_path: str | Path | None = None, *, timeout: float = 5.0):
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 < timeout <= 30:
            raise ValueError("Gateway timeout must be between 0 and 30 seconds.")
        self._host, self._port = _endpoint(host) if host else (None, 8080)
        if token is None:
            token = os.environ.get("NU87_GATEWAY_TOKEN")
        if not token and config_path is not None:
            token = _read_token(config_path)
        if token is not None and (not isinstance(token, str) or not 16 <= len(token) <= 128 or
                                  any(not 33 <= ord(char) <= 126 for char in token)):
            raise ValueError("Gateway token must contain 16 to 128 printable ASCII characters without spaces.")
        self._token = token
        self.timeout = float(timeout)
        self._lock = threading.Lock()
        # The embedded HTTP listener serves one socket at a time. Let input
        # polling run between output requests, without overlapping TCP sessions.
        self._http_lock = threading.Lock()
        self._connection = None
        self._connection_until = 0
        self._display_status = None
        self._display_status_until = 0

    @property
    def configured(self) -> bool:
        return bool(self._host and self._token)

    def _base(self) -> dict:
        return {"configured": self.configured, "available": False, "protocol": None,
                "connected": False, "lcdReady": False, "ready": False,
                "peer_configured": False, "capabilities": dict(_LIMITATIONS)}

    def _failure(self, error: _GatewayError) -> dict:
        self._display_status_until = 0
        return {**self._base(), "ok": False, "acknowledged": False,
                "code": error.code, "error": str(error), "visual_verified": False}

    def _request(self, method: str, path: str, body: bytes | None = None,
                 *, expected_status: int = 200, binary: bool = False, limit: int = 4096) -> dict | bytes:
        with self._http_lock:
            return self._request_once(method, path, body, expected_status=expected_status,
                                      binary=binary, limit=limit)

    def _request_once(self, method: str, path: str, body: bytes | None = None,
                      *, expected_status: int = 200, binary: bool = False,
                      limit: int = 4096) -> dict | bytes:
        if not self.configured:
            raise _GatewayError("disabled", "NU87 gateway is disabled; configure its IP and local token to enable it.")
        # HTTPConnection ignores proxy environment variables and never follows
        # redirects. Fixed paths and a numeric IP keep the bearer token on LAN.
        # Reuse only connections explicitly kept open by the firmware. Its
        # bounded session releases the single listener for other LAN clients.
        # An ambiguous failure is never retried, including GETs and POSTs.
        connection = self._connection
        self._connection = None
        if connection is not None:
            expired = time.monotonic() >= self._connection_until
            sock = connection.sock
            try:
                # The previous body was fully consumed. Readable bytes now
                # mean EOF or an unexpected reply, so discard before sending
                # a new command. This does not replay an ambiguous write.
                readable, _, failed = (
                    select.select([sock], [], [sock], 0) if sock else ([], [], [])
                )
                expired = expired or sock is None or bool(readable or failed)
            except (OSError, ValueError):
                expired = True
            if expired:
                connection.close()
                connection = None
        if connection is None:
            connection = http.client.HTTPConnection(self._host, self._port, timeout=self.timeout)
        reusable = False
        # Start the reuse budget before the network round trip. A delayed
        # response can arrive after the firmware's 750 ms idle timer has
        # already elapsed; receiving it must not restart the budget. Keep a
        # 100 ms margin below the firmware's 750 ms idle timeout, while allowing
        # ordinary 400-600 ms BLE ACKs to share a socket with the next sample.
        reuse_until = time.monotonic() + 0.65
        try:
            connection.request(method, path, body, {
                "Authorization": "Bearer " + self._token,
                "Content-Type": "application/octet-stream",
                "Connection": "keep-alive",
            })
            response = connection.getresponse()
            if response.status != expected_status:
                if response.status in (401, 403):
                    raise _GatewayError("unauthorized", "NU87 gateway rejected authentication.")
                if response.status in (404, 405):
                    raise _GatewayError("unsupported_feature", "The gateway firmware or selected NU40 peer does not support this feature.")
                raise _GatewayError("http_error", "NU87 gateway returned HTTP " + str(response.status) + ".")
            data = response.read(limit + 1)
            if len(data) > limit:
                raise _GatewayError("invalid_response", "NU87 gateway response exceeded the size limit.")
            # A fixed Content-Length and complete read delimit the next reply.
            # Legacy firmware advertises close and continues to work unchanged.
            reusable = (
                getattr(response, "will_close", True) is False
                and not getattr(response, "chunked", False)
                and response.getheader("Connection", "").lower() == "keep-alive"
                and response.getheader("Content-Length", "") == str(len(data))
            )
            if binary:
                if reusable and time.monotonic() < reuse_until:
                    self._connection = connection
                    self._connection_until = reuse_until
                return data
            try:
                result = json.loads(data)
            except (ValueError, UnicodeError):
                raise _GatewayError("invalid_response", "NU87 gateway returned invalid JSON.") from None
            if not isinstance(result, dict):
                raise _GatewayError("invalid_response", "NU87 gateway returned an invalid object.")
            if reusable and time.monotonic() < reuse_until:
                self._connection = connection
                self._connection_until = reuse_until
            return result
        except TimeoutError:
            raise _GatewayError("timeout", "NU87 gateway timed out; no successful display ACK was confirmed.") from None
        except (OSError, http.client.HTTPException):
            raise _GatewayError("unreachable", "NU87 gateway connection failed.") from None
        finally:
            if self._connection is not connection:
                connection.close()

    def status(self) -> dict:
        try:
            state = self._request("GET", "/v1/status")
            if (not isinstance(state.get("protocol"), str) or
                    any(type(state.get(key)) is not bool for key in ("connected", "lcdReady", "configured"))):
                raise _GatewayError("invalid_response", "NU87 gateway returned invalid status fields.")
            supported = (state["protocol"] == KIT_PROTOCOL and
                         type(state.get("columns")) is int and state["columns"] == 21 and
                         type(state.get("rows")) is int and state["rows"] == 4)
            result = {**self._base(), "ok": supported, "available": True,
                      "protocol": KIT_PROTOCOL if supported else "unsupported",
                      "connected": state["connected"], "lcdReady": state["lcdReady"],
                      "ready": supported and state["connected"] and state["lcdReady"],
                      "peer_configured": state["configured"],
                      "columns": 21 if supported else None, "rows": 4 if supported else None}
            capabilities = state.get("capabilities", {})
            if supported and isinstance(capabilities, dict):
                result["capabilities"].update(microphone_stream=capabilities.get("microphone") is True,
                                               ota=capabilities.get("smp") is True,
                                               ota_protocol="mcumgr-smp" if capabilities.get("smp") is True else None)
            if not supported:
                result.update(code="unsupported_protocol", error="NU87 must run the NU40 KIT display protocol.")
            return result
        except _GatewayError as error:
            return self._failure(error)

    def imu_status(self) -> dict:
        """Read the physical I2C endpoint; no generated or replayed measurements."""
        try:
            data = self._request("GET", "/v1/imu/status")
            if data.get("source") != "nu87-i2c" or type(data.get("configured")) is not bool:
                raise _GatewayError("invalid_response", "NU87 IMU source is invalid.")
            return data
        except _GatewayError as error:
            return {"configured": False, "ready": False, "fresh": False,
                    "source": "nu87-i2c", "error": str(error)}

    def _require_kit(self, *, display: bool = False, reuse_status: bool = False) -> dict:
        state = (
            self._display_status
            if display and reuse_status and time.monotonic() < self._display_status_until
            else self.status()
        )
        if not state["ok"]:
            raise _GatewayError(state["code"], state["error"])
        if display and not state["ready"]:
            raise _GatewayError("not_ready", "Select and connect a NU40 KIT peer before sending display text.")
        return state

    def peers(self) -> dict:
        try:
            state = self._require_kit()
            data = self._request("GET", "/v1/peers")
            if not isinstance(data.get("peers"), list):
                raise _GatewayError("invalid_response", "NU87 gateway returned invalid BLE candidates.")
            peers = []
            for peer in data["peers"]:
                if (not isinstance(peer, dict) or not isinstance(peer.get("address"), str) or
                        not _ADDRESS.fullmatch(peer["address"]) or peer.get("type") not in ("random", "public") or
                        peer.get("name") != "NU40-KIT-DISPLAY" or type(peer.get("rssi")) is not int or
                        not -127 <= peer["rssi"] <= 127):
                    raise _GatewayError("invalid_response", "NU87 gateway returned an invalid BLE candidate.")
                peers.append({key: peer[key] for key in ("address", "type", "name", "rssi")})
            return {**state, "peers": peers}
        except _GatewayError as error:
            return {**self._failure(error), "peers": []}

    def select_peer(self, address: str, address_type: str) -> dict:
        try:
            if not isinstance(address, str) or not _ADDRESS.fullmatch(address) or address_type not in ("random", "public"):
                raise _GatewayError("invalid_peer", "Choose a Bluetooth MAC address and its public or random address type.")
            with self._lock:
                self._display_status_until = 0
                self._require_kit()
                data = self._request("POST", "/v1/peer", (address.upper() + " " + address_type).encode("ascii"),
                                     expected_status=202)
                if data.get("accepted") is not True:
                    raise _GatewayError("invalid_response", "NU87 did not accept the BLE peer selection.")
            return {"ok": True, "configured": True, "available": True, "accepted": True,
                    "connected": False, "connection_pending": True, "acknowledged": False,
                    "message": "Peer selection accepted; poll status to confirm connection."}
        except _GatewayError as error:
            return {**self._failure(error), "accepted": False}

    def display(self, text: str, *, reuse_status: bool = False) -> dict:
        try:
            _validate_text(text)
            with self._lock:
                state = self._require_kit(display=True, reuse_status=reuse_status)
                data = self._request("POST", "/v1/display", text.encode("ascii"))
                if (data.get("ready") is not True or type(data.get("columns")) is not int or data["columns"] != 21 or
                        type(data.get("rows")) is not int or data["rows"] != 4 or data.get("dfuCapable") is not False):
                    raise _GatewayError("unconfirmed_ack", "NU87 did not confirm a successful KIT display ACK.")
                # Only a matching ACK renews this short protocol/readiness cache.
                # The live sensor loop avoids an extra HTTP RTT per OLED update;
                # ordinary/manual writes still perform their status preflight.
                self._display_status = state
                self._display_status_until = time.monotonic() + 3
            return {"ok": True, "configured": True, "available": True, "protocol": KIT_PROTOCOL,
                    "acknowledged": True, "verification": "gateway-matching-ack", "visual_verified": False,
                    "columns": 21, "rows": 4, "message": "Gateway confirmed the matching OLED-driver ACK; inspect the screen separately."}
        except _GatewayError as error:
            return self._failure(error)

    def audio_status(self) -> dict:
        try:
            data = self._request("GET", "/v1/audio/status")
            numbers = ("session", "received", "dropped", "queued")
            if (any(type(data.get(key)) is not bool for key in ("available", "running")) or
                    any(type(data.get(key)) is not int or not 0 <= data[key] <= 0xffffffff for key in numbers) or
                    data["queued"] > 64 or type(data.get("error")) is not int or
                    data.get("sampleRate") != 16000 or data.get("codec") != "ima-adpcm"):
                raise _GatewayError("invalid_response", "NU87 returned invalid microphone status.")
            return {"ok": True, "configured": True,
                    **{key: data[key] for key in (*numbers, "available", "running", "error", "sampleRate", "codec")},
                    # BTN2/BTN3 firmware sends silent frames while armed.
                    # The unchanged protocol cannot report local PDM state.
                    "transport_confirmed": data["running"] and data["received"] > 0,
                    "capture_confirmed": False}
        except _GatewayError as error:
            return {**self._failure(error), "running": False,
                    "transport_confirmed": False, "capture_confirmed": False}

    def _audio_control(self, path: str, body: bytes) -> dict:
        try:
            with self._lock:
                data = self._request("POST", path, body, expected_status=202)
            if type(data.get("error")) is not int or data["error"] != 0:
                raise _GatewayError("invalid_response", "NU87 did not accept the microphone control request.")
            return {"ok": True, "accepted": True, "capture_confirmed": False,
                    "message": ("Control accepted; BTN2 records and BTN3 sends. "
                                "Frames can be silent keepalives.")}
        except _GatewayError as error:
            return {**self._failure(error), "accepted": False, "capture_confirmed": False}

    def audio_start(self, session: int) -> dict:
        if type(session) is not int or not 1 <= session <= 0xffffffff:
            return self._failure(_GatewayError("invalid_session", "Microphone session must be a nonzero uint32."))
        return self._audio_control("/v1/audio/start", str(session).encode("ascii"))

    def audio_stop(self) -> dict:
        return self._audio_control("/v1/audio/stop", b"stop")

    def audio_chunk(self) -> bytes:
        """Drain at most 32 complete ADPCM frames; never silently truncate."""
        try:
            data = self._request("GET", "/v1/audio/chunk", binary=True, limit=176 * 32)
            if len(data) % 176:
                raise _GatewayError("invalid_response", "NU87 returned an incomplete microphone frame.")
            for at in range(0, len(data), 176):
                frame = data[at:at + 176]
                if (frame[:4] != b"WA\x01\x00" or frame[14] > 88 or frame[15] != 0 or frame[175] & 0xf0):
                    raise _GatewayError("invalid_response", "NU87 returned an invalid microphone frame.")
            return data
        except _GatewayError as error:
            raise RuntimeError(str(error)) from None

    def smp(self, packet: bytes) -> bytes:
        """Relay one complete MCUmgr packet, without retries or auto activation."""
        if (not isinstance(packet, bytes) or not 8 <= len(packet) <= 1024 or
                packet[0] not in (0, 2) or packet[1] != 0 or
                int.from_bytes(packet[2:4], "big") != len(packet) - 8):
            raise ValueError("Invalid or oversized SMP request.")
        try:
            with self._lock:
                data = self._request("POST", "/v1/smp", packet, binary=True, limit=1024)
            if (len(data) < 8 or data[0] != packet[0] + 1 or data[1] != 0 or
                    int.from_bytes(data[2:4], "big") != len(data) - 8 or data[4:8] != packet[4:8]):
                raise _GatewayError("invalid_response", "NU87 returned an unmatched SMP response.")
            return data
        except _GatewayError as error:
            raise RuntimeError(str(error)) from None

    def smp_status(self) -> dict:
        try:
            data = self._request("GET", "/v1/smp/status")
            if type(data.get("available")) is not bool:
                raise _GatewayError("invalid_response", "NU87 returned invalid SMP availability.")
            return {"ok": True, "configured": True, "available": data["available"],
                    "protocol": "mcumgr-smp", "authenticated_link_required": True}
        except _GatewayError as error:
            return self._failure(error)

    def alarm(self, active: bool) -> dict:
        try:
            if type(active) is not bool:
                raise _GatewayError("invalid_alarm", "Alarm state must be true or false.")
            with self._lock:
                data = self._request("POST", "/v1/alarm", b"on" if active else b"off")
            if data.get("available") is not True or data.get("active") is not active or data.get("acknowledged") is not True:
                raise _GatewayError("unconfirmed_alarm", "NU40 did not confirm the warning output.")
            return {"ok": True, "available": True, "active": active, "acknowledged": True,
                    "verification": "gateway-matching-output-ack"}
        except _GatewayError as error:
            return {**self._failure(error), "active": None}

    def alarm_status(self) -> dict:
        try:
            data = self._request("GET", "/v1/alarm/status")
            if (type(data.get("available")) is not bool or type(data.get("acknowledged")) is not bool or
                    (data.get("active") is not None and type(data["active"]) is not bool) or
                    any(type(data.get(key)) is not int or not 0 <= data[key] <= 0xffffffff for key in ("eventCounter", "generation")) or
                    data.get("source") not in ("button", "command", "none") or
                    type(data.get("outputStatus")) is not int or not 0 <= data["outputStatus"] <= 2):
                raise _GatewayError("invalid_response", "NU87 returned invalid warning-output status.")
            return {"ok": True, **{key: data[key] for key in
                    ("available", "active", "acknowledged", "eventCounter", "generation", "source", "outputStatus")}}
        except _GatewayError as error:
            return {**self._failure(error), "active": None}

    def _robot_result(self, data: dict, command: str | None = None) -> dict:
        commands = ("stop", "dock", "greet", "forward", "back", "left", "right")
        if (data.get("command") not in commands or data.get("state") not in ("idle", "moving", "greeting", "docking", "alert") or
                type(data.get("sequence")) is not int or not 0 <= data["sequence"] <= 0xffffffff or
                type(data.get("alert")) is not bool or data.get("simulated") is not True or
                (command is not None and (data.get("accepted") is not True or data["command"] != command))):
            raise _GatewayError("invalid_response", "NU87 did not confirm the requested Wallaby command.")
        return {"ok": True, **{key: data[key] for key in ("command", "state", "sequence", "alert", "simulated")},
                "accepted": command is not None}

    def robot_command(self, command: str) -> dict:
        try:
            if command not in ("stop", "dock", "greet", "forward", "back", "left", "right"):
                raise _GatewayError("invalid_motion", "Unknown Wallaby motion command.")
            with self._lock:
                data = self._request("POST", "/v1/robot/command", command.encode("ascii"))
            return self._robot_result(data, command)
        except _GatewayError as error:
            return {**self._failure(error), "accepted": False}

    def robot_status(self) -> dict:
        try:
            return self._robot_result(self._request("GET", "/v1/robot/status"))
        except _GatewayError as error:
            return self._failure(error)

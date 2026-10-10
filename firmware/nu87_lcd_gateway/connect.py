#!/usr/bin/env python3
"""Observe a specific NU87 startup and report its Wi-Fi DHCP address.

This does not send a reset or flash command. Opening a serial port can change
adapter control signals and restart the board; prefer HTTP for an already
connected gateway. Start it before pressing RESET when capturing boot. The
optional eFuse-warning acknowledgement sends one newline only after this serial
session observes the vendor's complete prompt; it never programs eFuse.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile
import time


APP_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = APP_DIR / "local.conf"
STATE_PATH = APP_DIR.parents[1] / "build/nu87-lcd-gateway/gateway-connection.json"
EFUSE_PROMPT = "Efuse empty! Wifi performance may be affected. Press any key to ignore and continue"
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
READY = re.compile(r"(?:Open|NU87 Wi-Fi LCD gateway:)\s+http://([0-9.]+):([0-9]{1,5})/(?:\s|$)")
PRIVATE_NETWORKS = tuple(ipaddress.ip_network(value) for value in
                         ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))
MAX_LINE = 16384


class ConnectionError(RuntimeError):
    """A safe diagnostic; never include raw serial or configuration data."""


def read_redactions(path: Path) -> tuple[str, ...]:
    """Use the provisioning parser, keeping credentials out of command lines."""
    spec = importlib.util.spec_from_file_location("gateway_private_config", APP_DIR / "configure.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    try:
        text = module._read_config(path)
        values = []
        for name in ("CONFIG_GATEWAY_WIFI_PSK", "CONFIG_GATEWAY_API_TOKEN"):
            encoded = module._setting(text, name)
            if encoded is not None:
                value = module._decode_string(encoded)
                if value:
                    values.append(value)
        return tuple(values)
    except (ValueError, OSError):
        raise ConnectionError("Could not safely read private configuration for log redaction.") from None


def clean_line(line: str) -> str:
    line = ANSI.sub("", line)
    return "".join(char for char in line if char.isprintable() or char == "\t")


def redact(line: str, values: tuple[str, ...]) -> str:
    line = clean_line(line)
    for value in sorted(values, key=len, reverse=True):
        line = line.replace(value, "[REDACTED]")
    # Also suppress recognizable credentials if firmware logs a different one.
    return re.sub(r"(?i)((?:password|passphrase|psk|api[_ -]?token|authorization)\s*[:=]\s*).*",
                  r"\1[REDACTED]", line)


def parse_ready(line: str) -> tuple[str, int] | None:
    match = READY.search(clean_line(line))
    if not match:
        return None
    try:
        address = ipaddress.IPv4Address(match[1])
        port = int(match[2])
    except ValueError:
        return None
    if not 1 <= port <= 65535 or not any(address in network for network in PRIVATE_NETWORKS):
        return None
    # These cannot be safely assumed unicast without the DHCP subnet mask.
    if int(address) & 255 in (0, 255):
        return None
    return str(address), port


class Lines:
    """Retain partial lines and suppress overlong lines without leaking fragments."""

    def __init__(self):
        self.pending = bytearray()
        self.discarding = False

    def feed(self, data: bytes) -> list[str]:
        complete = []
        for byte in data:
            if byte in (10, 13):
                if self.discarding:
                    complete.append("[overlong serial line suppressed]")
                elif self.pending:
                    complete.append(self.pending.decode("utf-8", errors="replace"))
                self.pending.clear()
                self.discarding = False
            elif not self.discarding:
                self.pending.append(byte)
                if len(self.pending) > MAX_LINE:
                    self.pending.clear()
                    self.discarding = True
        return complete


def select_port(ports, serial_number: str) -> str:
    matches = [port.device for port in ports
               if port.vid == 0x10C4 and port.pid == 0xEA60
               and port.serial_number == serial_number
               and (port.device.startswith("/dev/cu.")
                    or re.fullmatch(r"/dev/ttyUSB[0-9]+", port.device))]
    if len(matches) != 1:
        raise ConnectionError("Exactly one matching CP2102N USB serial port is required; no port was opened.")
    return matches[0]


def monitor(*, serial_number: str, timeout: float, acknowledge: bool,
            redactions: tuple[str, ...], ports, serial_factory, output=sys.stdout,
            clock=time.monotonic) -> tuple[str, int]:
    if not math.isfinite(timeout) or not 1 <= timeout <= 600:
        raise ConnectionError("Timeout must be finite and between 1 and 600 seconds.")
    if not serial_number or any(not char.isalnum() for char in serial_number):
        raise ConnectionError("Provide the exact alphanumeric USB serial number.")
    device = select_port(ports, serial_number)
    serial = serial_factory(port=None, baudrate=1500000, timeout=0.2, write_timeout=1)
    serial.dtr = False
    serial.rts = False
    serial.port = device
    acknowledged = False
    warned = False
    lines = Lines()
    deadline = clock() + timeout
    try:
        serial.open()
        print("Observing the selected NU87; press RESET to capture a fresh startup if needed.", file=output, flush=True)
        while clock() < deadline:
            for raw in lines.feed(serial.read(2048)):
                line = clean_line(raw)
                print(redact(line, redactions), file=output, flush=True)
                if "<err> os:" in line and line.endswith("Halting system"):
                    raise ConnectionError("NU87 firmware halted; inspect the preceding fault log before resetting.")
                if EFUSE_PROMPT in line and not acknowledged:
                    if acknowledge:
                        if serial.write(b"\n") != 1:
                            raise ConnectionError("Could not send the one-byte startup acknowledgement.")
                        acknowledged = True
                        print("Acknowledged the observed eFuse prompt once; no eFuse data was written.", file=output, flush=True)
                    elif not warned:
                        print("Startup is waiting for a key; automatic acknowledgement was not enabled.", file=output, flush=True)
                        warned = True
                ready = parse_ready(line)
                if ready is not None:
                    return ready
    finally:
        serial.close()
    raise ConnectionError("Timed out without observing a valid private Wi-Fi IPv4 ready message.")


def save_state(path: Path, *, serial_number: str, address: str, port: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    state = {"board": "NU87", "serial_number": serial_number, "ipv4": address,
             "http_port": port, "url": f"http://{address}:{port}",
             "observed_at": datetime.now(timezone.utc).isoformat()}
    descriptor, name = tempfile.mkstemp(prefix=".gateway-connection-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as target:
            os.fchmod(target.fileno(), 0o600)
            json.dump(state, target, indent=2)
            target.write("\n")
            target.flush()
            os.fsync(target.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-number", required=True)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--acknowledge-efuse-warning", action="store_true")
    args = parser.parse_args(argv)
    try:
        import serial
        from serial.tools import list_ports
        redactions = read_redactions(args.config)
        address, port = monitor(serial_number=args.serial_number, timeout=args.timeout,
                                acknowledge=args.acknowledge_efuse_warning,
                                redactions=redactions, ports=list_ports.comports(),
                                serial_factory=serial.Serial)
        save_state(STATE_PATH, serial_number=args.serial_number, address=address, port=port)
        print(f"Observed NU87 Wi-Fi ready: http://{address}:{port}/")
    except ConnectionError as error:
        print(f"Connection error: {error}", file=sys.stderr)
        return 1
    except (ImportError, OSError, ValueError):
        print("Connection error: serial access or local state storage failed.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Connection observation cancelled.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

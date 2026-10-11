"""Persistent, identity-matched NU87 USB-UART transport for the KIT gateway.

The protocol uses COBS, a whole-message CRC and authenticated sessions. Every
operation has a new sequence; neither ambiguous writes nor audio drains replay.
Boot diagnostics are discarded and never returned to the browser.
"""
from __future__ import annotations

import json
import secrets
import struct
import time
import zlib

from .gateway import Gateway, _GatewayError

HEADER = struct.Struct("<2sBBIIHBBHH")
WIRE_MAX = 6144
BODY_MAX = 5632
EFUSE_PROMPT = (
    b"Efuse empty! Wifi performance may be affected. Press any key to ignore and continue"
)
READY = b"MWUSB|READY version=1 transport=usb"


def cobs_encode(data: bytes) -> bytes:
    out = bytearray(b"\0")
    code_at, code = 0, 1
    for value in data:
        if value == 0:
            out[code_at] = code
            code_at = len(out)
            out.append(0)
            code = 1
        else:
            out.append(value)
            code += 1
            if code == 255:
                out[code_at] = code
                code_at = len(out)
                out.append(0)
                code = 1
    out[code_at] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out, at = bytearray(), 0
    while at < len(data):
        code = data[at]
        at += 1
        if not code or at + code - 1 > len(data) or 0 in data[at:at + code - 1]:
            raise ValueError("Invalid USB frame.")
        out.extend(data[at:at + code - 1])
        at += code - 1
        if code != 255 and at < len(data):
            out.append(0)
        if len(out) > WIRE_MAX:
            raise ValueError("Oversized USB frame.")
    return bytes(out)


def pack_request(session: int, sequence: int, method: int, path: str,
                 token: str, body: bytes = b"") -> bytes:
    path_bytes, token_bytes = path.encode("ascii"), token.encode("ascii")
    if (method not in (0, 1, 2) or not 0 < session <= 0xffffffff or
            not 0 < sequence <= 0xffffffff or len(path_bytes) > 95 or
            not 16 <= len(token_bytes) <= 128 or len(body) > 1024 or
            (method == 0 and (path_bytes or body)) or
            (method != 0 and not path.startswith("/v1/")) or
            any(not 33 <= value <= 126 for value in path_bytes + token_bytes)):
        raise ValueError("Invalid USB request.")
    packet = (HEADER.pack(b"MW", 1, 1, session, sequence, method,
                          len(path_bytes), len(token_bytes), len(body), 0) +
              path_bytes + token_bytes + body)
    packet += struct.pack("<I", zlib.crc32(packet))
    return b"\0" + cobs_encode(packet) + b"\0"


def unpack_response(frame: bytes, session: int, sequence: int) -> tuple[int, bytes]:
    data = cobs_decode(frame)
    if len(data) < HEADER.size + 4:
        raise ValueError("Incomplete USB response.")
    magic, version, kind, owner, seq, status, path, token, size, reserved = HEADER.unpack_from(data)
    if (magic != b"MW" or version != 1 or kind != 2 or owner != session or seq != sequence or
            not 100 <= status <= 599 or path or token or reserved or size > BODY_MAX or
            len(data) != HEADER.size + size + 4 or
            zlib.crc32(data[:-4]) != struct.unpack_from("<I", data, len(data) - 4)[0]):
        raise ValueError("Unmatched or damaged USB response.")
    return status, data[HEADER.size:-4]


class SerialGateway(Gateway):
    """One open serial descriptor is shared by all existing gateway methods.

    Opening this CP2102N can reset the attached board even with DTR/RTS low.
    Keeping the descriptor open is part of the transport's behavior.
    """

    def __init__(self, serial_number: str, *, token=None, config_path=None,
                 timeout: float = 5.0, serial_factory=None, ports=None):
        super().__init__(token=token, config_path=config_path, timeout=timeout)
        if (not isinstance(serial_number, str) or not serial_number or
                not serial_number.isascii() or not serial_number.isalnum()):
            raise ValueError("Provide the exact NU87 USB serial number.")
        self.serial_number = serial_number
        self._serial_factory, self._ports = serial_factory, ports
        self._serial = None
        self._session = self._sequence = 0
        self._pending = bytearray()
        self._discard = False
        self._next_open = 0.0

    @property
    def configured(self):
        return bool(self.serial_number and self._token)

    def _base(self):
        return {**super()._base(), "transport": "usb"}

    def _open(self):
        if self._serial is not None:
            return
        if time.monotonic() < self._next_open:
            raise _GatewayError("usb_unavailable", "NU87 USB is reconnecting.")
        try:
            import serial
            from serial.tools import list_ports
        except ImportError:
            raise _GatewayError("usb_dependency", "Install pyserial for the USB gateway.") from None
        ports = self._ports() if self._ports else list_ports.comports()
        matches = [p for p in ports if p.vid == 0x10c4 and p.pid == 0xea60 and
                   p.serial_number == self.serial_number]
        if len(matches) != 1:
            raise _GatewayError("usb_unavailable", "The configured NU87 USB device is not present.")
        factory = self._serial_factory or serial.Serial
        connection = factory(port=None, baudrate=1500000, timeout=0.05, write_timeout=2,
                             exclusive=True)
        connection.dtr = connection.rts = False
        connection.port = matches[0].device
        self._serial = connection
        connection.open()
        startup, acknowledged = bytearray(), False
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            startup.extend(connection.read(2048))
            if not acknowledged and EFUSE_PROMPT in startup:
                # Dismiss this observed RAM-only vendor startup prompt once.
                # No OTP/EFUSE operation, reset command or flash command is sent.
                if connection.write(b"\n") != 1:
                    raise OSError("Startup acknowledgement failed.")
                acknowledged = True
            if READY in startup:
                self._pending.clear()
                self._discard = False
                return
            if len(startup) > 32768:
                del startup[:-16384]
        raise _GatewayError("usb_firmware", "NU87 did not report the USB gateway firmware ready.")

    def close(self):
        connection, self._serial = self._serial, None
        self._session = self._sequence = 0
        self._pending.clear()
        if connection is not None:
            connection.close()

    def _exchange(self, method: int, path: str, body: bytes) -> tuple[int, bytes]:
        self._sequence += 1
        wire = pack_request(self._session, self._sequence, method, path, self._token, body)
        # The shared AmebaD log UART has a 16-byte FIFO and its KM0 monitor
        # owns the IRQ. Firmware disables that monitor and polls at 1 ms.
        # Pace eight-byte bursts; never assume a PC write is wire flow control.
        for offset in range(0, len(wire), 8):
            chunk = wire[offset:offset + 8]
            if self._serial.write(chunk) != len(chunk):
                raise OSError("Incomplete USB write.")
            time.sleep(.002)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            for byte in self._serial.read(2048):
                if byte == 0:
                    frame = bytes(self._pending)
                    self._pending.clear()
                    discard, self._discard = self._discard, False
                    if frame and not discard:
                        try:
                            return unpack_response(frame, self._session, self._sequence)
                        except ValueError:
                            pass
                elif not self._discard:
                    self._pending.append(byte)
                    if len(self._pending) > WIRE_MAX:
                        self._pending.clear()
                        self._discard = True
        raise _GatewayError("timeout", "NU87 USB request timed out; the command was not replayed.")

    def _request_once(self, method, path, body=None, *, expected_status=200,
                      binary=False, limit=4096):
        if not self.configured:
            raise _GatewayError("disabled", "Configure the NU87 USB serial number and local token.")
        try:
            self._open()
            if not self._session or self._sequence >= 0xffffffff:
                self._session, self._sequence = secrets.randbelow(0xffffffff) + 1, 0
                code, hello = self._exchange(0, "", b"")
                if code != 200 or json.loads(hello) != {"protocol": "wallaby-usb", "version": 1}:
                    raise _GatewayError("usb_firmware", "NU87 USB session was not accepted.")
            code, data = self._exchange({"GET": 1, "POST": 2}[method], path, body or b"")
            if code != expected_status:
                if code == 409 and data == b"session conflict":
                    # Reauthenticate the NEXT operation; never replay this one.
                    self._session = 0
                if code in (401, 403):
                    raise _GatewayError("unauthorized", "NU87 USB rejected authentication.")
                raise _GatewayError("device_error", "NU87 USB returned status " + str(code) + ".")
            if len(data) > limit:
                raise _GatewayError("invalid_response", "NU87 USB response exceeded the size limit.")
            if binary:
                return data
            result = json.loads(data)
            if not isinstance(result, dict):
                raise ValueError("Invalid object.")
            return result
        except _GatewayError as error:
            if error.code == "usb_firmware":
                self.close()
                self._next_open = time.monotonic() + 5
            raise
        except (OSError, ValueError, KeyError):
            self.close()
            self._next_open = time.monotonic() + 5
            raise _GatewayError("usb_unavailable", "NU87 USB communication failed.") from None

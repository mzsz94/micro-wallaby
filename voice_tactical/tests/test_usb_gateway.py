"""USB framing, real C interoperability and persistent/no-replay behavior."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

from voice_tactical.gateway import _GatewayError
from voice_tactical.usb_gateway import (
    HEADER, READY, EFUSE_PROMPT, SerialGateway, cobs_decode, cobs_encode,
    pack_request, unpack_response,
)

TOKEN = "usb-test-only-token"
PORT = SimpleNamespace(device="/dev/ttyUSB9", vid=0x10c4, pid=0xea60,
                       serial_number="1234abcd")


def response(session, seq, body=b"{}", status=200):
    packet = HEADER.pack(b"MW", 1, 2, session, seq, status, 0, 0, len(body), 0) + body
    packet += struct.pack("<I", zlib.crc32(packet))
    return b"\0" + cobs_encode(packet) + b"\0"


class FakeSerial:
    def __init__(self, **kwargs):
        self.incoming = bytearray(EFUSE_PROMPT + b"\r\n")
        self.opens = self.closes = 0
        self.writes = []
        self.port = None
        self.dtr = self.rts = None
        self.mode = "ok"

    def open(self):
        self.opens += 1
        assert self.dtr is False and self.rts is False

    def close(self):
        self.closes += 1

    def write(self, wire):
        self.writes.append(wire)
        if wire == b"\n":
            self.incoming.extend(READY + b"\r\n")
            return 1
        raw = cobs_decode(wire[1:-1])
        _, _, _, session, seq, method, path, token, size, _ = HEADER.unpack_from(raw)
        self.incoming.extend(b"unrelated diagnostic text\n\0\x03bad\0")
        if self.mode == "timeout" and method != 0:
            return len(wire)
        body = (b'{"protocol":"wallaby-usb","version":1}' if method == 0 else
                b'{"ok":true}')
        # A valid response to an earlier operation must not satisfy this one.
        self.incoming.extend(response(session, seq - 1, b'{"old":true}'))
        self.incoming.extend(response(session, seq, body))
        return len(wire)

    def read(self, size):
        # Exercise every boundary, including empty reads and split CRCs.
        data = bytes(self.incoming[:7])
        del self.incoming[:7]
        return data


class USBProtocolTests(unittest.TestCase):
    def test_roundtrip_all_bytes_and_boundaries(self):
        for data in (b"", b"\0", bytes(range(256)), b"x" * 254, b"x" * 255,
                     bytes(range(256)) * 22):
            with self.subTest(length=len(data)):
                self.assertEqual(cobs_decode(cobs_encode(data)), data)
                self.assertNotIn(0, cobs_encode(data))

    def test_corruption_identity_and_lengths_are_rejected(self):
        packet = response(11, 42, b"\0\xff\n")[1:-1]
        self.assertEqual(unpack_response(packet, 11, 42), (200, b"\0\xff\n"))
        for session, seq in ((12, 42), (11, 41)):
            with self.assertRaises(ValueError):
                unpack_response(packet, session, seq)
        data = bytearray(cobs_decode(packet))
        data[-5] ^= 1
        with self.assertRaises(ValueError):
            unpack_response(cobs_encode(data), 11, 42)
        for bad in (b"\0", b"\x03x", b"\x02\0", b"\x01"):
            with self.assertRaises(ValueError):
                unpack_response(bad, 11, 42)

    def test_request_limits_include_binary_smp(self):
        wire = pack_request(1, 2, 2, "/v1/smp", TOKEN, bytes(range(256)) * 4)
        self.assertEqual(len(cobs_decode(wire[1:-1])), HEADER.size + 7 + len(TOKEN) + 1024 + 4)
        for changes in ((0, 1, 1, "/v1/status", TOKEN, b""),
                        (1, 1, 3, "/v1/status", TOKEN, b""),
                        (1, 1, 1, "/outside", TOKEN, b""),
                        (1, 1, 2, "/v1/smp", TOKEN, b"x" * 1025),
                        (1, 1, 0, "/v1/status", TOKEN, b"")):
            with self.assertRaises(ValueError):
                pack_request(*changes)

    def test_persistent_port_exact_identity_and_one_startup_ack(self):
        serial = FakeSerial()
        gateway = SerialGateway("1234abcd", token=TOKEN,
                                serial_factory=lambda **kw: serial, ports=lambda: [PORT])
        self.assertEqual(gateway._request("GET", "/v1/status"), {"ok": True})
        self.assertEqual(gateway._request("POST", "/v1/display", b"hello"), {"ok": True})
        self.assertEqual(serial.opens, 1)
        self.assertEqual(serial.writes.count(b"\n"), 1)
        self.assertEqual(len(serial.writes), 4)  # LF, HELLO, GET, POST.
        self.assertEqual(gateway._base()["transport"], "usb")
        gateway.close()
        self.assertEqual(serial.closes, 1)

    def test_different_board_and_ambiguous_mutation_do_not_replay(self):
        gateway = SerialGateway("anotherboard", token=TOKEN, ports=lambda: [PORT])
        with self.assertRaises(_GatewayError):
            gateway._request("GET", "/v1/status")
        serial = FakeSerial()
        gateway = SerialGateway("1234abcd", token=TOKEN, timeout=.01,
                                serial_factory=lambda **kw: serial, ports=lambda: [PORT])
        gateway._request("GET", "/v1/status")
        serial.mode = "timeout"
        before = len(serial.writes)
        with self.assertRaisesRegex(_GatewayError, "not replayed"):
            gateway._request("POST", "/v1/display", b"once")
        self.assertEqual(len(serial.writes), before + 1)
        self.assertEqual(serial.opens, 1)
        serial.mode = "ok"
        self.assertEqual(gateway._request("GET", "/v1/status"), {"ok": True})
        self.assertEqual(serial.opens, 1)

    def test_production_c_matches_python_and_rejects_tampering(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        self.assertIsNotNone(compiler)
        include = Path(__file__).resolve().parents[2] / "firmware/nu87_lcd_gateway/include"
        raw = cobs_decode(pack_request(91, 2, 2, "/v1/smp", TOKEN, bytes(range(256)) * 4)[1:-1])
        source = r'''
#include <assert.h>
#include <stdio.h>
#include "gateway_usb_protocol.h"
int main(void) {
 struct usb_rpc_session session={0};
 assert(!usb_rpc_accept(&session,91,2,false));
 assert(!usb_rpc_accept(&session,91,2,true));
 assert(usb_rpc_accept(&session,91,1,true));
 assert(!usb_rpc_accept(&session,91,1,true));
 assert(usb_rpc_accept(&session,91,2,false));
 assert(!usb_rpc_accept(&session,91,2,false));
 assert(!usb_rpc_accept(&session,91,1,false));
 assert(!usb_rpc_accept(&session,92,3,false));
 assert(usb_rpc_accept(&session,92,1,true));
 assert(!usb_rpc_accept(&session,91,3,false));
 assert(usb_rpc_accept(&session,92,0xffffffff,false));
 assert(!usb_rpc_accept(&session,92,1,false));
 uint8_t data[6144], wire[6144], back[6144];
 size_t n=fread(data,1,sizeof(data),stdin);
 struct usb_rpc_request r;
 assert(usb_rpc_parse(data,n,&r));
 assert(r.session==91 && r.sequence==2 && r.method==2 && r.length==1024);
 assert(!strcmp(r.path,"/v1/smp") && !strcmp(r.token,"usb-test-only-token"));
 size_t used=usb_rpc_encode(data,n,wire,sizeof(wire));
 assert(used && usb_rpc_decode(wire,used,back,sizeof(back))==n && !memcmp(data,back,n));
 assert(!usb_rpc_encode(data,n,wire,8));
 assert(!usb_rpc_decode(wire,used,back,1));
 data[n-5]^=1;assert(!usb_rpc_parse(data,n,&r));data[n-5]^=1;
 data[18]=1;assert(!usb_rpc_parse(data,n,&r));data[18]=0;
 assert(!usb_rpc_parse(data,n-1,&r));
 fwrite(wire,1,used,stdout);
 return 0;
}
'''
        # Keep output intact for the cross-language assertion; capacity tests
        # use a separate output buffer so a failed encode cannot alter it.
        source = source.replace('usb_rpc_encode(data,n,wire,8)', 'usb_rpc_encode(data,n,back,8)')
        with tempfile.TemporaryDirectory() as directory:
            cfile, exe = Path(directory) / "usb.c", Path(directory) / "usb"
            cfile.write_text(source)
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I" + str(include), str(cfile),
                            "-o", str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], input=raw, check=True, capture_output=True)
            self.assertEqual(result.stdout, cobs_encode(raw))

    def test_unready_firmware_closes_and_backs_off(self):
        serial = FakeSerial()
        serial.incoming.clear()
        gateway = SerialGateway("1234abcd", token=TOKEN,
                                serial_factory=lambda **kw: serial, ports=lambda: [PORT])
        ticks = iter([0, 0, 21, 22])
        with patch("voice_tactical.usb_gateway.time.monotonic", side_effect=lambda: next(ticks)):
            with self.assertRaises(_GatewayError) as error:
                gateway._request("GET", "/v1/status")
        self.assertEqual(error.exception.code, "usb_firmware")
        self.assertEqual(serial.closes, 1)
        self.assertIsNone(gateway._serial)
        self.assertEqual(gateway._next_open, 27)


if __name__ == "__main__":
    unittest.main()

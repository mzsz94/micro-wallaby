"""NU40 package and bridge regressions; only ephemeral loopback HTTP is used.

Run: python3 -m unittest discover -s web/tests -p test_lcd_bridge.py -v
No board, external network, third-party package, or persistent server is used.
"""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import http.client
import io
import json
from pathlib import Path
import struct
import sys
import threading
import time
import unittest
from unittest.mock import patch
import warnings
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from lcd_bridge import Controller, Handler, main
from lcd_package import APP_START, MAX_IMAGE, MAX_PACKAGE, TARGET, image_metadata, make_package, validate_package


def image_bytes(size=64):
    return struct.pack("<II", 0x20040000, APP_START + 9) + bytes(i % 256 for i in range(8, size))


def package_with(manifest=None, image=None, extra=(), compression=zipfile.ZIP_STORED):
    if image is None:
        image = image_bytes()
    if manifest is None:
        manifest = image_metadata(image, 0x01020304)
    stream = io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(stream, "w", compression) as archive:
            archive.writestr("manifest.json", json.dumps(manifest))
            archive.writestr("application.bin", image)
            for name, value in extra:
                archive.writestr(name, value)
    return stream.getvalue()


class PackageTests(unittest.TestCase):
    def test_known_nrfutil_crc_fixture_and_roundtrip(self):
        image = image_bytes()
        expected = {
            "format": 1, "target": TARGET, "address": 0x26000, "size": 64,
            "version": 0x01020304, "sdFwid": 0xB6, "crc16": 0xEE75,
            "sha256": hashlib.sha256(image).hexdigest(),
        }
        self.assertEqual(image_metadata(image, 0x01020304), expected)
        self.assertEqual(validate_package(make_package(image, 0x01020304)), (expected, image))

    def test_wrong_target_address_size_crc_hash_and_unknown_fields(self):
        original = image_metadata(image_bytes(), 7)
        changes = {
            "target": "nu87", "address": 0x1000, "size": 60,
            "sdFwid": 0xFFFE, "crc16": original["crc16"] ^ 1,
            "sha256": "0" * 64, "format": 2, "unexpected": True,
        }
        for key, value in changes.items():
            with self.subTest(field=key), self.assertRaises(ValueError):
                validate_package(package_with({**original, key: value}))

    def test_image_tamper_with_unchanged_manifest_is_rejected(self):
        image = image_bytes()
        changed = image[:-1] + bytes([image[-1] ^ 1])
        with self.assertRaises(ValueError):
            validate_package(package_with(image_metadata(image, 7), changed))

    def test_versions_are_bounded_integers_not_booleans(self):
        for version in (-1, 0x100000000, True, False, 1.0, "1", None):
            with self.subTest(version=version), self.assertRaises(ValueError):
                image_metadata(image_bytes(), version)
        for version in (0, 0xFFFFFFFF):
            self.assertEqual(validate_package(make_package(image_bytes(), version))[0]["version"], version)

    def test_size_alignment_and_application_limit(self):
        for size in (0, 4, 7, 9, 63, MAX_IMAGE + 4):
            with self.subTest(size=size), self.assertRaises(ValueError):
                image_metadata(image_bytes(size) if size >= 8 else bytes(size), 1)
        self.assertEqual(image_metadata(image_bytes(MAX_IMAGE), 1)["size"], MAX_IMAGE)
        with self.assertRaises(ValueError):
            validate_package(bytes(MAX_PACKAGE + 1))

    def test_stack_reset_thumb_and_image_boundaries(self):
        valid = image_bytes()
        invalid = ((0x20000000, APP_START + 9), (0x20040008, APP_START + 9),
                   (0x2003FFFF, APP_START + 9), (0x20040000, APP_START + 8),
                   (0x20040000, APP_START - 1), (0x20040000, APP_START + 65))
        for sp, reset in invalid:
            with self.subTest(sp=hex(sp), reset=hex(reset)), self.assertRaises(ValueError):
                image_metadata(struct.pack("<II", sp, reset) + valid[8:], 1)

    def test_duplicate_members_and_extra_paths_are_rejected(self):
        for name in ("application.bin", "manifest.json", "../application.bin", "nested/application.bin", "bootloader.bin"):
            with self.subTest(member=name), self.assertRaises(ValueError):
                validate_package(package_with(extra=((name, b"bad"),)))
        stream = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(stream, "w") as archive:
                archive.writestr("manifest.json", "{}")
                archive.writestr("manifest.json", "{}")
        with self.assertRaises(ValueError):
            validate_package(stream.getvalue())

    def test_declared_zip_expansion_and_manifest_limits(self):
        large = image_bytes(MAX_IMAGE + 4)
        with self.assertRaises(ValueError):
            validate_package(package_with({}, large, compression=zipfile.ZIP_DEFLATED))
        with self.assertRaises(ValueError):
            validate_package(package_with({"padding": "x" * 2048}))

    def test_bad_zip_json_and_missing_manifest_version(self):
        for data in (b"", b"not a ZIP", make_package(image_bytes(), 1)[:-20], package_with({}), package_with([])):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                validate_package(data)
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            archive.writestr("manifest.json", "{invalid")
            archive.writestr("application.bin", image_bytes())
        with self.assertRaises(ValueError):
            validate_package(stream.getvalue())

    def test_unsupported_zip_compression_is_a_validation_error(self):
        with self.assertRaises(ValueError):
            validate_package(package_with(compression=zipfile.ZIP_DEFLATED))
        data = bytearray(package_with())
        # Both local and central directory headers must report the same method.
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            offsets = [item.header_offset for item in archive.infolist()]
        for offset in offsets:
            struct.pack_into("<H", data, offset + 8, 99)
        offset = 0
        while (offset := data.find(b"PK\x01\x02", offset)) != -1:
            struct.pack_into("<H", data, offset + 10, 99)
            offset += 4
        with self.assertRaises(ValueError):
            validate_package(bytes(data))

    def test_encrypted_zip_flags_are_rejected_before_extraction(self):
        data = bytearray(package_with())
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            offsets = [item.header_offset for item in archive.infolist()]
        for offset in offsets:
            struct.pack_into("<H", data, offset + 6, 1)
        offset = 0
        while (offset := data.find(b"PK\x01\x02", offset)) != -1:
            struct.pack_into("<H", data, offset + 8, 1)
            offset += 4
        with self.assertRaises(ValueError):
            validate_package(bytes(data))


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.controller = Controller("127.0.0.1", "test-token-123456789")
        # Existing protocol unit tests isolate binary LCD/DFU handling. HTTP
        # tests below exercise the new gateway status preflight end to end.
        status = patch.object(self.controller, "status", return_value={"protocol": "legacy-lcd"})
        status.start()
        self.addCleanup(status.stop)

    @staticmethod
    def ack(_method, _path, body, result=0, flags=3, columns=20, rows=4):
        return body[:4] + bytes((result, flags, columns, rows))

    def test_matching_ack_and_sequence_wrap(self):
        self.controller.sequence = 0xFFFF
        with patch.object(self.controller, "request", side_effect=self.ack) as request:
            self.assertEqual(self.controller.command(3, b"\x01"),
                             {"ready": True, "dfuCapable": True, "columns": 20, "rows": 4})
        request.assert_called_once_with("POST", "/v1/lcd", bytes((1, 3, 0, 0, 1)))

    def test_wrong_ack_version_opcode_sequence_and_length(self):
        for position in range(4):
            def wrong(method, path, body):
                response = bytearray(self.ack(method, path, body))
                response[position] ^= 1
                return bytes(response)
            with self.subTest(position=position), patch.object(self.controller, "request", side_effect=wrong):
                with self.assertRaises(ValueError):
                    self.controller.command(2)
        for size in (0, 4, 7, 9):
            with self.subTest(size=size), patch.object(self.controller, "request", return_value=bytes(size)):
                with self.assertRaises(ValueError):
                    self.controller.command(0)

    def test_all_remote_lcd_errors_are_reported(self):
        for result in (1, 2, 3, 4, 5, 255):
            with self.subTest(result=result):
                with patch.object(self.controller, "request", side_effect=lambda m, p, b: self.ack(m, p, b, result=result)):
                    with self.assertRaises(ValueError):
                        self.controller.command(0)

    def test_non_ascii_controls_and_excess_length_never_reach_gateway(self):
        with patch.object(self.controller, "request") as request:
            for text in (None, 123, "한글", "tab\there", "\r", "\x00", "\x7f", "a" * 85):
                with self.subTest(text=repr(text)), self.assertRaises(ValueError):
                    self.controller.display(text)
            request.assert_not_called()

    def test_display_fills_all_cells_with_bounded_packets(self):
        with patch.object(self.controller, "request", side_effect=self.ack) as request:
            self.controller.display("12345678901234567890\nsecond")
        calls = [call.args[2] for call in request.call_args_list]
        self.assertEqual(calls[0][1], 0)  # STATUS before any mutation.
        self.assertEqual(len(calls), 9)  # Two chunks per each of four rows.
        cells = [bytearray(b"?" * 20) for _ in range(4)]
        for command in calls[1:]:
            self.assertEqual(command[1], 2)
            self.assertLessEqual(len(command), 20)
            row, column = command[4:6]
            cells[row][column:column + len(command) - 6] = command[6:]
        self.assertEqual([bytes(row) for row in cells],
                         [b"12345678901234567890", b"second              ", b" " * 20, b" " * 20])

    def test_display_dimension_limits_prevent_partial_writes(self):
        for text in ("x" * 21, "1\n2\n3\n4\n5"):
            with self.subTest(text=text), patch.object(self.controller, "request", side_effect=self.ack) as request:
                with self.assertRaises(ValueError):
                    self.controller.display(text)
                self.assertEqual(request.call_count, 1)  # Only read-only STATUS.
        for flags, columns, rows in ((0, 20, 4), (3, 0, 4), (3, 21, 4), (3, 20, 0), (3, 20, 5)):
            with self.subTest(flags=flags, columns=columns, rows=rows):
                with patch.object(self.controller, "request", side_effect=lambda m, p, b: self.ack(m, p, b, flags=flags, columns=columns, rows=rows)) as request:
                    with self.assertRaises(ValueError):
                        self.controller.display("text")
                    self.assertEqual(request.call_count, 1)

    def test_error_during_display_never_reports_success(self):
        count = 0
        def reply(method, path, body):
            nonlocal count
            count += 1
            return self.ack(method, path, body, result=3 if count == 3 else 0)
        with patch.object(self.controller, "request", side_effect=reply):
            with self.assertRaises(ValueError):
                self.controller.display("hello")
        self.assertEqual(count, 3)

    def test_ota_uses_checked_application_descriptor_and_requires_capability(self):
        self.controller.serve_firmware_lan = True
        manifest = self.controller.stage(make_package(image_bytes(), 0x01020304))
        def reply(method, path, body):
            return self.ack(method, path, body) if path == "/v1/lcd" else b'{}'
        with patch.object(self.controller, "request", side_effect=reply) as request:
            self.assertEqual(self.controller.start_ota(manifest["sha256"], 8766), {"accepted": True})
        descriptor = request.call_args_list[-1].args[2]
        self.assertEqual(len(descriptor), 80)
        self.assertEqual(struct.unpack_from("<4sIIHH", descriptor), (b"LCD1", 64, 0x01020304, 0xEE75, 8766))
        self.assertEqual(descriptor[16:].decode("ascii"), manifest["sha256"])
        with patch.object(self.controller, "request", side_effect=lambda m, p, b: self.ack(m, p, b, flags=1)) as request:
            with self.assertRaises(ValueError):
                self.controller.start_ota(manifest["sha256"], 8766)
            self.assertEqual(request.call_count, 1)
        with patch.object(self.controller, "request") as request:
            with self.assertRaises(ValueError):
                self.controller.start_ota("0" * 64, 8766)
            request.assert_not_called()

    def test_ota_refreshes_source_lifetime_and_rejects_expired_stage(self):
        self.controller.serve_firmware_lan = True
        manifest = self.controller.stage(make_package(image_bytes(), 1))
        image_id = manifest["sha256"]
        old = time.monotonic() - 3590
        self.controller.images[image_id] = (manifest, image_bytes(), old)
        def reply(method, path, body):
            return self.ack(method, path, body) if path == "/v1/lcd" else b'{}'
        with patch.object(self.controller, "request", side_effect=reply):
            self.controller.start_ota(image_id, 8766)
        self.assertGreater(self.controller.images[image_id][2], old + 3500)
        self.controller.images[image_id] = (manifest, image_bytes(), time.monotonic() - 3601)
        with patch.object(self.controller, "request") as request:
            with self.assertRaises(ValueError):
                self.controller.start_ota(image_id, 8766)
            request.assert_not_called()

    def test_stage_is_bounded_and_restage_same_image_does_not_consume_slot(self):
        for i in range(4):
            image = bytearray(image_bytes()); image[-1] = i
            self.controller.stage(make_package(bytes(image), 1))
        image = bytearray(image_bytes()); image[-1] = 5
        with self.assertRaises(ValueError):
            self.controller.stage(make_package(bytes(image), 1))
        image[-1] = 3
        self.controller.stage(make_package(bytes(image), 1))
        self.assertEqual(len(self.controller.images), 4)

    def test_ota_disabled_by_default_before_any_gateway_request(self):
        manifest = self.controller.stage(make_package(image_bytes(), 1))
        original = self.controller.images[manifest["sha256"]]
        with patch.object(self.controller, "request") as request:
            with self.assertRaisesRegex(ValueError, "--serve-firmware-lan"):
                self.controller.start_ota(manifest["sha256"], 8766)
            request.assert_not_called()
        self.assertEqual(self.controller.images[manifest["sha256"]], original)


class KitControllerTests(unittest.TestCase):
    def setUp(self):
        self.controller = Controller("127.0.0.1", "test-token-123456789", serve_firmware_lan=True)
        self.state = {"protocol": "nu40-kit-display", "connected": True, "lcdReady": True,
                      "columns": 21, "rows": 4, "capabilities": {"text": True, "power": False, "ota": False}}

    def reply(self, method, path, body=None):
        if (method, path) == ("GET", "/v1/status"):
            return json.dumps(self.state).encode()
        if (method, path) == ("POST", "/v1/display"):
            return b'{"ready":true,"columns":21,"rows":4}'
        self.fail(f"Unexpected gateway request: {method} {path}")

    def test_kit_forwards_exact_text_once_without_legacy_lcd_packets(self):
        for text in ("Hello\nNU40", "a" * 84, "a" * 21 + "\n" + "b" * 21, "\n" * 84, " "):
            with self.subTest(text=repr(text)), patch.object(self.controller, "request", side_effect=self.reply) as request:
                self.assertEqual(self.controller.display(text), {"ready": True, "columns": 21, "rows": 4})
                self.assertEqual([call.args for call in request.call_args_list],
                                 [("GET", "/v1/status"), ("POST", "/v1/display", text.encode("ascii"))])

    def test_kit_rejects_empty_and_layout_overflow_before_any_mutation(self):
        for text in ("", "1\n2\n3\n4\n5", "\n" * 4 + "x", "x" * 43 + "\n\nx"):
            with self.subTest(text=repr(text)), patch.object(self.controller, "request", side_effect=self.reply) as request:
                with self.assertRaises(ValueError):
                    self.controller.display(text)
                request.assert_called_once_with("GET", "/v1/status")
        for text in ("a" * 85, "한글", "\r", "\x00"):
            with self.subTest(text=repr(text)), patch.object(self.controller, "request") as request:
                with self.assertRaises(ValueError):
                    self.controller.display(text)
                request.assert_not_called()

    def test_kit_disconnected_and_invalid_ack_are_not_success(self):
        for key in ("connected", "lcdReady"):
            self.state[key] = False
            with patch.object(self.controller, "request", side_effect=self.reply) as request:
                with self.assertRaises(ValueError):
                    self.controller.display("Hello")
                request.assert_called_once_with("GET", "/v1/status")
            self.state[key] = True
        for ack in ({"ready": False, "columns": 21, "rows": 4}, {"ready": True, "columns": 20, "rows": 4}, [], {}):
            with self.subTest(ack=ack), patch.object(self.controller, "request", side_effect=[json.dumps(self.state), json.dumps(ack)]):
                with self.assertRaises(ValueError):
                    self.controller.display("Hello")

    def test_kit_power_and_all_ota_mutations_blocked_even_with_legacy_package(self):
        manifest = self.controller.stage(make_package(image_bytes(), 1))
        original = self.controller.images[manifest["sha256"]]
        for operation in (lambda: self.controller.power(True), self.controller.cancel_ota,
                          lambda: self.controller.start_ota(manifest["sha256"], 8766)):
            with patch.object(self.controller, "request", side_effect=self.reply) as request:
                with self.assertRaisesRegex(ValueError, "지원하지 않습니다"):
                    operation()
                request.assert_called_once_with("GET", "/v1/status")
        self.assertEqual(self.controller.images[manifest["sha256"]], original)
        # Protocol identity itself blocks these operations if capability fields
        # are absent or accidentally advertise legacy support.
        self.state["capabilities"] = {"power": True, "ota": True}
        with patch.object(self.controller, "request", side_effect=self.reply) as request:
            with self.assertRaises(ValueError):
                self.controller.start_ota(manifest["sha256"], 8766)
            request.assert_called_once_with("GET", "/v1/status")

    def test_peer_selection_preserves_address_type_and_never_uses_name(self):
        peers = {"peers": [{"address": "AA:BB:CC:DD:EE:FF", "type": "random", "name": "NU40-KIT-DISPLAY", "rssi": -40}]}
        with patch.object(self.controller, "request", return_value=json.dumps(peers).encode()) as request:
            self.assertEqual(self.controller.peers(), peers)
            request.assert_called_once_with("GET", "/v1/peers")
        for address_type in ("random", "public"):
            with patch.object(self.controller, "request", return_value=b'{"accepted":true}') as request:
                self.assertEqual(self.controller.select_peer("aa:bb:cc:dd:ee:ff", address_type), {"accepted": True})
                request.assert_called_once_with("POST", "/v1/peer", f"AA:BB:CC:DD:EE:FF {address_type}".encode())
        for address, address_type in (("NU40-KIT-DISPLAY", "random"), ("AA:BB:CC:DD:EE:FF\n", "random"),
                                      ("AA:BB:CC:DD:EE:FF", "unknown"), (None, "random")):
            with patch.object(self.controller, "request") as request:
                with self.assertRaises(ValueError):
                    self.controller.select_peer(address, address_type)
                request.assert_not_called()


class BindPolicyTests(unittest.TestCase):
    """Server construction is mocked: these tests never bind a LAN socket."""

    def run_main(self, arguments):
        with patch("lcd_bridge.ThreadingHTTPServer") as server, \
             patch.dict("os.environ", {"NU87_GATEWAY_TOKEN": "test-token-123456789"}), \
             patch("builtins.print"):
            main(arguments)
        return server

    def test_preview_defaults_to_loopback(self):
        server = self.run_main([])
        server.assert_called_once_with(("127.0.0.1", 8766), Handler)
        self.assertFalse(server.return_value.controller.serve_firmware_lan)

    def test_configured_gateway_still_binds_only_loopback(self):
        server = self.run_main(["--gateway", "192.168.1.147"])
        server.assert_called_once_with(("127.0.0.1", 8766), Handler)
        self.assertEqual(server.return_value.controller.gateway, "192.168.1.147")
        self.assertFalse(server.return_value.controller.serve_firmware_lan)

    def test_lan_firmware_listener_requires_explicit_opt_in(self):
        server = self.run_main(["--gateway", "192.168.1.147", "--serve-firmware-lan"])
        server.assert_called_once_with(("0.0.0.0", 8766), Handler)
        self.assertTrue(server.return_value.controller.serve_firmware_lan)

    def test_lan_opt_in_without_gateway_rejected_before_server_creation(self):
        with patch("lcd_bridge.ThreadingHTTPServer") as server, \
             patch("sys.stderr", new_callable=io.StringIO), \
             self.assertRaises(SystemExit):
            main(["--serve-firmware-lan"])
        server.assert_not_called()


class MockGatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        self.reply()

    def do_POST(self):
        self.reply()

    def reply(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append((self.command, self.path, dict(self.headers), body))
        code = self.server.result_code
        if self.path == "/v1/lcd":
            result = body[:4] + bytes((0, 3, 20, 4))
        elif self.path == "/v1/status":
            result = json.dumps(self.server.state).encode()
        elif self.path == "/v1/display":
            result = b'{"ready":true,"columns":21,"rows":4}'
        elif self.path == "/v1/peers":
            result = b'{"peers":[{"address":"AA:BB:CC:DD:EE:FF","type":"random","name":"NU40-KIT-DISPLAY","rssi":-40}]}'
        else:
            result = b'{}'
        if self.server.response_body is not None:
            result = self.server.response_body
        self.send_response(code)
        self.send_header("Content-Length", str(len(result)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(result)
        self.close_connection = True


class LoopbackHTTPTests(unittest.TestCase):
    def start_server(self, handler):
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        thread.start()
        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
        self.addCleanup(stop)
        return server

    def setUp(self):
        self.gateway = self.start_server(MockGatewayHandler)
        self.gateway.requests = []
        self.gateway.result_code = 200
        self.gateway.response_body = None
        self.gateway.state = {"configured": True, "connected": True}
        self.bridge = self.start_server(Handler)
        self.bridge.controller = Controller("127.0.0.1", "test-token-123456789", self.gateway.server_port)
        self.origin = f"http://127.0.0.1:{self.bridge.server_port}"

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.bridge.server_port, timeout=3)
        try:
            connection.request(method, path, body, headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def post(self, path, value, **headers):
        return self.request("POST", path, json.dumps(value).encode(),
                            {"Origin": self.origin, "X-LCD-Client": "1", **headers})

    def test_status_proxy_auth_and_preview(self):
        code, headers, body = self.request("GET", "/api/status")
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body), {"configured": True, "connected": True})
        self.assertEqual(self.gateway.requests[-1][2]["Authorization"], "Bearer test-token-123456789")
        self.assertNotIn(b"test-token", body)
        self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
        self.bridge.controller.gateway = None
        code, _, body = self.request("GET", "/api/status")
        self.assertEqual(code, 200)
        self.assertTrue(json.loads(body)["preview"])
        self.assertEqual(len(self.gateway.requests), 1)

    def test_csrf_rejects_wrong_origin_host_or_missing_custom_header(self):
        cases = ({"Origin": "http://evil.invalid", "X-LCD-Client": "1"},
                 {"Origin": "null", "X-LCD-Client": "1"},
                 {"Origin": self.origin},
                 {"Host": "evil.invalid", "X-LCD-Client": "1"})
        for headers in cases:
            with self.subTest(headers=headers):
                self.assertEqual(self.request("POST", "/api/power", b'{"on":true}', headers)[0], 403)
        self.assertEqual(self.gateway.requests, [])
        self.assertEqual(self.request("GET", "/api/status", headers={"Host": "evil.invalid"})[0], 403)

    def test_real_http_display_and_power_commands_receive_matching_acks(self):
        code, _, result = self.post("/api/display", {"text": "Hello\nNU40"})
        self.assertEqual(code, 200)
        self.assertTrue(json.loads(result)["ready"])
        self.assertEqual(self.gateway.requests[0][:2], ("GET", "/v1/status"))
        self.assertEqual(self.gateway.requests[1][3][1], 0)
        self.assertEqual(len(self.gateway.requests), 10)
        code, _, _ = self.post("/api/power", {"on": False})
        self.assertEqual(code, 200)
        self.assertEqual(self.gateway.requests[-1][3][1], 3)
        self.assertEqual(self.gateway.requests[-1][3][-1], 0)
        for bad in (1, "true", None):
            self.assertEqual(self.post("/api/power", {"on": bad})[0], 422)

    def test_bad_gateway_ack_status_and_large_response_do_not_report_success(self):
        self.gateway.response_body = bytes(8)
        self.assertEqual(self.post("/api/power", {"on": True})[0], 422)
        self.gateway.result_code = 503
        self.gateway.response_body = b"offline"
        self.assertEqual(self.request("GET", "/api/status")[0], 502)
        self.gateway.result_code = 200
        self.gateway.response_body = b"x" * 4097
        self.assertEqual(self.request("GET", "/api/status")[0], 502)

    def test_kit_http_text_forwarding_and_forbidden_controls(self):
        self.gateway.state.update({"protocol": "nu40-kit-display", "lcdReady": True,
                                   "columns": 21, "rows": 4,
                                   "capabilities": {"text": True, "power": False, "ota": False}})
        text = "NU87 -> NU40\nWi-Fi + BLE"
        code, _, body = self.post("/api/display", {"text": text})
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body), {"ready": True, "columns": 21, "rows": 4})
        self.assertEqual(self.gateway.requests[-1][0:2], ("POST", "/v1/display"))
        self.assertEqual(self.gateway.requests[-1][3], text.encode())
        self.bridge.controller.serve_firmware_lan = True
        manifest = self.bridge.controller.stage(make_package(image_bytes(), 1))
        for path, payload in (("/api/power", {"on": True}), ("/api/cancel", {}), ("/api/ota", {"id": manifest["sha256"]})):
            self.gateway.requests.clear()
            code, _, body = self.post(path, payload)
            self.assertEqual(code, 422)
            self.assertIn("지원하지 않습니다", json.loads(body)["error"])
            self.assertEqual([(req[0], req[1]) for req in self.gateway.requests], [("GET", "/v1/status")])

    def test_http_peer_search_readonly_and_explicit_selection_protected(self):
        code, _, body = self.request("GET", "/api/peers")
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)["peers"][0]["type"], "random")
        self.assertEqual(self.gateway.requests[-1][0:2], ("GET", "/v1/peers"))
        self.assertEqual(self.request("POST", "/api/peer", b'{"address":"AA:BB:CC:DD:EE:FF","type":"random"}')[0], 403)
        self.assertEqual(len(self.gateway.requests), 1)
        code, _, _ = self.post("/api/peer", {"address": "AA:BB:CC:DD:EE:FF", "type": "random"})
        self.assertEqual(code, 202)
        self.assertEqual(self.gateway.requests[-1][0:2], ("POST", "/v1/peer"))
        self.assertEqual(self.gateway.requests[-1][3], b"AA:BB:CC:DD:EE:FF random")

    def test_package_upload_then_ota_posts_the_exact_descriptor(self):
        self.bridge.controller.serve_firmware_lan = True
        package = make_package(image_bytes(), 0x01020304)
        code, _, body = self.request("POST", "/api/package", package,
                                     {"Origin": self.origin, "X-LCD-Client": "1"})
        self.assertEqual(code, 200)
        image_id = json.loads(body)["sha256"]
        code, _, body = self.post("/api/ota", {"id": image_id})
        self.assertEqual(code, 202)
        self.assertTrue(json.loads(body)["accepted"])
        method, path, headers, descriptor = self.gateway.requests[-1]
        self.assertEqual((method, path), ("POST", "/v1/ota"))
        self.assertEqual(struct.unpack_from("<4sIIHH", descriptor),
                         (b"LCD1", 64, 0x01020304, 0xEE75, self.bridge.server_port))
        self.assertEqual(descriptor[16:], image_id.encode())
        self.assertEqual(headers["Authorization"], "Bearer test-token-123456789")

    def test_default_ota_and_firmware_download_are_disabled(self):
        manifest = self.bridge.controller.stage(make_package(image_bytes(), 1))
        code, _, body = self.post("/api/ota", {"id": manifest["sha256"]})
        self.assertEqual(code, 422)
        self.assertIn("--serve-firmware-lan", json.loads(body)["error"])
        self.assertEqual(self.gateway.requests, [])
        route = f'/firmware/{manifest["sha256"]}.bin'
        code, _, body = self.request("GET", route, headers={"Range": "bytes=0-19"})
        self.assertEqual(code, 403)
        self.assertIn("disabled", json.loads(body)["error"])

    def test_invalid_package_upload_and_cancel_have_framed_http_results(self):
        for package in (b"not a zip", package_with(compression=zipfile.ZIP_DEFLATED)):
            with self.subTest(package_length=len(package)):
                code, _, body = self.request("POST", "/api/package", package,
                                             {"Origin": self.origin, "X-LCD-Client": "1"})
                self.assertEqual(code, 422)
                self.assertIn("error", json.loads(body))
        self.assertEqual(self.gateway.requests, [])
        self.assertEqual(self.post("/api/cancel", {})[0], 202)
        self.assertEqual(self.gateway.requests[-1][0:2], ("POST", "/v1/ota/cancel"))
        self.assertEqual(self.gateway.requests[-1][3], b"")

    def test_firmware_exact_finite_range_and_source_identity(self):
        self.bridge.controller.serve_firmware_lan = True
        image = image_bytes(8192)
        manifest = self.bridge.controller.stage(make_package(image, 1))
        path = f'/firmware/{manifest["sha256"]}.bin'
        code, headers, body = self.request("GET", path, headers={"Range": "bytes=0-4095"})
        self.assertEqual((code, body), (206, image[:4096]))
        self.assertEqual(headers, {"Content-Length": "4096", "Content-Range": "bytes 0-4095/8192", "Connection": "close"})
        code, headers, body = self.request("GET", path, headers={"Range": "bytes=8191-8191"})
        self.assertEqual((code, body), (206, image[-1:]))
        self.assertEqual(headers["Content-Range"], "bytes 8191-8191/8192")
        self.bridge.controller.gateway = "127.0.0.2"
        self.assertEqual(self.request("GET", path, headers={"Range": "bytes=0-19"})[0], 403)

    def test_firmware_rejects_missing_multi_suffix_oversize_and_out_of_range(self):
        self.bridge.controller.serve_firmware_lan = True
        manifest = self.bridge.controller.stage(make_package(image_bytes(8192), 1))
        path = f'/firmware/{manifest["sha256"]}.bin'
        for value in ("", "bytes=0-4096", "bytes=8192-8192", "bytes=2-1", "bytes=-20", "bytes=0-", "bytes=0-1,4-5"):
            with self.subTest(range=value):
                self.assertEqual(self.request("GET", path, headers={"Range": value})[0], 416)
        self.assertEqual(self.request("GET", path + "?extra=1", headers={"Range": "bytes=0-1"})[0], 404)
        self.assertEqual(self.request("GET", "/firmware/" + "0" * 64 + ".bin", headers={"Range": "bytes=0-1"})[0], 404)
        entry = self.bridge.controller.images[manifest["sha256"]]
        self.bridge.controller.images[manifest["sha256"]] = (entry[0], entry[1], time.monotonic() - 3601)
        self.assertEqual(self.request("GET", path, headers={"Range": "bytes=0-1"})[0], 404)

    def test_duplicate_lengths_and_transfer_encoding_are_rejected_before_body(self):
        for extra_headers in (("Content-Length", "2"), ("Transfer-Encoding", "chunked")):
            with self.subTest(header=extra_headers):
                connection = http.client.HTTPConnection("127.0.0.1", self.bridge.server_port, timeout=3)
                try:
                    connection.putrequest("POST", "/api/cancel")
                    connection.putheader("X-LCD-Client", "1")
                    connection.putheader("Origin", self.origin)
                    connection.putheader("Content-Length", "2")
                    connection.putheader(*extra_headers)
                    connection.endheaders(b"{}")
                    response = connection.getresponse()
                    self.assertEqual(response.status, 422)
                    response.read()
                finally:
                    connection.close()
        self.assertEqual(self.gateway.requests, [])


if __name__ == "__main__":
    unittest.main()

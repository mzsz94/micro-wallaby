"""Startup helper tests: synthetic serial only, never USB or real credentials."""

import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock


spec = importlib.util.spec_from_file_location("gateway_connect", Path(__file__).resolve().parents[1] / "connect.py")
connect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(connect)


def port(serial="unit123", vid=0x10C4, pid=0xEA60, device="/dev/cu.unit"):
    return SimpleNamespace(serial_number=serial, vid=vid, pid=pid, device=device)


class ConnectTests(unittest.TestCase):
    def test_parse_ready_requires_explicit_private_address_message(self):
        self.assertEqual(connect.parse_ready("[00:01] gateway_wifi: Open http://192.168.10.9:8080/ on a device connected to the same network"), ("192.168.10.9", 8080))
        self.assertEqual(connect.parse_ready("NU87 Wi-Fi LCD gateway: http://10.0.0.8:8080/"), ("10.0.0.8", 8080))
        for address in ("127.0.0.1", "169.254.3.4", "8.8.8.8", "224.0.0.1", "0.0.0.0", "192.168.1.255", "192.168.1.0", "999.0.0.1"):
            self.assertIsNone(connect.parse_ready(f"Open http://{address}:8080/"))
        for text in ("peer IPv4=192.168.10.9", "Open http://192.168.10.9:65536/", "Open http://192.168.10.9:0/"):
            self.assertIsNone(connect.parse_ready(text))

    def test_lines_handle_split_utf8_crlf_and_long_line(self):
        lines = connect.Lines()
        encoded = "연결\r\nnext\n".encode()
        self.assertEqual(lines.feed(encoded[:2]), [])
        self.assertEqual(lines.feed(encoded[2:]), ["연결", "next"])
        self.assertEqual(lines.feed(b"x" * (connect.MAX_LINE + 3)), [])
        self.assertEqual(lines.feed(b"secret\nready\n"), ["[overlong serial line suppressed]", "ready"])

    def observe(self, data, *, acknowledge=True, ports=None):
        handle = Mock()
        handle.read.side_effect = data + [b""] * 10
        handle.write.return_value = 1
        factory = Mock(return_value=handle)
        output = io.StringIO()
        ticks = iter(range(100))
        kwargs = dict(serial_number="unit123", timeout=10, acknowledge=acknowledge,
                      redactions=("dummy-password", "example-token"), ports=ports or [port()],
                      serial_factory=factory, output=output, clock=lambda: next(ticks))
        return handle, factory, output, kwargs

    def test_exact_prompt_after_split_and_acknowledges_only_once(self):
        prompt = connect.EFUSE_PROMPT.encode()
        handle, factory, output, kwargs = self.observe([
            prompt[:31], prompt[31:] + b"\r\n" + prompt + b"\n",
            b"password=dummy-", b"password\n\x1b[31mexample-token\x1b[0m\n",
            b"Open http://192.168.1.8:8080/ on a device connected to the same network\n"])
        self.assertEqual(connect.monitor(**kwargs), ("192.168.1.8", 8080))
        handle.write.assert_called_once_with(b"\n")
        factory.assert_called_once_with(port=None, baudrate=1500000, timeout=0.2, write_timeout=1)
        self.assertFalse(handle.dtr)
        self.assertFalse(handle.rts)
        handle.close.assert_called_once()
        self.assertNotIn("dummy-password", output.getvalue())
        self.assertNotIn("example-token", output.getvalue())

    def test_no_ack_without_flag_or_complete_prompt(self):
        for acknowledge, data in ((False, connect.EFUSE_PROMPT), (True, "Efuse empty!")):
            handle, _, _, kwargs = self.observe([data.encode() + b"\n"], acknowledge=acknowledge)
            with self.assertRaises(connect.ConnectionError):
                connect.monitor(**kwargs)
            handle.write.assert_not_called()
            handle.close.assert_called_once()

    def test_warning_retry_requires_fresh_boot_and_respects_total_limit(self):
        prompt = connect.EFUSE_PROMPT.encode() + b"\n"
        boot = b"[BOOT-I] IMG1 ENTER ROMSUB:2\n"
        data = [boot + prompt + prompt, boot + prompt + prompt,
                boot + prompt, b"Open http://192.168.1.8:8080/ ready\n"]
        for limit in (1, 2, 3):
            handle, _, output, kwargs = self.observe(data)
            kwargs["max_boot_acknowledgements"] = limit
            self.assertEqual(connect.monitor(**kwargs), ("192.168.1.8", 8080))
            self.assertEqual(handle.write.call_count, limit)
            self.assertTrue(all(call.args == (b"\n",) for call in handle.write.call_args_list))
            self.assertIn(f"({limit}/{limit})", output.getvalue())

    def test_invalid_warning_retry_limit_rejected_before_open(self):
        for limit in (0, 4, True, 1.5):
            _, factory, _, kwargs = self.observe([])
            kwargs["max_boot_acknowledgements"] = limit
            with self.assertRaises(connect.ConnectionError):
                connect.monitor(**kwargs)
            factory.assert_not_called()

    def test_firmware_halt_stops_observation_and_closes_port(self):
        handle, _, output, kwargs = self.observe([
            b"[00:00:41] <err> os: >>> ZEPHYR FATAL ERROR 4: Kernel panic\n",
            b"[00:00:41] <err> os: Halting sys", b"tem\n",
            b"Open http://192.168.1.8:8080/ on a device connected to the same network\n"])
        with self.assertRaisesRegex(connect.ConnectionError, "firmware halted"):
            connect.monitor(**kwargs)
        self.assertIn("ZEPHYR FATAL ERROR", output.getvalue())
        self.assertEqual(handle.read.call_count, 3)
        handle.write.assert_not_called()
        handle.close.assert_called_once()

    def test_wrong_ambiguous_or_non_cp2102_port_never_opened(self):
        for ports in ([port(serial="other")], [port(vid=0x1234)], [port(), port(device="/dev/cu.second")], [port(device="/dev/tty.unit")]):
            _, factory, _, kwargs = self.observe([], ports=ports)
            with self.assertRaises(connect.ConnectionError):
                connect.monitor(**kwargs)
            factory.assert_not_called()

    def test_linux_device_requires_exact_bridge_serial_and_anchored_path(self):
        handle, factory, _, kwargs = self.observe([
            b"Open http://192.168.1.8:8080/ on the same network\n"],
            ports=[port(device="/dev/ttyUSB0"), port(serial="other", device="/dev/ttyUSB1")])
        self.assertEqual(connect.monitor(**kwargs), ("192.168.1.8", 8080))
        self.assertEqual(handle.port, "/dev/ttyUSB0")
        handle.close.assert_called_once()
        for name in ("/dev/ttyUSB", "/dev/ttyUSB0extra", "/dev/ttyACM0"):
            _, factory, _, kwargs = self.observe([], ports=[port(device=name)])
            with self.assertRaises(connect.ConnectionError):
                connect.monitor(**kwargs)
            factory.assert_not_called()

    def test_timeout_is_finite_and_bounded_before_open(self):
        for timeout in (float("inf"), float("nan"), 0, -1, 601):
            _, factory, _, kwargs = self.observe([])
            kwargs["timeout"] = timeout
            with self.assertRaises(connect.ConnectionError):
                connect.monitor(**kwargs)
            factory.assert_not_called()

    def test_private_config_redactions_and_state_have_no_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "local.conf"
            path.write_text('CONFIG_GATEWAY_WIFI_PSK="dummy-password"\nCONFIG_GATEWAY_API_TOKEN="dummy-token"\n')
            self.assertEqual(connect.read_redactions(path), ("dummy-password", "dummy-token"))
            state_path = Path(directory) / "build/gateway-connection.json"
            connect.save_state(state_path, serial_number="unit123", address="192.168.1.8", port=8080)
            state = json.loads(state_path.read_text())
            self.assertEqual(state["ipv4"], "192.168.1.8")
            self.assertEqual(state["serial_number"], "unit123")
            self.assertIn("observed_at", state)
            self.assertNotIn("dummy", state_path.read_text())
            self.assertEqual(state_path.stat().st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()

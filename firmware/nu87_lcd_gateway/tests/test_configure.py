"""Private Wi-Fi provisioning tests; no USB, network, or real credentials."""

import contextlib
import importlib.util
import io
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("gateway_configure", Path(__file__).resolve().parents[1] / "configure.py")
configure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(configure)


class ConfigureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "local.conf"

    def write(self, **overrides):
        options = dict(ssid="example-test-ap", security="wpa2", psk="dummy-password", test_network=True)
        options.update(overrides)
        configure.configure(self.path, **options)

    def test_wpa2_private_file_and_random_token(self):
        self.write()
        content = self.path.read_text()
        self.assertIn("CONFIG_GATEWAY_WIFI_SECURITY_PSK=y\n", content)
        self.assertIn("CONFIG_GATEWAY_WIFI_SECURITY_OPEN=n\n", content)
        self.assertIn("CONFIG_GATEWAY_ALLOW_INSECURE_TEST_PSK=y\n", content)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        self.assertRegex(configure.read_api_token(self.path), r"^[0-9a-f]{64}$")

    def test_preserves_ble_identities_unrelated_settings_and_token(self):
        original = ('# Keep peer address independent of Wi-Fi\n'
                    'CONFIG_GATEWAY_PEER_ADDRESS="AA:BB:CC:DD:EE:FF"\n'
                    'CONFIG_GATEWAY_DFU_ADDRESS="AA:BB:CC:DD:EE:FE"\n'
                    'CONFIG_GATEWAY_HTTP_PORT=8080\n'
                    'CONFIG_GATEWAY_API_TOKEN="existing-test-token-1234"\n')
        self.path.write_text(original)
        self.write()
        self.assertTrue(self.path.read_text().startswith(original.rsplit("CONFIG_GATEWAY_API_TOKEN", 1)[0]))
        self.assertEqual(configure.read_api_token(self.path), "existing-test-token-1234")
        self.write(ssid="second-test-ap")
        self.assertEqual(configure.read_api_token(self.path), "existing-test-token-1234")

    def test_replaces_duplicate_settings_and_unsets(self):
        self.path.write_text('CONFIG_GATEWAY_WIFI_SSID="old"\n'
                             'CONFIG_GATEWAY_WIFI_SSID="older"\n'
                             '# CONFIG_GATEWAY_WIFI_SECURITY_PSK is not set\n')
        self.write()
        content = self.path.read_text()
        self.assertEqual(content.count("CONFIG_GATEWAY_WIFI_SSID="), 1)
        self.assertNotIn("is not set", content)

    def test_unicode_ssid_byte_limit(self):
        self.write(ssid="가" * 10 + "ab")
        previous = self.path.read_bytes()
        with self.assertRaises(configure.ConfigurationError):
            self.write(ssid="가" * 11)
        self.assertEqual(self.path.read_bytes(), previous)

    def test_kconfig_quoting_round_trip(self):
        ssid = 'test"\\network'
        password = 'dummy"\\ password'
        self.write(ssid=ssid, psk=password)
        text = self.path.read_text()
        for key, value in (("CONFIG_GATEWAY_WIFI_SSID", ssid), ("CONFIG_GATEWAY_WIFI_PSK", password)):
            self.assertEqual(configure._decode_string(configure._setting(text, key)), value)
        self.assertEqual(len(text.splitlines()), 7)

    def test_injected_newline_is_rejected_before_write(self):
        for key, value in (("ssid", 'test\nCONFIG_FAKE=y'), ("psk", 'test\nCONFIG_FAKE=y')):
            with self.subTest(key=key), self.assertRaises(configure.ConfigurationError):
                self.write(**{key: value})
            self.assertFalse(self.path.exists())

    def test_wpa2_requires_explicit_test_network(self):
        with self.assertRaises(configure.ConfigurationError):
            self.write(test_network=False)
        self.assertFalse(self.path.exists())

    def test_password_boundaries_and_printable_ascii(self):
        for password in ("a" * 8, "b" * 63):
            self.write(psk=password)
        previous = self.path.read_bytes()
        for password in ("", "a" * 7, "0" * 64, "가" * 8, "ab\tdefgh", "a" * 7 + "\x7f"):
            with self.subTest(length=len(password)), self.assertRaises(configure.ConfigurationError):
                self.write(psk=password)
            self.assertEqual(self.path.read_bytes(), previous)

    def test_open_clears_stale_password_and_gate(self):
        self.write()
        token = configure.read_api_token(self.path)
        self.write(security="open", psk="", test_network=False)
        content = self.path.read_text()
        self.assertIn('CONFIG_GATEWAY_WIFI_PSK=""\n', content)
        self.assertIn("CONFIG_GATEWAY_WIFI_SECURITY_OPEN=y\n", content)
        self.assertIn("CONFIG_GATEWAY_ALLOW_INSECURE_TEST_PSK=n\n", content)
        self.assertNotIn("dummy-password", content)
        self.assertEqual(configure.read_api_token(self.path), token)

    def test_invalid_existing_token_does_not_overwrite_config(self):
        original = 'CONFIG_GATEWAY_API_TOKEN="too short"\n'
        self.path.write_text(original)
        with self.assertRaises(configure.ConfigurationError):
            self.write()
        self.assertEqual(self.path.read_text(), original)

    def test_missing_or_empty_token_is_generated(self):
        self.path.write_text('CONFIG_GATEWAY_API_TOKEN=""\n')
        self.write()
        self.assertEqual(len(configure.read_api_token(self.path)), 64)

    def test_symlink_is_rejected(self):
        target = self.path.with_name("elsewhere.conf")
        target.write_text("untouched")
        self.path.symlink_to(target)
        with self.assertRaises(configure.ConfigurationError):
            self.write()
        self.assertEqual(target.read_text(), "untouched")

    def test_atomic_replace_failure_preserves_original_and_removes_temporary(self):
        original = '# Existing configuration\n'
        self.path.write_text(original)
        with patch.object(configure.os, "replace", side_effect=OSError("simulated")):
            with self.assertRaises(OSError):
                self.write()
        self.assertEqual(self.path.read_text(), original)
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

    def test_stdin_secret_never_appears_in_output(self):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = configure.main(["--ssid", "test-ap", "--security", "wpa2", "--test-network", "--psk-stdin"],
                                  config_path=self.path, stdin=io.StringIO("dummy-test-secret\n"))
        self.assertEqual(code, 0)
        self.assertNotIn("dummy-test-secret", output.getvalue() + errors.getvalue())
        self.assertNotIn(configure.read_api_token(self.path), output.getvalue() + errors.getvalue())

    def test_invalid_stdin_secret_is_not_echoed(self):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = configure.main(["--ssid", "test-ap", "--security", "wpa2", "--test-network", "--psk-stdin"],
                                  config_path=self.path, stdin=io.StringIO("SECRET1\nSECOND\n"))
        self.assertEqual(code, 1)
        self.assertFalse(self.path.exists())
        self.assertNotIn("SECRET1", output.getvalue() + errors.getvalue())

    def test_noninteractive_without_stdin_option_does_not_fallback_to_echo(self):
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors), patch.object(configure.getpass, "getpass") as hidden_prompt:
            code = configure.main(["--ssid", "test-ap", "--security", "wpa2", "--test-network"],
                                  config_path=self.path, stdin=io.StringIO("dummy-test-secret"))
        self.assertEqual(code, 1)
        hidden_prompt.assert_not_called()


if __name__ == "__main__":
    unittest.main()

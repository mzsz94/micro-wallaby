#!/usr/bin/env python3
"""Write private NU87 gateway Wi-Fi settings without putting secrets in argv.

Run interactively for a hidden password prompt, or use --psk-stdin with a pipe.
The configured SSID must be the same network used by the Mac. WPA2 is currently
limited to an explicitly acknowledged test network because the pinned AmebaD
port does not provide a verified cryptographic entropy source.
"""

from __future__ import annotations

import argparse
import getpass
import os
from pathlib import Path
import re
import secrets
import sys
import tempfile
from typing import TextIO


DEFAULT_CONFIG = Path(__file__).resolve().with_name("local.conf")
_ASSIGNMENT = re.compile(r"^\s*(CONFIG_[A-Z0-9_]+)\s*=\s*(.*?)\s*$")
_UNSET = re.compile(r"^\s*#\s*(CONFIG_[A-Z0-9_]+) is not set\s*$")
_STRING = re.compile(r'"(?:[^"\\]|\\["\\])*"\Z')


class ConfigurationError(ValueError):
    """Validation failure with a safe message that never includes a secret."""


def quote_kconfig(value: str) -> str:
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise ConfigurationError("Configuration strings cannot contain control characters.")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _decode_string(value: str) -> str:
    if not _STRING.fullmatch(value):
        raise ConfigurationError("Existing API token is not a valid quoted Kconfig string.")
    return re.sub(r'\\(["\\])', r"\1", value[1:-1])


def _read_config(config_path: Path) -> str:
    if config_path.is_symlink():
        raise ConfigurationError("Refusing to replace a symbolic-link configuration file.")
    try:
        return config_path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return ""
    except UnicodeError:
        raise ConfigurationError("Existing configuration is not valid UTF-8.") from None


def _setting(text: str, key: str) -> str | None:
    value = None
    for line in text.splitlines():
        match = _ASSIGNMENT.fullmatch(line)
        if match and match[1] == key:
            value = match[2]
        unset = _UNSET.fullmatch(line)
        if unset and unset[1] == key:
            value = None
    return value


def _validate_token(token: str) -> None:
    if not 16 <= len(token) <= 128 or any(not 33 <= ord(char) <= 126 for char in token):
        raise ConfigurationError("Existing API token must contain 16–128 printable ASCII characters without spaces.")


def read_api_token(config_path: Path = DEFAULT_CONFIG) -> str:
    """Read a configured bearer token for the local bridge; never print it."""
    value = _setting(_read_config(Path(config_path)), "CONFIG_GATEWAY_API_TOKEN")
    if value is None:
        raise ConfigurationError("No gateway API token is configured.")
    token = _decode_string(value)
    _validate_token(token)
    return token


def _replace_settings(text: str, updates: dict[str, str]) -> str:
    # Preserve comments, BLE identities and unrelated settings exactly. Remove
    # every old occurrence of changed keys so Kconfig cannot apply a stale value.
    lines = []
    for line in text.splitlines(keepends=True):
        match = _ASSIGNMENT.fullmatch(line.rstrip("\r\n")) or _UNSET.fullmatch(line.rstrip("\r\n"))
        if not match or match[1] not in updates:
            lines.append(line)
    result = "".join(lines)
    if result and not result.endswith("\n"):
        result += "\n"
    if not result:
        result = "# Private gateway configuration; do not commit this file.\n"
    return result + "".join(f"{key}={value}\n" for key, value in updates.items())


def _atomic_private_write(config_path: Path, content: str) -> None:
    descriptor, temporary_name = tempfile.mkstemp(prefix=".local-conf-", dir=config_path.parent)
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            os.fchmod(output.fileno(), 0o600)
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, config_path)
    finally:
        temporary_path.unlink(missing_ok=True)


def configure(config_path: Path, *, ssid: str, security: str, psk: str = "", test_network: bool = False) -> None:
    """Validate all input before replacing the private configuration atomically."""
    config_path = Path(config_path)
    try:
        ssid_length = len(ssid.encode("utf-8"))
    except UnicodeError:
        raise ConfigurationError("Wi-Fi SSID must be valid UTF-8.") from None
    if not 1 <= ssid_length <= 32:
        raise ConfigurationError("Wi-Fi SSID must contain 1–32 UTF-8 bytes.")
    quoted_ssid = quote_kconfig(ssid)
    if security not in ("open", "wpa2"):
        raise ConfigurationError("Wi-Fi security must be open or wpa2.")
    if security == "wpa2":
        if not test_network:
            raise ConfigurationError("WPA2 requires --test-network: this AmebaD port has unverified cryptographic entropy.")
        # The gateway/vendor path accepts passphrases; do not assume a 64-hex
        # raw PSK has the same semantics across its pinned driver versions.
        if not 8 <= len(psk) <= 63 or any(not 32 <= ord(char) <= 126 for char in psk):
            raise ConfigurationError("WPA2 password must contain 8–63 printable ASCII characters.")
    elif psk:
        raise ConfigurationError("An open network must not have a Wi-Fi password.")

    current = _read_config(config_path)
    token_value = _setting(current, "CONFIG_GATEWAY_API_TOKEN")
    token = _decode_string(token_value) if token_value is not None else ""
    if token:
        _validate_token(token)
    else:
        token = secrets.token_hex(32)
    updates = {
        "CONFIG_GATEWAY_WIFI_SSID": quoted_ssid,
        "CONFIG_GATEWAY_WIFI_SECURITY_OPEN": "y" if security == "open" else "n",
        "CONFIG_GATEWAY_WIFI_SECURITY_PSK": "y" if security == "wpa2" else "n",
        "CONFIG_GATEWAY_WIFI_PSK": quote_kconfig(psk),
        "CONFIG_GATEWAY_ALLOW_INSECURE_TEST_PSK": "y" if security == "wpa2" else "n",
        "CONFIG_GATEWAY_API_TOKEN": quote_kconfig(token),
    }
    _atomic_private_write(config_path, _replace_settings(current, updates))


def _stdin_password(stream: TextIO) -> str:
    if stream.isatty():
        raise ConfigurationError("Use the hidden password prompt for an interactive terminal; --psk-stdin requires a pipe.")
    value = stream.read(257)
    if len(value) > 256:
        raise ConfigurationError("Password input is too long.")
    if value.endswith("\r\n"):
        value = value[:-2]
    elif value.endswith("\n"):
        value = value[:-1]
    return value


def main(argv: list[str] | None = None, *, config_path: Path = DEFAULT_CONFIG, stdin: TextIO | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssid", required=True, help="Network name used by the Mac (1–32 UTF-8 bytes).")
    parser.add_argument("--security", required=True, choices=("wpa2", "open"))
    parser.add_argument("--test-network", action="store_true", help="Acknowledge unverified AmebaD entropy; use an isolated test AP and disposable credentials.")
    parser.add_argument("--psk-stdin", action="store_true", help="Read WPA2 password from a pipe, never from command-line arguments.")
    args = parser.parse_args(argv)
    stream = stdin if stdin is not None else sys.stdin
    try:
        if args.security == "open" and args.psk_stdin:
            raise ConfigurationError("--psk-stdin applies only to WPA2 networks.")
        if args.security == "wpa2" and not args.test_network:
            raise ConfigurationError("WPA2 requires --test-network: this AmebaD port has unverified cryptographic entropy.")
        password = ""
        if args.security == "wpa2":
            if args.psk_stdin:
                password = _stdin_password(stream)
            elif stream.isatty():
                password = getpass.getpass("Disposable test Wi-Fi password: ")
            else:
                raise ConfigurationError("Use --psk-stdin with a pipe, or run in a terminal for a hidden password prompt.")
        configure(config_path, ssid=args.ssid, security=args.security, psk=password, test_network=args.test_network)
    except (ConfigurationError, OSError, EOFError) as error:
        # OS errors can contain paths, so do not echo their arguments either.
        message = str(error) if isinstance(error, ConfigurationError) else "Could not read input or write private configuration."
        print(f"Configuration error: {message}", file=sys.stderr)
        return 1
    print("Private gateway configuration saved (permissions 0600). Rebuild and flash to apply.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

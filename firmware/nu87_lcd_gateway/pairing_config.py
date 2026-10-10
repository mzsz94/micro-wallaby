#!/usr/bin/env python3
"""Create a private build-only Kconfig fragment from a shared pairing file."""
import argparse
import os
from pathlib import Path
import re
import stat


def write_pairing_config(source: Path, destination: Path) -> None:
    try:
        if source.is_symlink() or not source.is_file():
            raise ValueError("Pairing file must be a regular local file.")
        if stat.S_IMODE(source.stat().st_mode) & 0o077:
            raise ValueError("Pairing file must be private (chmod 600).")
        value = source.read_text(encoding="ascii").strip()
        if re.fullmatch(r"[0-9]{6}", value) is None:
            raise ValueError("Pairing file must contain exactly six decimal digits.")
        destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w") as handle:
            os.fchmod(handle.fileno(), 0o600)
            handle.write('CONFIG_GATEWAY_NU40_PASSKEY="' + value + '"\n')
    except (OSError, UnicodeError):
        raise ValueError("Cannot read the private pairing file or write the build configuration.") from None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairing-code-file", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        write_pairing_config(args.pairing_code_file, args.output)
    except ValueError as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

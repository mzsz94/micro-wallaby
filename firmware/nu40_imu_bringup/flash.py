#!/usr/bin/env python3
"""Validate and optionally flash a NU40 image matching its bootloader layout."""
import argparse
import datetime
import hashlib
from pathlib import Path
import struct
import subprocess

APP_END = 0xDC000
FAMILY = 0xADA52840
LAYOUTS = {
    "baram": {
        "start": 0x1000,
        "volume": "/Volumes/BARAM-NU40",
        "board": "Board-ID: BARAM-NU40",
        "softdevice": "SoftDevice: not found",
    },
    "legacy": {
        "start": 0x26000,
        "volume": "/Volumes/NRF52BOOT",
        "board": "Board-ID: nRF52840-pca10056-v1",
        "softdevice": "SoftDevice: S140 6.1.1",
    },
}


def verify(path, layout="baram"):
    app_start = LAYOUTS[layout]["start"]
    data = path.read_bytes()
    if not data or len(data) % 512:
        raise ValueError("Invalid UF2 length")
    count = len(data) // 512
    numbers, memory = set(), {}
    for offset in range(0, len(data), 512):
        block = data[offset:offset + 512]
        magic0, magic1, flags, address, size, number, total, family = struct.unpack_from("<8I", block)
        if (magic0, magic1, struct.unpack_from("<I", block, 508)[0]) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise ValueError("Invalid UF2 magic")
        if flags != 0x2000 or family != FAMILY:
            raise ValueError("Not a normal nRF52840 flash UF2 block")
        if size != 256 or address % 256 or total != count or number >= count or number in numbers:
            raise ValueError("Invalid UF2 block layout")
        if not app_start <= address < address + size <= APP_END:
            raise ValueError("UF2 would touch MBR/SoftDevice, calibration storage, or bootloader")
        if address in memory:
            raise ValueError("Overlapping UF2 payload")
        numbers.add(number)
        memory[address] = block[32:32 + size]
    if app_start not in memory:
        raise ValueError(f"Missing application vector at 0x{app_start:x}")
    stack, reset = struct.unpack_from("<2I", memory[app_start])
    if not 0x20000000 < stack <= 0x20040000 or stack % 8 or not reset & 1:
        raise ValueError("Invalid Cortex-M application vectors")
    if (reset & ~1) // 256 * 256 not in memory:
        raise ValueError("Reset handler is outside this image")
    start, end = min(memory), max(memory) + 256
    if len(memory) * 256 != end - start:
        raise ValueError("Unexpected gaps in application image")
    digest = hashlib.sha256(data).hexdigest()
    print(f"UF2 OK ({layout}): {count} blocks, 0x{start:x}..0x{end:x}, sha256={digest}")
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--layout", choices=LAYOUTS, default="baram")
    parser.add_argument("--volume", type=Path,
                        help="UF2 volume (defaults to the selected layout's volume)")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--write", action="store_true")
    action.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    layout = LAYOUTS[args.layout]
    volume = args.volume or Path(layout["volume"])
    digest = verify(args.image, args.layout)
    if args.verify_only:
        return
    info = (volume / "INFO_UF2.TXT").read_text()
    if layout["board"] not in info.splitlines() or layout["softdevice"] not in info.splitlines():
        raise ValueError(f"Expected {layout['board']} and {layout['softdevice']}; "
                         "refusing a different bootloader")
    print(info.strip())
    if not args.write:
        print("Dry run passed; board was not written.")
        return
    root = Path(__file__).resolve().parents[2]
    backup = root / "backups" / (f"nu40-imu-{args.layout}-" +
                                datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    backup.mkdir(parents=True, exist_ok=False)
    current = volume / "CURRENT.UF2"
    if not current.is_file():
        raise ValueError("CURRENT.UF2 missing; cannot back up existing firmware")
    subprocess.run(["cp", "-X", str(current), str(backup / "CURRENT.UF2")], check=True)
    (backup / "INFO_UF2.TXT").write_text(info)
    (backup / "layout.txt").write_text(args.layout + "\n")
    (backup / "flashed.sha256").write_text(digest + "  " + str(args.image.resolve()) + "\n")
    print(f"Backup: {backup}", flush=True)
    result = subprocess.run(["cp", "-X", str(args.image), str(volume / "FIRMWARE.UF2")])
    if result.returncode:
        raise SystemExit("Copy returned an error (the UF2 board may have rebooted during close). "
                         "Flashing is NOT yet confirmed: read USB logs and verify the new build before retrying.")
    subprocess.run(["sync"], check=True)
    print("UF2 copied. Verify the new firmware's USB serial output.")


if __name__ == "__main__":
    main()

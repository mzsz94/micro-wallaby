"""Application-only package format for the NU40 S140 6.1.1 legacy DFU path."""
from __future__ import annotations
import argparse
import binascii
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile

APP_START, APP_END = 0x26000, 0xDC000
MAX_IMAGE = APP_END - APP_START
MAX_PACKAGE = MAX_IMAGE + 16384
TARGET = "nu40-s140-6.1.1-lcd"


def image_metadata(image: bytes, version: int) -> dict:
    if type(version) is not int or not 0 <= version <= 0xFFFFFFFF:
        raise ValueError("버전은 0~4294967295 정수여야 합니다.")
    if not 8 <= len(image) <= MAX_IMAGE or len(image) % 4:
        raise ValueError("NU40 앱 크기 또는 4바이트 정렬이 잘못됐습니다.")
    sp, reset = struct.unpack_from("<II", image)
    if not 0x20000000 < sp <= 0x20040000 or sp % 8:
        raise ValueError("nRF52840 스택 주소가 잘못됐습니다.")
    if not reset & 1 or not APP_START <= (reset & ~1) < APP_START + len(image):
        raise ValueError("0x26000에서 실행되는 NU40 앱만 지원합니다.")
    return {"format": 1, "target": TARGET, "address": APP_START, "size": len(image),
            "version": version, "sdFwid": 0xB6, "crc16": binascii.crc_hqx(image, 0xFFFF),
            "sha256": hashlib.sha256(image).hexdigest()}


def validate_package(data: bytes) -> tuple[dict, bytes]:
    if len(data) > MAX_PACKAGE:
        raise ValueError("업데이트 파일이 너무 큽니다.")
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            items = archive.infolist()
            if len(items) != 2 or {i.filename for i in items} != {"manifest.json", "application.bin"}:
                raise ValueError("manifest.json과 application.bin만 포함해야 합니다.")
            for item in items:
                limit = MAX_IMAGE if item.filename == "application.bin" else 2048
                if item.file_size > limit or item.flag_bits & 1 or item.compress_type != zipfile.ZIP_STORED:
                    raise ValueError("압축 파일 크기 또는 암호화 형식이 잘못됐습니다.")
            manifest = json.loads(archive.read("manifest.json"))
            image = archive.read("application.bin")
        expected = image_metadata(image, manifest["version"])
        if manifest != expected:
            raise ValueError("대상 보드, 주소, 크기 또는 파일 해시가 일치하지 않습니다.")
        return expected, image
    except (zipfile.BadZipFile, KeyError, TypeError, json.JSONDecodeError) as error:
        raise ValueError("올바른 NU40 LCD 업데이트 패키지가 아닙니다.") from error


def make_package(image: bytes, version: int) -> bytes:
    manifest = image_metadata(image, version)
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("manifest.json", json.dumps(manifest, sort_keys=True))
        archive.writestr("application.bin", image)
    return output.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--version", required=True, type=int)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    config = (args.build / "zephyr/.config").read_text()
    if "CONFIG_LCD_LEGACY_DFU=y\n" not in config:
        parser.error("검증한 레거시 부트로더용 CONFIG_LCD_LEGACY_DFU=y 빌드가 필요합니다.")
    image = (args.build / "zephyr/zephyr.bin").read_bytes()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(make_package(image, args.version))
    print(json.dumps(image_metadata(image, args.version), indent=2))


if __name__ == "__main__":
    main()

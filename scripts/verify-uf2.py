#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

MAGIC_START0 = 0x0A324655
MAGIC_START1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30
FLAG_FAMILY_ID_PRESENT = 0x00002000
NRF52840_FAMILY_ID = 0xADA52840
APP_START = 0x00026000
APP_END = 0x000ED000


def fail(message: str) -> None:
    print(f"UF2 검증 실패: {message}", file=sys.stderr)
    raise SystemExit(1)


if len(sys.argv) != 2:
    fail("사용법: verify-uf2.py <firmware.uf2>")

path = Path(sys.argv[1])
data = path.read_bytes()
if not data or len(data) % 512:
    fail("파일 크기가 512바이트 블록 단위가 아닙니다")

addresses = []
block_numbers = set()
expected_blocks = len(data) // 512
for offset in range(0, len(data), 512):
    block = data[offset : offset + 512]
    (
        magic0,
        magic1,
        flags,
        address,
        payload_size,
        block_number,
        block_count,
        family_id,
    ) = struct.unpack_from("<IIIIIIII", block)
    magic_end = struct.unpack_from("<I", block, 508)[0]
    if (magic0, magic1, magic_end) != (MAGIC_START0, MAGIC_START1, MAGIC_END):
        fail(f"블록 {offset // 512}의 매직 값이 올바르지 않습니다")
    if not 0 < payload_size <= 476:
        fail(f"블록 {block_number}의 payload 크기가 올바르지 않습니다")
    if block_count != expected_blocks:
        fail(f"블록 수 필드 {block_count}가 실제 {expected_blocks}와 다릅니다")
    if not flags & FLAG_FAMILY_ID_PRESENT or family_id != NRF52840_FAMILY_ID:
        fail(f"블록 {block_number}가 nRF52840 UF2가 아닙니다")
    if block_number in block_numbers:
        fail(f"중복 블록 번호가 있습니다: {block_number}")
    block_numbers.add(block_number)
    addresses.append((address, address + payload_size))

if block_numbers != set(range(expected_blocks)):
    fail("UF2 블록 번호가 연속적이지 않습니다")

image_start = min(start for start, _ in addresses)
image_end = max(end for _, end in addresses)
if image_start < APP_START or image_end > APP_END:
    fail(
        f"이미지 범위 0x{image_start:08x}-0x{image_end:08x}가 "
        f"legacy NU40 앱 영역 0x{APP_START:08x}-0x{APP_END:08x} 밖입니다"
    )

print(
    f"UF2 검증 완료: nRF52840, {expected_blocks} blocks, "
    f"0x{image_start:08x}-0x{image_end:08x}"
)

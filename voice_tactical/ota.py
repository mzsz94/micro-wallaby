"""Explicit MCUboot OTA over the NU87 Wi-Fi → BLE SMP relay.

Run ``python -m voice_tactical.ota --help``. Upload, test, reset and confirm are
separate commands. This client never installs firmware when the UI starts.

Protocol references:
https://docs.zephyrproject.org/latest/services/device_mgmt/smp_protocol.html
https://docs.zephyrproject.org/latest/services/device_mgmt/smp_groups/smp_group_1.html
https://docs.mcuboot.com/design.html#image-format
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from typing import Callable

from .gateway import Gateway

MAX_PACKET = 65535 + 8
MAX_IMAGE = 2 * 1024 * 1024
HEADER = struct.Struct(">BBHHBB")


class OTAError(ValueError):
    """A response or image could not be safely accepted."""


def _head(major: int, value: int) -> bytes:
    if not 0 <= value <= 0xffffffffffffffff:
        raise OTAError("CBOR integer is out of range.")
    if value < 24:
        return bytes([(major << 5) | value])
    for marker, width in ((24, 1), (25, 2), (26, 4), (27, 8)):
        if value < 1 << (8 * width):
            return bytes([(major << 5) | marker]) + value.to_bytes(width, "big")
    raise OTAError("CBOR integer is out of range.")


def cbor_encode(value, _depth=0) -> bytes:
    """Small, definite-length CBOR encoder for the SMP management fields."""
    if _depth > 16:
        raise OTAError("CBOR nesting is too deep.")
    if value is None:
        return b"\xf6"
    if type(value) is bool:
        return b"\xf5" if value else b"\xf4"
    if type(value) is int:
        return _head(0, value) if value >= 0 else _head(1, -1 - value)
    if isinstance(value, bytes):
        return _head(2, len(value)) + value
    if isinstance(value, str):
        encoded = value.encode("utf-8")
        return _head(3, len(encoded)) + encoded
    if isinstance(value, (list, tuple)) and len(value) <= 4096:
        return _head(4, len(value)) + b"".join(cbor_encode(item, _depth + 1) for item in value)
    if isinstance(value, dict) and len(value) <= 4096 and all(isinstance(key, str) for key in value):
        return _head(5, len(value)) + b"".join(cbor_encode(key, _depth + 1) + cbor_encode(item, _depth + 1) for key, item in value.items())
    raise OTAError("Unsupported CBOR value.")


def cbor_decode(data: bytes):
    """Decode bounded SMP CBOR, including Zephyr's indefinite maps/arrays.

    Tags, floats, undefined, duplicate map keys and indefinite strings are
    deliberately rejected; none are used by these management commands.
    """
    if not isinstance(data, bytes) or not 0 < len(data) <= 65535:
        raise OTAError("Invalid CBOR payload size.")
    offset = 0
    count = 0

    def take(size):
        nonlocal offset
        if size < 0 or offset + size > len(data):
            raise OTAError("Truncated CBOR payload.")
        result = data[offset:offset + size]
        offset += size
        return result

    def read(depth=0):
        nonlocal count, offset
        count += 1
        if depth > 16 or count > 4096:
            raise OTAError("CBOR complexity limit exceeded.")
        initial = take(1)[0]
        major, additional = initial >> 5, initial & 31
        if major == 7:
            if additional in (20, 21, 22):
                return {20: False, 21: True, 22: None}[additional]
            raise OTAError("Unsupported CBOR simple value.")
        if additional < 24:
            size = additional
        elif additional in (24, 25, 26, 27):
            size = int.from_bytes(take(1 << (additional - 24)), "big")
        elif additional == 31 and major in (4, 5):
            size = None
        else:
            raise OTAError("Unsupported CBOR length.")
        if major in (0, 1):
            return size if major == 0 else -1 - size
        if major in (2, 3):
            raw = take(size)
            try:
                return raw if major == 2 else raw.decode("utf-8")
            except UnicodeError as error:
                raise OTAError("Invalid CBOR UTF-8 text.") from error
        if major not in (4, 5) or (size is not None and size > 4096):
            raise OTAError("Unsupported CBOR container.")
        result = [] if major == 4 else {}
        n = 0
        while size is None or n < size:
            if size is None:
                if offset >= len(data):
                    raise OTAError("Unterminated CBOR container.")
                if data[offset] == 0xff:
                    offset += 1
                    break
            item = read(depth + 1)
            if major == 4:
                result.append(item)
            else:
                if not isinstance(item, str) or item in result:
                    raise OTAError("CBOR maps require unique text keys.")
                result[item] = read(depth + 1)
            n += 1
        return result

    value = read()
    if offset != len(data):
        raise OTAError("Trailing data after CBOR payload.")
    return value


def inspect_image(data: bytes) -> dict:
    """Check MCUboot layout and SHA256; signature authenticity is MCUboot's job."""
    if not isinstance(data, bytes) or not 32 <= len(data) <= MAX_IMAGE:
        raise OTAError("Expected a signed MCUboot image between 32 bytes and 2 MiB.")
    magic, _, header_size, protected_size, image_size, flags = struct.unpack_from("<IIHHII", data)
    if magic != 0x96f3b83d or header_size < 32 or header_size % 4 or not image_size:
        raise OTAError("Invalid MCUboot image header; use zephyr.signed.bin.")
    if flags:
        raise OTAError("Only ordinary unencrypted flash images are supported by this client.")
    body_end = header_size + image_size
    if body_end > len(data):
        raise OTAError("MCUboot image body is truncated.")

    def tlvs(start, expected_magic, expected_length=None):
        if start + 4 > len(data):
            raise OTAError("Missing MCUboot TLV header.")
        tlv_magic, total = struct.unpack_from("<HH", data, start)
        if tlv_magic != expected_magic or total < 4 or start + total > len(data) or (expected_length is not None and total != expected_length):
            raise OTAError("Invalid MCUboot TLV bounds.")
        cursor, end, entries = start + 4, start + total, []
        while cursor < end:
            if cursor + 4 > end:
                raise OTAError("Truncated MCUboot TLV entry.")
            kind, length = struct.unpack_from("<HH", data, cursor)
            cursor += 4
            if cursor + length > end:
                raise OTAError("MCUboot TLV entry exceeds its area.")
            entries.append((kind, data[cursor:cursor + length]))
            cursor += length
        return end, entries

    protected_end = body_end
    if protected_size:
        protected_end, _ = tlvs(body_end, 0x6908, protected_size)
    image_end, entries = tlvs(protected_end, 0x6907)
    if any(byte != 0xff for byte in data[image_end:]):
        raise OTAError("Unexpected trailer data; use the unpadded zephyr.signed.bin application image.")
    hashes = [value for kind, value in entries if kind == 0x10]
    if len(hashes) != 1 or len(hashes[0]) != 32:
        raise OTAError("The image must contain exactly one SHA256 TLV.")
    digest = hashlib.sha256(data[:protected_end]).digest()
    if hashes[0] != digest:
        raise OTAError("MCUboot SHA256 TLV does not match the image content.")
    signatures = [(kind, value) for kind, value in entries if kind in (0x20, 0x22, 0x23, 0x24)]
    if len(signatures) != 1:
        raise OTAError("The image must contain exactly one supported signature TLV.")
    kind, signature = signatures[0]
    valid_length = {0x20: len(signature) == 256, 0x22: 8 <= len(signature) <= 144,
                    0x23: len(signature) == 384, 0x24: len(signature) == 64}[kind]
    if not valid_length:
        raise OTAError("Invalid MCUboot signature length.")
    major, minor, revision, build = struct.unpack_from("<BBHI", data, 20)
    return {"size": len(data), "version": f"{major}.{minor}.{revision}+{build}",
            "image_hash": digest.hex(), "upload_sha256": hashlib.sha256(data).hexdigest(),
            "signature_present": True, "signature_verified": False,
            "verification": "layout-and-image-sha256; MCUboot verifies the signing key on the device"}


def _hash(value: str | bytes) -> bytes:
    if isinstance(value, str):
        if len(value) != 64:
            raise OTAError("Use the exact 64-character image hash from image status or inspect.")
        try:
            value = bytes.fromhex(value)
        except ValueError as error:
            raise OTAError("Image hash must be hexadecimal.") from error
    if not isinstance(value, bytes) or len(value) != 32:
        raise OTAError("Image hash must contain 32 bytes.")
    return value


class SMPClient:
    def __init__(self, transport: Callable[[bytes], bytes]):
        self.transport = transport
        self.sequence = 0

    def request(self, operation: int, group: int, command: int, payload: dict | None = None) -> dict:
        if operation not in (0, 2) or type(group) is not int or not 0 <= group <= 65535 or type(command) is not int or not 0 <= command <= 255:
            raise OTAError("Invalid SMP request header.")
        body = cbor_encode({} if payload is None else payload)
        if len(body) > 65535:
            raise OTAError("SMP request is too large.")
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 255
        packet = HEADER.pack(operation, 0, len(body), group, sequence, command) + body
        response = self.transport(packet)
        if not isinstance(response, bytes) or not 8 <= len(response) <= MAX_PACKET:
            raise OTAError("Invalid SMP response size.")
        op, flags, length, rsp_group, rsp_sequence, rsp_command = HEADER.unpack_from(response)
        if (op, flags, rsp_group, rsp_sequence, rsp_command) != (operation + 1, 0, group, sequence, command) or length != len(response) - 8:
            raise OTAError("SMP response does not match its request.")
        result = cbor_decode(response[8:])
        if not isinstance(result, dict):
            raise OTAError("SMP response must contain a CBOR map.")
        if "rc" in result and (type(result["rc"]) is not int or result["rc"] != 0):
            raise OTAError(f"Device rejected SMP request (rc={result['rc']!r}).")
        if "err" in result:
            error = result["err"]
            if not isinstance(error, dict) or type(error.get("rc")) is not int or error["rc"] != 0:
                raise OTAError(f"Device rejected SMP request ({error!r}).")
        return result

    def status(self) -> dict:
        result = self.request(0, 1, 0)
        images = result.get("images")
        if not isinstance(images, list) or len(images) > 16:
            raise OTAError("Device returned an invalid image list.")
        for image in images:
            if not isinstance(image, dict) or type(image.get("slot")) is not int or image["slot"] not in (0, 1):
                raise OTAError("Device returned an invalid image slot.")
            if "hash" in image:
                _hash(image["hash"])
            for key in ("active", "pending", "confirmed", "bootable", "permanent"):
                if key in image and type(image[key]) is not bool:
                    raise OTAError("Device returned invalid image state flags.")
        return result

    @staticmethod
    def _matching(state, digest):
        return [image for image in state["images"] if image.get("image", 0) == 0 and image.get("hash") == digest]

    def upload(self, data: bytes, progress: Callable[[int, int], None] | None = None) -> dict:
        metadata = inspect_image(data)
        digest = bytes.fromhex(metadata["upload_sha256"])
        offset = 0
        repeats = resets = 0
        # Partial ACKs are allowed, but pathological peers cannot loop forever.
        packet_limit = max(64, ((len(data) + 255) // 256) * 4)
        for _ in range(packet_limit):
            chunk = data[offset:offset + 256]
            payload = {"off": offset, "data": chunk}
            if offset == 0:
                payload.update(len=len(data), sha=digest)
            result = self.request(2, 1, 1, payload)
            next_offset = result.get("off")
            if type(next_offset) is not int or not 0 <= next_offset <= len(data):
                raise OTAError("Device returned an invalid upload offset.")
            if "match" in result and result["match"] is not True:
                raise OTAError("Device did not verify the uploaded file hash.")
            if next_offset == offset:
                repeats += 1
                if repeats >= 3:
                    raise OTAError("Upload stopped making progress; no reset or confirmation was sent.")
            else:
                repeats = 0
            if next_offset == 0 and offset:
                resets += 1
                if resets > 2:
                    raise OTAError("Device repeatedly restarted the upload.")
            elif next_offset < offset:
                raise OTAError("Device returned a backwards upload offset.")
            elif offset != 0 and next_offset > offset + len(chunk):
                raise OTAError("Device acknowledged bytes that were not sent.")
            offset = next_offset
            if progress:
                progress(offset, len(data))
            if offset == len(data):
                state = self.status()
                matches = self._matching(state, bytes.fromhex(metadata["image_hash"]))
                if not matches:
                    raise OTAError("Transfer finished, but device image status did not contain the uploaded image hash.")
                return {**metadata, "uploaded": True, "image_listed": True, "images": matches,
                        "next_step": "Run test IMAGE_HASH, then reset. Verify the running application before confirm IMAGE_HASH."}
        raise OTAError("Upload exceeded its bounded request count.")

    def test(self, image_hash: str | bytes) -> dict:
        digest = _hash(image_hash)
        matches = self._matching(self.status(), digest)
        if not any(image.get("slot") == 1 and not image.get("active") for image in matches):
            raise OTAError("Test requires an uploaded inactive image in slot 1 with the exact requested hash.")
        return self.request(2, 1, 0, {"hash": digest, "confirm": False})

    def confirm(self, image_hash: str | bytes) -> dict:
        digest = _hash(image_hash)
        matches = self._matching(self.status(), digest)
        if not any(image.get("active") is True for image in matches):
            raise OTAError("Refusing confirmation: the requested hash is not the currently active image.")
        return self.request(2, 1, 0, {"hash": digest, "confirm": True})

    def reset(self) -> dict:
        return self.request(2, 0, 5)


def _json_safe(value):
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, list):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    return value


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gateway", help="NU87 private IP[:port]; required except for inspect")
    parser.add_argument("--gateway-config", type=Path, help="Private local.conf containing the existing gateway token")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="Read MCUboot image slots and hashes")
    for name in ("inspect", "upload"):
        command = commands.add_parser(name, help="Inspect a signed image locally" if name == "inspect" else "Upload signed image; do not switch or reset")
        command.add_argument("file", type=Path)
    for name in ("test", "confirm"):
        command = commands.add_parser(name, help="Mark inactive image for one test boot" if name == "test" else "Confirm only the matching active image")
        command.add_argument("hash", help="MCUboot image SHA256, not the whole-file upload SHA256")
    commands.add_parser("reset", help="Explicitly request a device reset")
    args = parser.parse_args(argv)
    if args.command != "inspect" and not args.gateway:
        parser.error("--gateway is required for device commands")
    try:
        data = None
        if args.command in ("inspect", "upload"):
            if not args.file.is_file() or args.file.stat().st_size > MAX_IMAGE:
                raise OTAError("Select a signed application image file no larger than 2 MiB.")
            data = args.file.read_bytes()
        if args.command == "inspect":
            result = inspect_image(data)
        else:
            gateway = Gateway(host=args.gateway, config_path=args.gateway_config, timeout=30)
            client = SMPClient(gateway.smp)
            if args.command == "upload":
                last_percent = -1

                def progress(done, total):
                    nonlocal last_percent
                    percent = done * 100 // total
                    if percent != last_percent:
                        print(f"Upload {percent}% ({done}/{total} bytes)", file=sys.stderr)
                        last_percent = percent

                result = client.upload(data, progress)
            elif args.command in ("test", "confirm"):
                result = getattr(client, args.command)(args.hash)
            else:
                result = getattr(client, args.command)()
        print(json.dumps(_json_safe(result), ensure_ascii=False, indent=2))
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"OTA failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

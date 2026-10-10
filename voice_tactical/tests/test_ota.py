import hashlib
import struct
import unittest

from voice_tactical.ota import (
    HEADER, OTAError, SMPClient, cbor_decode, cbor_encode, inspect_image,
)


def signed_image(body_size=1800, protected=False):
    header = struct.pack("<IIHHIIBBHII", 0x96f3b83d, 0, 32, 12 if protected else 0,
                         body_size, 0, 1, 2, 3, 4, 0)
    body = bytes((i % 251 for i in range(body_size)))
    protection = struct.pack("<HHHHI", 0x6908, 12, 0x50, 4, 7) if protected else b""
    prefix = header + body + protection
    digest = hashlib.sha256(prefix).digest()
    entries = struct.pack("<HH", 0x10, 32) + digest + struct.pack("<HH", 0x24, 64) + b"s" * 64
    return prefix + struct.pack("<HH", 0x6907, len(entries) + 4) + entries


class Transport:
    def __init__(self, handler):
        self.handler = handler
        self.requests = []

    def __call__(self, packet):
        op, flags, size, group, sequence, command = HEADER.unpack_from(packet)
        assert flags == 0 and size == len(packet) - 8
        payload = cbor_decode(packet[8:])
        request = {"operation": op, "group": group, "command": command, "sequence": sequence, "payload": payload}
        self.requests.append(request)
        body = cbor_encode(self.handler(request))
        return HEADER.pack(op + 1, 0, len(body), group, sequence, command) + body


class CBORTests(unittest.TestCase):
    def test_round_trip_management_types(self):
        value = {"off": 66000, "rc": -8, "hash": b"\x00\xff", "images": [True, False, None, "한국어", 2**40]}
        self.assertEqual(cbor_decode(cbor_encode(value)), value)

    def test_zephyr_indefinite_map_and_arrays(self):
        value = b"\xbf\x66images\x9f\xbf\x64slot\x00\x66active\xf5\xff\xff\xff"
        self.assertEqual(cbor_decode(value), {"images": [{"slot": 0, "active": True}]})

    def test_malformed_and_unbounded_cbor_rejected(self):
        for value in (b"", b"\xbf", b"\xa1\x61a", b"\xa2\x61a\x00\x61a\x01",
                      b"\xa1\x00\x00", b"\xa0\x00", b"\x5a\xff\xff\xff\xff",
                      b"\x61\xff", b"\x9a\xff\xff\xff\xff", b"\xc0\x00",
                      b"\xf9\x00\x00", b"\x9f" * 18 + b"\xff" * 18):
            with self.subTest(value=value), self.assertRaises(OTAError):
                cbor_decode(value)


class ImageTests(unittest.TestCase):
    def test_hashes_are_distinct_and_protected_metadata_is_hashed(self):
        for protected in (False, True):
            data = signed_image(protected=protected)
            result = inspect_image(data)
            self.assertEqual(result["version"], "1.2.3+4")
            self.assertNotEqual(result["image_hash"], result["upload_sha256"])
            self.assertEqual(result["upload_sha256"], hashlib.sha256(data).hexdigest())
            self.assertTrue(result["signature_present"])
            self.assertFalse(result["signature_verified"])

    def test_invalid_headers_hashes_tlvs_and_unsigned_image_rejected(self):
        original = signed_image()
        cases = [original[:12], b"wrong" + original[5:], original[:-5], original + b"unexpected"]
        for offset in (8, 12, 16, 100, len(original) - 108):
            modified = bytearray(original)
            modified[offset] ^= 1
            cases.append(bytes(modified))
        unsigned = original[:-68]
        unsigned = unsigned[:-40] + struct.pack("<HH", 0x6907, 40) + unsigned[-36:]
        cases.append(unsigned)
        for data in cases:
            with self.subTest(size=len(data)), self.assertRaises(OTAError):
                inspect_image(data)


class ProtocolTests(unittest.TestCase):
    def test_response_headers_and_lengths_must_match(self):
        def response(packet, changed):
            values = list(HEADER.unpack_from(packet))
            values[0] += 1
            values[2] = 1
            values[changed] += 1
            return HEADER.pack(*values) + b"\xa0"
        for index in range(6):
            with self.subTest(index=index), self.assertRaises(OTAError):
                SMPClient(lambda packet: response(packet, index)).request(0, 1, 0)

    def test_device_error_and_invalid_status_are_not_success(self):
        for value in ({"rc": 2}, {"err": {"group": 1, "rc": 3}}, {"rc": False}, [], {"images": [{}]}, {"images": [{"slot": True}]}):
            with self.subTest(value=value), self.assertRaises(OTAError):
                SMPClient(Transport(lambda request: value)).status()

    def test_sequence_wrap_and_empty_reset_response(self):
        transport = Transport(lambda request: {})
        client = SMPClient(transport)
        client.sequence = 255
        self.assertEqual(client.reset(), {})
        client.reset()
        self.assertEqual([r["sequence"] for r in transport.requests], [255, 0])
        self.assertTrue(all(r["group"] == 0 and r["command"] == 5 and r["payload"] == {} for r in transport.requests))


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.image = signed_image()
        self.metadata = inspect_image(self.image)
        self.digest = bytes.fromhex(self.metadata["image_hash"])

    def handler(self, request):
        if request["command"] == 0:
            return {"images": [{"slot": 1, "hash": self.digest, "bootable": True}]}
        payload = request["payload"]
        return {"off": payload["off"] + len(payload["data"])}

    def test_upload_has_correct_whole_file_hash_and_no_boot_actions(self):
        transport = Transport(self.handler)
        progress = []
        result = SMPClient(transport).upload(self.image, lambda done, total: progress.append((done, total)))
        self.assertTrue(result["uploaded"])
        first = transport.requests[0]["payload"]
        self.assertEqual(first["sha"], hashlib.sha256(self.image).digest())
        self.assertEqual(first["len"], len(self.image))
        chunks = [r["payload"] for r in transport.requests if r["command"] == 1]
        self.assertEqual(b"".join(chunk["data"] for chunk in chunks), self.image)
        self.assertTrue(all(len(chunk["data"]) <= 256 for chunk in chunks))
        self.assertTrue(all("sha" not in chunk and "len" not in chunk for chunk in chunks[1:]))
        self.assertEqual(progress[-1], (len(self.image), len(self.image)))
        self.assertTrue(all(r["group"] == 1 for r in transport.requests))
        self.assertFalse(any("confirm" in r["payload"] for r in transport.requests))

    def test_resume_and_partial_ack_offsets(self):
        def handler(request):
            if request["command"] == 0:
                return self.handler(request)
            payload = request["payload"]
            return {"off": 512 if payload["off"] == 0 else payload["off"] + min(128, len(payload["data"]))}
        transport = Transport(handler)
        self.assertTrue(SMPClient(transport).upload(self.image)["uploaded"])
        self.assertEqual(transport.requests[1]["payload"]["off"], 512)

    def test_device_restart_resends_initial_metadata(self):
        restarted = False

        def handler(request):
            nonlocal restarted
            if request["command"] == 1 and request["payload"]["off"] == 256 and not restarted:
                restarted = True
                return {"off": 0}
            return self.handler(request)

        transport = Transport(handler)
        self.assertTrue(SMPClient(transport).upload(self.image)["uploaded"])
        first_packets = [r["payload"] for r in transport.requests if r["command"] == 1 and r["payload"]["off"] == 0]
        self.assertEqual(len(first_packets), 2)
        self.assertTrue(all("sha" in packet and "len" in packet for packet in first_packets))

    def test_no_progress_offset_errors_and_file_mismatch_abort(self):
        cases = ({"off": 0}, {"off": -1}, {"off": True}, {"off": len(self.image) + 1}, {"off": 256, "match": False})
        for result in cases:
            transport = Transport(lambda request: result)
            with self.subTest(result=result), self.assertRaises(OTAError):
                SMPClient(transport).upload(self.image)
            self.assertLessEqual(len(transport.requests), 3)

    def test_later_forward_jump_and_unknown_final_image_abort(self):
        def jump(request):
            return {"off": 256 if request["payload"]["off"] == 0 else 1000}
        with self.assertRaises(OTAError):
            SMPClient(Transport(jump)).upload(self.image)
        def unknown(request):
            return {"images": []} if request["command"] == 0 else {"off": len(self.image)}
        with self.assertRaises(OTAError):
            SMPClient(Transport(unknown)).upload(self.image)

    def test_invalid_image_never_reaches_transport(self):
        transport = Transport(self.handler)
        with self.assertRaises(OTAError):
            SMPClient(transport).upload(b"not firmware")
        self.assertEqual(transport.requests, [])

    def test_confirmation_checks_exact_active_hash_before_any_write(self):
        def inactive(request):
            return {"images": [{"slot": 1, "hash": self.digest}]}
        transport = Transport(inactive)
        with self.assertRaises(OTAError):
            SMPClient(transport).confirm(self.digest)
        self.assertEqual(len(transport.requests), 1)
        self.assertEqual(transport.requests[0]["operation"], 0)

        def active(request):
            return {"images": [{"slot": 0, "hash": self.digest, "active": True}]}
        transport = Transport(active)
        SMPClient(transport).confirm(self.digest.hex())
        self.assertEqual(transport.requests[-1]["payload"], {"hash": self.digest, "confirm": True})

    def test_test_boot_requires_inactive_secondary_slot(self):
        transport = Transport(self.handler)
        SMPClient(transport).test(self.digest)
        self.assertEqual(transport.requests[-1]["payload"], {"hash": self.digest, "confirm": False})
        for wrong in ("ab", "zz" * 32, b"x" * 32):
            with self.subTest(wrong=wrong), self.assertRaises(OTAError):
                SMPClient(Transport(self.handler)).test(wrong)


if __name__ == "__main__":
    unittest.main()

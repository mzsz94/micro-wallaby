import io
import struct
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import wave

from voice_tactical.device_audio import DeviceAudio, UtteranceVAD, decode_frame


def frame(session=1, seq=0, predictor=0, index=0, payload=None):
    return b"WA\x01\x00" + struct.pack("<IIhBB", session, seq, predictor, index, 0) + (payload or bytes(160))


class CodecTests(unittest.TestCase):
    def test_zero_frame_and_header_validation(self):
        session, seq, samples = decode_frame(frame(12, 34))
        self.assertEqual((session, seq, len(samples)), (12, 34, 320))
        self.assertEqual(samples, [0] * 320)
        for packet in (frame()[:-1], frame(index=89), frame(session=0), b"XX" + frame()[2:], frame(payload=bytes(159) + b"\xf0")):
            with self.assertRaises(ValueError):
                decode_frame(packet)

    def test_known_nibbles_and_saturation(self):
        _, _, samples = decode_frame(frame(predictor=1000, payload=b"\x21" + bytes(159)))
        # IMA index0, step7: nibble1 adds1, nibble2 adds3.
        self.assertEqual(samples[:3], [1000, 1001, 1004])
        _, _, samples = decode_frame(frame(predictor=32760, index=88, payload=b"\x77" * 159 + b"\x07"))
        self.assertEqual(samples[1:], [32767] * 319)

    def test_vad_ignores_dc_silence_click_and_emits_bounded_wav(self):
        vad = UtteranceVAD()
        silent = [1000] * 320  # DC offset is not speech.
        sound = [2500, -2500] * 160
        for _ in range(100):
            self.assertIsNone(vad.feed(silent))
        self.assertIsNone(vad.feed(sound))
        for _ in range(40):
            self.assertIsNone(vad.feed(silent))
        for _ in range(20):
            self.assertIsNone(vad.feed(sound))
        output = None
        for _ in range(35):
            output = vad.feed(silent) or output
        self.assertIsNotNone(output)
        with wave.open(io.BytesIO(output)) as wav:
            self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
            self.assertLessEqual(wav.getnframes(), 16000 * 12)


class FakeGateway:
    def __init__(self, bad_seq=False):
        self.session = 0
        self.bad_seq = bad_seq
        self.sent = False
        self.stops = 0

    def audio_status(self):
        return {"ok": True, "available": True}

    def audio_start(self, session):
        self.session = session
        self.sent = False
        return {"ok": True}

    def audio_stop(self):
        self.stops += 1
        return {"ok": True}

    def audio_chunk(self):
        if not self.sent:
            self.sent = True
            return frame(self.session, 4 if self.bad_seq else 0)
        return b""


class DeviceSessionTests(unittest.TestCase):
    def app(self, gateway):
        def turn(**kwargs):
            self.fail("Silence or broken streams must not run inference")
        return SimpleNamespace(gateway=gateway, transcriber=SimpleNamespace(status=lambda: {"ready": True}), turn=turn)

    def test_gap_discards_stream_and_stops_hardware(self):
        gateway = FakeGateway(bad_seq=True)
        mic = DeviceAudio(self.app(gateway))
        mic.start()
        mic._thread.join(2)
        state = mic.status()
        self.assertFalse(state["running"])
        self.assertEqual(state["state"], "error")
        self.assertIsNone(state["last_result"])
        self.assertGreater(gateway.stops, 0)

    def test_browser_lease_expiry_stops_recording(self):
        gateway = FakeGateway()
        mic = DeviceAudio(self.app(gateway), lease_seconds=0.05)
        started = mic.start()
        mic._thread.join(2)
        self.assertFalse(mic.status()["running"])
        self.assertIn("제어 화면", mic.status()["error"])
        self.assertGreater(started["generation"], 0)

    def test_manual_stop_invalidates_generation(self):
        gateway = FakeGateway()
        mic = DeviceAudio(self.app(gateway))
        started = mic.start()
        mic.gate(True)
        self.assertTrue(mic.status()["muted"])
        stopped = mic.stop()
        mic._thread.join(2)
        self.assertGreater(stopped["generation"], started["generation"])
        self.assertFalse(stopped["running"])
        self.assertIsNone(stopped["last_result"])

    def test_stop_during_slow_start_cannot_restart_capture(self):
        entered, release = threading.Event(), threading.Event()

        class SlowGateway(FakeGateway):
            def audio_start(self, session):
                entered.set()
                release.wait(2)
                return super().audio_start(session)

        gateway = SlowGateway()
        mic = DeviceAudio(self.app(gateway))
        starter = threading.Thread(target=mic.start)
        starter.start()
        self.assertTrue(entered.wait(1))
        reserved = mic.status()["generation"]
        stopper = threading.Thread(target=mic.stop)
        stopper.start()
        for _ in range(100):
            if mic.status()["generation"] > reserved:
                break
            time.sleep(.001)
        release.set()
        starter.join(2)
        stopper.join(2)
        self.assertFalse(starter.is_alive())
        self.assertFalse(stopper.is_alive())
        self.assertFalse(mic.status()["running"])
        self.assertIsNone(mic._thread)
        self.assertGreater(gateway.stops, 0)

    def test_idle_and_wrong_session_status_reads_do_not_renew_lease(self):
        mic = DeviceAudio(self.app(FakeGateway()))
        mic._running, mic._generation, mic._last_seen = True, 7, 1
        with patch("voice_tactical.device_audio.time.monotonic", return_value=10):
            mic.status()
            mic.status(touch=True, generation=6)
            self.assertEqual(mic._last_seen, 1)
            mic.status(touch=True, generation=7)
            self.assertEqual(mic._last_seen, 10)

    def test_gate_round_trip_discards_inflight_result(self):
        entered, release, restarted = threading.Event(), threading.Event(), threading.Event()

        class RestartGateway(FakeGateway):
            def audio_start(self, session):
                if self.session:
                    restarted.set()
                return super().audio_start(session)

        def turn(**kwargs):
            entered.set()
            release.wait(2)
            return {"id": "stale", "motion": "forward"}

        gateway = RestartGateway()
        app = self.app(gateway)
        app.turn = turn
        mic = DeviceAudio(app)
        with patch.object(UtteranceVAD, "feed", side_effect=[b"wav", None]):
            mic.start()
            self.assertTrue(entered.wait(1))
            before = mic.status()["gate_generation"]
            mic.gate(True)
            mic.gate(False)
            release.set()
            self.assertTrue(restarted.wait(1))
            self.assertIsNone(mic.status()["last_result"])
            self.assertGreater(mic.status()["gate_generation"], before)
            mic.stop()
            mic._thread.join(2)

    def test_unrecognized_speech_resumes_continuous_listening(self):
        restarted = threading.Event()

        class RestartGateway(FakeGateway):
            def audio_start(self, session):
                if self.session:
                    restarted.set()
                return super().audio_start(session)

        class NoSpeech(Exception):
            status = 422

        def turn(**kwargs):
            raise NoSpeech()

        gateway = RestartGateway()
        app = self.app(gateway)
        app.turn = turn
        mic = DeviceAudio(app)
        with patch.object(UtteranceVAD, "feed", side_effect=[b"wav", None]):
            mic.start()
            self.assertTrue(restarted.wait(1))
            state = mic.status()
            self.assertTrue(state["running"])
            self.assertIsNone(state["error"])
            self.assertIsNone(state["last_result"])
            self.assertTrue(state["notice"])
            mic.stop()
            mic._thread.join(2)

    def test_silent_stream_renews_before_firmware_thirty_second_limit(self):
        restarted = threading.Event()

        class SilenceGateway(FakeGateway):
            def audio_start(self, session):
                if self.session:
                    restarted.set()
                self.sequence = 0
                return super().audio_start(session)

            def audio_chunk(self):
                if restarted.is_set():
                    return b""
                packet = frame(self.session, self.sequence)
                self.sequence += 1
                return packet

        gateway = SilenceGateway()
        mic = DeviceAudio(self.app(gateway))
        mic.start()
        self.assertTrue(restarted.wait(2))
        self.assertTrue(mic.status()["running"])
        self.assertIsNone(mic.status()["error"])
        mic.stop()
        mic._thread.join(2)

    def test_lease_expired_during_inference_discards_result_without_restart(self):
        entered, release = threading.Event(), threading.Event()
        gateway = FakeGateway()
        app = self.app(gateway)

        def turn(**kwargs):
            entered.set()
            release.wait(2)
            return {"id": "expired", "motion": "forward"}

        app.turn = turn
        mic = DeviceAudio(app, lease_seconds=.05)
        with patch.object(UtteranceVAD, "feed", return_value=b"wav"):
            mic.start()
            self.assertTrue(entered.wait(1))
            time.sleep(.06)
            release.set()
            mic._thread.join(2)
        self.assertFalse(mic.status()["running"])
        self.assertIsNone(mic.status()["last_result"])
        self.assertIn("제어 화면", mic.status()["error"])

    def test_control_tower_warning_stops_microphone_without_restart(self):
        gateway = FakeGateway()
        app = self.app(gateway)
        app.alert_active = False
        mic = DeviceAudio(app)
        mic.start()
        app.alert_active = True
        mic._thread.join(2)
        self.assertFalse(mic.status()["running"])
        self.assertEqual(mic.status()["state"], "alert")
        self.assertGreater(gateway.stops, 0)
        with self.assertRaises(RuntimeError):
            mic.start()


if __name__ == "__main__":
    unittest.main()

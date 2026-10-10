import io
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import wave

from voice_tactical.audio import LocalTranscriber, validate_wav


def wav_bytes(rate=16000, channels=1, width=2, frames=1600):
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(width)
        wav.setframerate(rate)
        wav.writeframes(b"\x00" * frames * channels * width)
    return output.getvalue()


class AudioTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.model_path = Path(self.temp.name)

    def prepared_model(self):
        for name in ("model.bin", "config.json", "tokenizer.json"):
            (self.model_path / name).write_text("{}")
        return LocalTranscriber(self.model_path)

    def test_valid_wav_and_duration_boundary(self):
        data = wav_bytes(frames=480000)
        self.assertEqual(validate_wav(data), data)
        with self.assertRaises(ValueError):
            validate_wav(wav_bytes(frames=480001))

    def test_rejects_wrong_format_truncation_and_empty_audio(self):
        cases = [b"hello", b"RIFF" + b"\x00" * 40, wav_bytes(rate=44100), wav_bytes(channels=2),
                 wav_bytes(width=1), wav_bytes(frames=0), wav_bytes()[:-2]]
        truncated = bytearray(wav_bytes()[:-4])
        struct.pack_into("<I", truncated, 4, len(truncated) - 8)
        cases.append(bytes(truncated))
        for data in cases:
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                validate_wav(data)

    def test_missing_models_do_not_load_or_download(self):
        transcriber = LocalTranscriber("small")
        self.assertFalse(transcriber.status()["ready"])
        with patch("importlib.util.find_spec") as spec:
            with self.assertRaises(RuntimeError):
                transcriber.transcribe(wav_bytes())
            spec.assert_not_called()
        self.assertFalse(LocalTranscriber().status()["ready"])

    def test_missing_tokenizer_blocks_faster_whisper_remote_fallback(self):
        (self.model_path / "model.bin").write_text("test")
        (self.model_path / "config.json").write_text("{}")
        transcriber = LocalTranscriber(self.model_path)
        self.assertIn("tokenizer.json", transcriber.status()["detail"])
        with self.assertRaises(RuntimeError):
            transcriber.transcribe(wav_bytes())

    def test_lazy_local_model_initialization_and_transcription(self):
        transcriber = self.prepared_model()
        model = Mock()
        model.transcribe.return_value = ([SimpleNamespace(text=" 왈라비 "), SimpleNamespace(text="멈춰")], None)
        factory = Mock(return_value=model)
        with patch("importlib.util.find_spec", return_value=object()), patch.dict(sys.modules, {"faster_whisper": SimpleNamespace(WhisperModel=factory)}):
            self.assertTrue(transcriber.status()["ready"])
            factory.assert_not_called()
            self.assertEqual(transcriber.transcribe(wav_bytes()), "왈라비 멈춰")
            self.assertEqual(transcriber.transcribe(wav_bytes()), "왈라비 멈춰")
        factory.assert_called_once_with(str(self.model_path.resolve()), device="cpu", compute_type="int8", local_files_only=True)
        self.assertTrue(model.transcribe.call_args.kwargs["vad_filter"])
        self.assertEqual(model.transcribe.call_args.kwargs["language"], "ko")

    def test_bad_audio_is_rejected_before_loading_model(self):
        transcriber = self.prepared_model()
        with patch("importlib.util.find_spec") as spec, self.assertRaises(ValueError):
            transcriber.transcribe(b"not wav")
        spec.assert_not_called()

    def test_model_failure_is_visible_and_empty_speech_is_not_a_command(self):
        transcriber = self.prepared_model()
        factory = Mock(side_effect=OSError("invalid model"))
        with patch("importlib.util.find_spec", return_value=object()), patch.dict(sys.modules, {"faster_whisper": SimpleNamespace(WhisperModel=factory)}):
            with self.assertRaises(RuntimeError):
                transcriber.transcribe(wav_bytes())
            self.assertFalse(transcriber.status()["ready"])
        transcriber = self.prepared_model()
        transcriber._model = Mock()
        transcriber._model.transcribe.return_value = ([], None)
        with patch("importlib.util.find_spec", return_value=object()), self.assertRaises(ValueError):
            transcriber.transcribe(wav_bytes())


if __name__ == "__main__":
    unittest.main()

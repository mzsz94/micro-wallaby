"""Local-only Whisper adapter. No model names, remote fetches, or browser STT."""
from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import struct
import threading
import wave

SAMPLE_RATE = 16000
MAX_SECONDS = 30
MAX_WAV_BYTES = SAMPLE_RATE * 2 * MAX_SECONDS + 65536


def validate_wav(data: bytes) -> bytes:
    """Validate the complete PCM payload before passing it to an audio decoder."""
    if not isinstance(data, bytes) or len(data) < 44 or len(data) > MAX_WAV_BYTES:
        raise ValueError("WAV 파일은 비어 있지 않은 30초 이하 음성이어야 합니다.")
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE" or struct.unpack_from("<I", data, 4)[0] + 8 != len(data):
        raise ValueError("완전한 RIFF WAV 파일이 필요합니다.")
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (1, 2, SAMPLE_RATE, "NONE"):
                raise ValueError("16 kHz 모노 PCM16 WAV만 지원합니다.")
            frames = wav.getnframes()
            if not 1 <= frames <= SAMPLE_RATE * MAX_SECONDS:
                raise ValueError("음성 길이는 0초 초과, 30초 이하여야 합니다.")
            pcm = wav.readframes(frames + 1)
            if len(pcm) != frames * 2:
                raise ValueError("WAV 음성 데이터가 잘렸습니다.")
    except (wave.Error, EOFError, struct.error) as exc:
        raise ValueError("올바른 PCM WAV 파일이 아닙니다.") from exc
    return data


class LocalTranscriber:
    def __init__(self, model_path: str | Path | None = None):
        self.model_path = Path(model_path).expanduser().resolve() if model_path else None
        self._model = None
        self._lock = threading.Lock()
        self._error = None

    def status(self) -> dict:
        detail = "로컬 Whisper 모델 경로를 설정하세요."
        ready = False
        if self.model_path:
            # faster-whisper can fetch a tokenizer even with local_files_only if
            # tokenizer.json is missing, so require it before instantiation.
            missing = [name for name in ("model.bin", "config.json", "tokenizer.json") if not (self.model_path / name).is_file()]
            if missing:
                detail = "로컬 모델 파일이 없습니다: " + ", ".join(missing)
            elif importlib.util.find_spec("faster_whisper") is None:
                detail = "faster-whisper가 설치되지 않았습니다. 오프라인 준비 단계에서 설치하세요."
            elif self._error:
                detail = self._error
            else:
                ready = True
                detail = "로컬 Whisper 준비됨" if self._model else "로컬 모델 준비됨 (첫 전사 시 로드)"
        return {"ready": ready, "engine": "faster-whisper-local", "detail": detail,
                "sample_rate": SAMPLE_RATE, "max_seconds": MAX_SECONDS}

    def transcribe(self, wav_bytes: bytes) -> str:
        validate_wav(wav_bytes)
        with self._lock:
            state = self.status()
            if not state["ready"]:
                raise RuntimeError(state["detail"])
            try:
                if self._model is None:
                    from faster_whisper import WhisperModel
                    self._model = WhisperModel(str(self.model_path), device="cpu", compute_type="int8", local_files_only=True)
                segments, _ = self._model.transcribe(io.BytesIO(wav_bytes), language="ko", beam_size=3,
                                                     vad_filter=True, condition_on_previous_text=False)
                text = " ".join(segment.text.strip() for segment in segments).strip()
                if not text:
                    raise ValueError("음성을 인식하지 못했습니다. 마이크 입력을 확인하고 다시 말해 주세요.")
                return text
            except ValueError:
                raise
            except Exception as exc:
                self._error = f"로컬 Whisper 실행 실패 ({type(exc).__name__}). 모델과 설치 상태를 확인하세요."
                raise RuntimeError(self._error) from exc

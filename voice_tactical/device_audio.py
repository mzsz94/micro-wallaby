"""NU40 PDM → BLE IMA ADPCM → NU87 HTTP → local speech turns."""
from collections import deque
import io
import math
import secrets
import struct
import threading
import time
import wave

FRAME_BYTES = 176
SAMPLES_PER_FRAME = 320
SAMPLE_RATE = 16000
STEP_TABLE = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
              31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
              130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
              449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
              1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
              3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
              9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
              22385, 24623, 27086, 29794, 32767)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8)


def decode_frame(frame):
    """Each independently decodable frame contains exactly 20 ms of mono PCM."""
    if len(frame) != FRAME_BYTES or frame[:4] != b"WA\x01\x00" or frame[15] != 0:
        raise ValueError("올바르지 않은 NU40 오디오 프레임입니다.")
    session, sequence, predictor, index = struct.unpack_from("<IIhB", frame, 4)
    if not session or index > 88 or frame[-1] & 0xF0:
        raise ValueError("NU40 오디오 헤더 또는 ADPCM 데이터가 손상되었습니다.")
    samples = [predictor]
    for n in range(SAMPLES_PER_FRAME - 1):
        code = (frame[16 + n // 2] >> (4 * (n % 2))) & 15
        step = STEP_TABLE[index]
        delta = (step >> 3) + (step if code & 4 else 0) + (step >> 1 if code & 2 else 0) + (step >> 2 if code & 1 else 0)
        predictor = max(-32768, min(32767, predictor + (-delta if code & 8 else delta)))
        index = max(0, min(88, index + INDEX_TABLE[code & 7]))
        samples.append(predictor)
    return session, sequence, samples


def wav_bytes(samples):
    stream = io.BytesIO()
    with wave.open(stream, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    return stream.getvalue()


class UtteranceVAD:
    """Energy VAD with pre-roll, 700 ms silence, and a bounded 12 s utterance."""
    def __init__(self, threshold=500):
        self.threshold = threshold
        self.reset()

    def reset(self):
        self.preroll = deque(maxlen=10)
        self.frames = []
        self.voiced = 0
        self.silent = 0

    def feed(self, samples):
        if len(samples) != SAMPLES_PER_FRAME:
            raise ValueError("320 samples required")
        # Ignore DC bias when detecting speech.
        mean = sum(samples) / len(samples)
        rms = math.sqrt(sum((s - mean) ** 2 for s in samples) / len(samples))
        speaking = rms >= self.threshold
        if not self.frames:
            self.preroll.append(samples)
            self.voiced = self.voiced + 1 if speaking else 0
            if self.voiced >= 3:
                self.frames = list(self.preroll)
            return None
        self.frames.append(samples)
        self.voiced += int(speaking)
        self.silent = 0 if speaking else self.silent + 1
        if self.silent >= 35 or len(self.frames) >= 600:
            result = wav_bytes([s for frame in self.frames for s in frame]) if self.voiced >= 12 else None
            self.reset()
            return result
        return None


class DeviceAudio:
    """One half-duplex microphone session, stopped when its UI lease expires.

    Recording pauses during inference. A new stream ID after each inference
    prevents old BLE/HTTP packets from being interpreted as a later command.
    """
    def __init__(self, app, *, threshold=500, lease_seconds=5):
        self.app = app
        self.threshold = threshold
        self.lease_seconds = lease_seconds
        self._lock = threading.RLock()
        self._control = threading.Lock()
        self._generation = 0
        self._running = False
        self._state = "idle"
        self._error = None
        self._last_result = None
        self._muted = False
        self._gate_generation = 0
        self._notice = None
        self._gate_until = 0
        self._last_seen = time.monotonic()
        self._thread = None
        self._available = False

    def status(self, *, touch=False, generation=None):
        with self._lock:
            # An idle dashboard must not keep another tab's microphone alive.
            if touch and self._running and generation == self._generation:
                self._last_seen = time.monotonic()
            return {"running": self._running, "state": self._state, "error": self._error,
                    "last_result": self._last_result, "generation": self._generation,
                    "available": self._available, "muted": self._muted,
                    "gate_generation": self._gate_generation, "notice": self._notice,
                    "source": "nu40-pdm", "sample_rate": SAMPLE_RATE}

    def _live(self, generation):
        with self._lock:
            return self._running and generation == self._generation

    def _restart_stream(self, generation, *, stopped=False):
        with self._control:
            if not self._live(generation):
                return None
            if not stopped:
                self._require_ok(self.app.gateway.audio_stop())
            if not self._live(generation):
                return None
            session = secrets.randbits(32) or 1
            self._require_ok(self.app.gateway.audio_start(session))
            return session if self._live(generation) else None

    @staticmethod
    def _require_ok(reply):
        if not isinstance(reply, dict) or reply.get("ok") is not True:
            message = reply.get("error") if isinstance(reply, dict) else None
            raise RuntimeError(message or "NU87 오디오 요청 실패")

    def start(self, mode="auto"):
        if not isinstance(mode, str) or mode not in {"auto", "query", "report", "orders", "search"}:
            raise ValueError("지원하지 않는 모드입니다.")
        with self._control:
            if self._thread and self._thread.is_alive():
                if self._running:
                    return self.status()
                raise RuntimeError("이전 마이크 세션이 종료 중입니다. 잠시 후 시작하세요.")
            if not self.app.transcriber.status()["ready"]:
                raise RuntimeError("로컬 Whisper 모델을 먼저 준비하세요.")
            if getattr(self.app, "alert_active", False):
                raise RuntimeError("경고를 해제한 뒤 마이크를 다시 시작하세요.")
            # Reserve the generation before any network I/O. stop() may
            # invalidate it even while this method owns the control lock.
            with self._lock:
                self._generation += 1
                generation = self._generation
                self._state, self._error, self._last_result = "starting", None, None
                self._last_seen = time.monotonic()
            try:
                capability = self.app.gateway.audio_status()
                self._require_ok(capability)
                if not capability.get("available"):
                    raise RuntimeError("NU40/NU87 음성 펌웨어와 BLE 오디오 연결이 필요합니다.")
                with self._lock:
                    if generation != self._generation:
                        return self.status()
                session = secrets.randbits(32) or 1
                self._require_ok(self.app.gateway.audio_start(session))
                with self._lock:
                    if generation != self._generation:
                        # stop() already owns the invalidation and is waiting
                        # for this control lock to stop the physical stream.
                        return self.status()
                    self._running = self._available = True
                    self._state, self._notice = "listening", None
                    self._muted = False
                    self._gate_until = 0
                    self._gate_generation += 1
                    self._thread = threading.Thread(target=self._run, args=(generation, session, mode), daemon=True)
                    self._thread.start()
            except Exception:
                with self._lock:
                    if generation == self._generation:
                        self._state = "idle"
                        self._available = False
                raise
            return self.status()

    def stop(self):
        # Invalidate results immediately, before a possibly slow network request.
        with self._lock:
            self._running = False
            self._generation += 1
            self._state = "idle"
            self._last_result = None
            self._muted = False
            self._notice = None
        with self._control:
            result = self.app.gateway.audio_stop()
        response = self.status()
        response["device"] = result
        return response

    def gate(self, muted):
        if not isinstance(muted, bool):
            raise ValueError("muted는 boolean이어야 합니다.")
        with self._lock:
            self._muted = muted
            self._gate_until = time.monotonic() + 0.7
            self._gate_generation += 1
            if muted:
                self._last_result = None
        return self.status()

    def _run(self, generation, session, mode):
        vad = UtteranceVAD(self.threshold)
        expected = 0
        last_packet = time.monotonic()
        try:
            while self._live(generation):
                if getattr(self.app, "alert_active", False):
                    with self._lock:
                        self._state = "alert"
                    break
                now = time.monotonic()
                if now - self._last_seen > self.lease_seconds:
                    raise RuntimeError("제어 화면 연결이 끊겨 NU40 마이크를 정지했습니다.")
                chunk = self.app.gateway.audio_chunk()
                if len(chunk) % FRAME_BYTES or len(chunk) > FRAME_BYTES * 32:
                    raise RuntimeError("NU87 오디오 응답 길이가 올바르지 않습니다.")
                if not chunk:
                    if now - last_packet > 3:
                        raise RuntimeError("NU40 마이크 데이터가 3초 동안 도착하지 않았습니다.")
                    time.sleep(0.05)
                    continue
                last_packet = now
                utterance = None
                for offset in range(0, len(chunk), FRAME_BYTES):
                    frame_session, sequence, samples = decode_frame(chunk[offset:offset + FRAME_BYTES])
                    if frame_session != session or sequence != expected:
                        raise RuntimeError("오디오 패킷 누락·중복·세션 변경을 감지해 발화를 폐기했습니다.")
                    expected = (expected + 1) & 0xFFFFFFFF
                    if self._muted or time.monotonic() < self._gate_until:
                        vad.reset()
                        continue
                    utterance = vad.feed(samples)
                    if utterance:
                        break
                if utterance and self._live(generation):
                    with self._lock:
                        gate_generation = self._gate_generation
                        if self._muted or time.monotonic() < self._gate_until:
                            vad.reset()
                            continue
                    with self._control:
                        if not self._live(generation):
                            break
                        self._require_ok(self.app.gateway.audio_stop())
                    with self._lock:
                        if not self._live(generation):
                            break
                        self._state = "processing"
                    try:
                        result = self.app.turn(audio=utterance, mode=mode)
                    except Exception as error:
                        # VAD may include a cough or noise that Whisper drops;
                        # an overlapping typed turn may also own the lock.
                        # Neither condition should end hands-free listening.
                        if getattr(error, "status", None) not in (409, 422):
                            raise
                        result = None
                        with self._lock:
                            self._notice = "인식된 발화가 없거나 다른 입력을 처리 중입니다. 다시 말해 주세요."
                    if not self._live(generation):
                        break
                    with self._lock:
                        if time.monotonic() - self._last_seen > self.lease_seconds:
                            raise RuntimeError("제어 화면 연결이 끊겨 NU40 마이크를 정지했습니다.")
                        # Never dispatch a transcription after a browser/TTS cancellation.
                        if result is not None and not self._muted and gate_generation == self._gate_generation:
                            self._last_result = result
                            self._notice = None
                    if getattr(self.app, "alert_active", False):
                        with self._lock:
                            self._state = "alert"
                        break
                    session = self._restart_stream(generation, stopped=True)
                    if session is None:
                        break
                    expected = 0
                    vad.reset()
                    last_packet = time.monotonic()
                    with self._lock:
                        if self._live(generation):
                            self._state = "listening"
                elif expected >= 1250:
                    # NU40 stops capture at 1500 frames (30 s). Renew silent
                    # or TTS-gated streams before that firmware safety limit.
                    session = self._restart_stream(generation)
                    if session is None:
                        break
                    expected = 0
                    vad.reset()
                    last_packet = time.monotonic()
        except Exception as error:
            with self._lock:
                if generation == self._generation:
                    self._error = str(error) if isinstance(error, (ValueError, RuntimeError)) else "NU40 음성 처리 실패. 연결과 모델 상태를 확인하세요."
                    self._state = "error"
                    self._last_result = None
        finally:
            with self._lock:
                if generation == self._generation:
                    self._running = False
            with self._control:
                self.app.gateway.audio_stop()

# Military Wallaby · 밀리터리 왈라비

microWallaby의 `test/military-wallaby` 미니 프로젝트입니다. 주제는 **Voice-first
Tactical AI**입니다. **NU40DK는 컨트롤 타워**, **NU87 Tiny는 왈라비**,
**HostPC는 클라우드 서버 역할을 대신하는 로컬 AI 서버**입니다. NU40DK의 PDM 마이크로
말하면 HostPC가 Whisper와 로컬 LLM으로 분석하고, NU87이 받아들인 명령을 기존
`feat/motion-simulator`의 왈라비 움직임으로 보여줍니다.

```text
NU40DK 컨트롤 타워: 키트 PDM 마이크 (16 kHz mono)
  → BLE IMA ADPCM (20 ms/frame)
  → NU87 Tiny 왈라비
  → Wi-Fi HTTP / 같은 로컬망
  → HostPC AI 서버: 발화 검출 → 로컬 Whisper → 로컬 LLM 분석
  → NU87: 명령 수락 / 왈라비 상태 갱신
  → Motion Lab: 인사 · 전진 · 회전 · 도킹 · 정지

HostPC → Wi-Fi → NU87 → BLE → NU40 OLED: 상태 표시 / matching ACK
HostPC → Wi-Fi → NU87 → 인증된 BLE SMP → NU40 MCUboot: 서명 이미지 OTA
NU40 BTN4 → NU40 MAX98357A + 스피커 경고음 / NU87 경고 상태 / 시뮬레이터 정지
```

외부망은 최초 패키지·모델 준비에만 필요합니다. 운용 중에는 로컬 모델 디렉터리만
읽으며 브라우저 SpeechRecognition, 클라우드 STT, CDN을 사용하지 않습니다.
PC/이어셋 마이크와 WAV 입력은 보드 연결 전 점검용 보조 입력입니다. 기본 입력은 NU40 PDM입니다.

## 빠른 실행

저장소 루트에서 Python 3.12 이상으로 실행합니다. 텍스트·보고·교범 검색·시뮬레이터에는
별도 Python 패키지가 필요하지 않습니다.

```sh
python3 -m voice_tactical.server
# 브라우저 자동 열기를 끄려면 --no-browser
# 기본 주소 http://127.0.0.1:8767
```

서버는 loopback에만 바인딩합니다. HostPC 브라우저에서 접속하세요.
NU87에는 브라우저가 직접 접속하지 않고 Python 서버가 토큰을 첨부합니다.
위 명령은 모델 없는 규칙 기반 데모입니다. 요청한 LLM 분석을 사용하려면 아래처럼
로컬 음성 모델과 LLM 서버를 함께 준비합니다.

### 로컬 음성 모델 준비 (최초 온라인 1회)

```sh
python3.12 -m venv voice_tactical/.venv
voice_tactical/.venv/bin/pip install -r voice_tactical/requirements.txt
voice_tactical/.venv/bin/python -m voice_tactical.prepare_model
```

`prepare_model`은 고정 revision의 `Systran/faster-whisper-small`을
`voice_tactical/models/whisper-small/`에 받습니다. 이미 준비한 CTranslate2 모델은
`--stt-model /absolute/path/to/model`로 지정합니다. `model.bin`, `config.json`,
`tokenizer.json`이 없으면 음성 인식을 비활성화하고 다운로드하지 않습니다.
검증 환경의 전체 패키지 버전은 `requirements-lock.txt`에 있습니다. 기본 의존성은
Python 3.12 / Apple Silicon에서 실행 확인했고 플랫폼별 wheel 지원은 설치 시 확인합니다.

```sh
voice_tactical/.venv/bin/python -m voice_tactical.server \
  --gateway '<NU87-IP>:8080' \
  --gateway-config firmware/nu87_lcd_gateway/local.conf \
  --llm-url http://127.0.0.1:11435/v1 --llm-model qwen2.5:7b \
  --auto-display
```

`--gateway-config` 또는 `NU87_GATEWAY_TOKEN` 환경변수로 기존 토큰을 사용합니다.
토큰은 UI, 음성 기록, 명령 인수에 넣지 않습니다. 데이터·모델·가상환경은 Git에서 제외됩니다.
`sh voice_tactical/start.command`도 동일 서버를 실행합니다.

## 시연 순서

1. 새 NU40 음성 앱과 NU87 게이트웨이를 준비하고 두 보드를 같은 개발 환경에 연결합니다.
2. 화면의 **BLE 목록 조회 → 장치 주소/종류 선택 → 선택 연결**을 수행합니다.
   다른 중앙 장치가 NU40에 연결돼 있다면 해제합니다.
3. 로컬 STT 준비와 NU87/NU40 상태를 확인합니다. 입력은 **NU 40DK · 키트 PDM 마이크**를
   선택한 상태에서 **마이크 켜기**를 누릅니다. 이 단계는 버튼 입력을 받을 전송 세션을 준비합니다.
4. NU40의 **BTN2**를 한 번 누르고 아래 예시 중 짧은 문장 하나를 말한 다음 **BTN3**를 누릅니다.
   BTN2는 보드 내부 녹음 시작, BTN3는 녹음 종료와 기존 BLE 오디오 경로로 전송입니다.
   최대 10초에 도달하면 녹음만 멈추고 BTN3를 기다립니다. 녹음한 음성은 BTN3 전에는
   전송하지 않습니다. 수신 화면은 계속 열어 두세요.
5. NU40의 **BTN4**를 눌러 폭발물 감지 상황을 모사합니다. NU40에서 경고음을 내고
   NU87과 시뮬레이터가 경고 상태로 정지하는지 확인합니다. **경고 해제** 후에만 이동을 재개합니다.
6. **전체 정지**는 녹음과 시뮬레이터를 함께 중단합니다. 숨겨진 탭, 연결 단절, 프레임 누락도
   입력/움직임을 중단합니다. 새로 마이크를 켜야 재개합니다.

버튼 기능은 NU40 펌웨어에만 추가했습니다. NU87 펌웨어와 HostPC 수신기는 기존 방식을
사용합니다. 연결 유지를 위한 무음 프레임은 대기·녹음 중에도 전송하지만 마이크의 녹음
내용은 포함하지 않습니다. 기존 수신기는 약 0.7초의 침묵을 문장 끝으로 판단하므로,
녹음 도중 길게 쉬면 첫 발화 처리 때 나머지 녹음이 취소될 수 있습니다. 버튼 입력은
짧은 명령 한 문장씩 사용하고 긴 보고는 기존 텍스트/WAV 입력을 사용하세요.
이전 응답 처리와 음성 읽어주기가 끝난 뒤 다음 BTN2 녹음을 시작하세요.

| 발화 | 결과 |
| --- | --- |
| 왈라비 인사해 | 기존 환영 동작 |
| 왈라비 앞으로 가 / 뒤로 가 / 왼쪽으로 돌아 / 오른쪽으로 돌아 | 최대 0.9초 이동 후 정지 |
| 왈라비 도킹해 | 기존 도킹 상태 전이 |
| 왈라비 정지 | 이동 중단 |
| 경고 해제 | NU40 경고 출력 해제 확인 후 NU87·시뮬레이터 경고 해제 |
| 상황 보고. 보고자: 지원팀. 시간: 14시 20분. 위치: 집결지. 인원: 4명. 관측: 장비 점검 완료. 요청: 추가 안내. | 요약, SALUTE·상황보고 필드, 원문 근거 |
| 상황 보고에 어떤 항목을 포함해야 해? | 로컬 훈련 교범 검색과 출처 표시 |
| 명령 정리. 지원팀은 15시까지 창고에서 장비를 점검한다. | 담당·내용·시간·장소 정리 |
| 기록 검색 장비 | 저장된 원문/응답 검색 |

보고 속 “앞으로 가라는 지시를 받았다”, 부정·질문·인용 표현은 동작 명령으로 실행하지 않습니다.
보고/명령 정리 모드에서는 동작을 실행하지 않습니다. 실물 바퀴나 무기를 제어하는 기능은 없습니다.
브라우저에서 실물 키트 상태와 가상 로봇 상태를 구분해서 표시합니다.

기본 응답은 로컬 교범 검색과 근거 보존 규칙 추출입니다. 실제 배포 교범 대신 저장소에 작성한
**훈련용 보고·통신 절차 예시**를 제공합니다. `knowledge/*.json`의 `id`, `title`, `text`,
선택 `tags` 필드로 교범을 추가하거나 `--knowledge-dir`로 별도 폴더를 지정합니다.
빠진 내용은 “미확인”으로 남깁니다. JSON 저장과 SQLite 기록은 HostPC에 저장됩니다.

HostPC에서 OpenAI 호환 로컬 LLM 서버를 실행합니다. 검증 환경에서는 기존에 설치된
Ollama와 `qwen2.5:7b` 모델을 사용했습니다. 모델 다운로드는 최초 준비 때만 수행합니다.
LLM 서버를 먼저 켜고, 별도 터미널에서 위의 음성 서버 명령을 실행합니다.

```sh
OLLAMA_NO_CLOUD=1 OLLAMA_HOST=127.0.0.1:11435 ollama serve
# 최초 온라인 준비 때, 다른 터미널에서:
OLLAMA_HOST=127.0.0.1:11435 ollama pull qwen2.5:7b
```

LLM 서버 주소는 숫자 loopback IP만 허용하며 proxy/redirect를 사용하지 않습니다.
LLM은 보고 요약·표준 필드 추출·명령 정리·교범 응답의 근거를 분석합니다. 원문에 있는
인용·필드만 수용하며 허용된 동작 목록은 규칙으로 검사합니다. 모델 장애·근거 불일치 시
규칙 기반 결과와 대체 처리 상태를 표시합니다. 정지와 BTN4 경고는 LLM 응답을 기다리지 않습니다.
응답 읽어주기는 HostPC에 설치된 로컬 한국어 TTS만 사용하며, NU40 스피커는 경고음을 담당합니다.

### BTN4 감지 모사와 경고

실제 폭발물 센서 없이 **BTN4(P1.05)** 입력을 감지 이벤트로 사용합니다. NU40는 통신이
끊겨도 즉시 스피커 경고음을 내며, 연결 중이면 BLE 이벤트로 NU87의 경고 상태를 설정합니다.
HostPC는 음성 입력을 꺼 놓았거나 LLM이 처리 중이어도 이벤트를 확인합니다. 경고 중에는
NU87이 이동 명령을 거부하고 시뮬레이터도 정지합니다. 경고음은 최대 10초 후 멈추지만
경고 상태는 유지됩니다. 해제 명령에 대한 NU40의 성공 ACK를 받아야 이동 차단을 해제합니다.
ACK는 드라이버 수락 확인이며 실제 소리가 났다는 측정 결과는 아닙니다.
보드 없이 확인하는 화면의 소프트웨어 감지 버튼은 명시적인 로컬 시뮬레이션입니다.

## NU40와 NU87 펌웨어 / OTA

NU40는 [기존 nu40-ble-display](https://github.com/mzsz94/nu40-ble-display)의
`230a1300d121b3bf25d3125a0cf247679148a40a`를 기반으로 OLED·ACK 호환성을 유지합니다.
**[NU40 음성 펌웨어 빌드·초기 설치·서명 안내](../firmware/nu40_voice/README.md)**를 따릅니다.

기본 PDM 프로필은 제조사 [PDM/SD 예제](https://github.com/Nu-makers/nucode_nu40_PDM_SD)의
`PDM.setPins(7, 5, -1)`과 NU40 Arduino pin map을 확인해 DIN=P0.07, CLK=P0.05로 설정했습니다.
키트 배선이 다르면 overlay를 변경합니다. OLED는 기존 키트와 같은 SDA=P0.22, SCL=P0.21입니다.
MAX98357A 배선은 DIN=P0.19, BCLK=P1.09, LRCK=P1.08, SD=P1.10입니다.
BTN4는 P1.05를 사용합니다. NeoPixel과 SD는 이번 미니 프로젝트에서 제어하지 않습니다.
NU87 빌드·음성·로봇·경고·OTA 경로는
[NU87 확장 안내](../firmware/nu87_lcd_gateway/VOICE-TACTICAL.md)를 확인합니다.

기존 OLED 전용 앱은 BLE OTA를 수신하지 않습니다. **첫 1회에는 호환 MCUboot와 서명 앱을
설치해야 하며, 이후 업데이트는 HostPC → NU87 → BLE SMP OTA를 사용합니다.** 현재 BARAM-NU40
레이아웃용 빌드이며 기존 S140/0x26000 레거시 DFU 이미지를 혼용하지 않습니다.
빌드 도구는 이미지를 생성하며 보드에 자동 기록하지 않습니다.

새 OTA 경로는 사설 pairing code를 설정한 BLE 인증(L3)과 MCUboot 서명 검증을 사용합니다.
NU87과 NU40에 같은 비공개 pairing code를 설정하고, 다음 업데이트에도 **동일한 서명 키**를
보존하세요. 최신 NU87 게이트웨이에 오디오와 SMP 중계가 포함되어야 합니다.
원래 NU87 테스트 포트의 난수/전송 보안 제한은 기존
[게이트웨이 안내](../docs/WIRELESS-LCD.md#현재-제한과-복구)에 따릅니다.

OTA 도구는 작업을 나누어 수행합니다. 업로드만으로 재부팅·확정하지 않습니다.

```sh
python3 -m voice_tactical.ota inspect build/nu40-voice/artifacts/nu40-update.signed.bin
python3 -m voice_tactical.ota --gateway '<NU87-IP>:8080' \
  --gateway-config firmware/nu87_lcd_gateway/local.conf status
python3 -m voice_tactical.ota --gateway '<NU87-IP>:8080' \
  --gateway-config firmware/nu87_lcd_gateway/local.conf upload build/nu40-voice/artifacts/nu40-update.signed.bin
# 출력된 image_hash (전체 파일 sha256과 다름)를 사용:
python3 -m voice_tactical.ota --gateway '<NU87-IP>:8080' --gateway-config firmware/nu87_lcd_gateway/local.conf test '<image_hash>'
python3 -m voice_tactical.ota --gateway '<NU87-IP>:8080' --gateway-config firmware/nu87_lcd_gateway/local.conf reset
# 재접속 후 status, 마이크/OLED 동작을 확인하고 같은 활성 image_hash만 확정:
python3 -m voice_tactical.ota --gateway '<NU87-IP>:8080' --gateway-config firmware/nu87_lcd_gateway/local.conf confirm '<image_hash>'
```

업데이트 중에는 음성 입력을 끕니다. 시험 부팅을 확정하지 않은 상태의 다음 리셋은 MCUboot의
복귀 동작을 사용합니다. 실제 전원 차단·무선 단절 복구는 보드에서 검증해야 합니다.

## 오디오 프로토콜과 운용 제한

KIT 서비스에 CONTROL `90e2c003-3c0d-4ef3-9d78-574cd1a64087`, AUDIO notify
`90e2c004-3c0d-4ef3-9d78-574cd1a64087`을 추가합니다. 최소 ATT MTU 179가 필요합니다.
기존 TEXT/ACK는 변경하지 않습니다. `firmware/voice_common/`의 코덱을 사용합니다.

176바이트 AUDIO 프레임은 `WA`, version 1, flags 0, little-endian session(u32),
sequence(u32), predictor(i16), IMA index(u8), reserved 0, 160바이트 ADPCM입니다.
처음 predictor와 다음 319샘플을 포함하며 low nibble부터 복호화합니다. 마지막 high nibble은0입니다.
매 프레임은 320샘플/20ms이고 독립적으로 복호화합니다. 0부터 연속인 sequence와 session을 확인합니다.

NU87 버퍼는64프레임이며 HTTP는 최대32프레임씩 전달합니다. 패킷 누락·중복·세션 불일치·시간 초과는
불완전한 발화를 폐기하고 마이크를 정지시킵니다. 재연결 후 이전 발화를 재생하지 않습니다.
HostPC는 에너지 기반 VAD(DC 제거, 약700ms 침묵, 최대12초)를 사용합니다. 추론하는 동안
마이크를 잠시 멈춘 뒤 새로운 session으로 재개하는 반이중 방식입니다. 따라서 처리 중에는
다음 명령을 말하지 말고 화면의 듣기 상태를 확인합니다. 잡음 환경의 VAD/STT 품질은 실물에서 조정해야 합니다.

## 검증

```sh
python3 -m unittest discover -s voice_tactical/tests -v
node --test simulation/*.test.mjs voice_tactical/web/*.test.mjs
bash firmware/nu87_lcd_gateway/tests/run_kit_tests.sh
python3 -m unittest discover -s firmware/voice_common/tests -v
python3 -m unittest discover -s firmware/nu87_lcd_gateway/tests -p 'test_*.py' -v
# intelhex==2.3.0이 설치된 Zephyr 환경에서:
python3 -m unittest discover -s firmware/nu40_voice/tests -v
```

로컬 HTTP·검색·표준 양식·출처 검증, 잘못된 WAV, offline-only STT 초기화, ADPCM·누락·세션 종료,
시뮬레이터 이동 제한과 정지, SMP 응답/이미지/업로드 실패를 검사합니다.
상세 결과는 [검증 기록](VALIDATION.md)에 기록합니다. 호스트 테스트, 합성 WAV 시험, 펌웨어 빌드와
실제 보드의 마이크·BLE throughput·OTA 복구 시험은 구분합니다.

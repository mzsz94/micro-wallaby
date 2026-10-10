# Linux / WSL 로컬 음성·LLM 실행

이 안내는 `test/military-wallaby`에서 분기한 작업의 Linux 실행 환경입니다.
운용 중에는 준비된 모델 파일과 loopback LLM 서버를 사용합니다. 외부망은 아래
최초 패키지·소스·모델 준비에만 필요합니다. 보드 연결은 별도의
[NU87 Linux 안내](../firmware/nu87_lcd_gateway/LINUX.md)를 따릅니다.

## 1. 한국어 음성 인식 준비

저장소 루트에서 실행합니다. Python 3.12, CMake, C/C++ 컴파일러가 필요합니다.

```bash
python3.12 -m venv voice_tactical/.venv
voice_tactical/.venv/bin/python -m pip install -r voice_tactical/requirements.txt
voice_tactical/.venv/bin/python -m voice_tactical.prepare_model
```

[faster-whisper](https://github.com/SYSTRAN/faster-whisper) 1.2.1과 PyAV 16.0.1을
사용합니다. Whisper small revision은
`536b0662742c02347bc0e980a01041f333bce120`입니다. 추론은 CPU int8이며 로컬 파일만
읽습니다. 한국어 인식 결과에도 오탈자가 생길 수 있으므로 시각·장소·인원은 원문을
재확인하세요. 결과의 **원문 수정**을 누르면 입력창에 복사합니다. 수정 후 보내기를
눌러야 새 기록으로 저장되며, 기존 기록은 유지됩니다.

STT가 보고의 첫 단어를 잘못 인식하면 자동 분류도 달라질 수 있습니다. 시연할 때는
**상황 보고** 또는 **명령 정리** 모드를 먼저 선택할 수 있습니다. 선택한 모드에서는
발화에 포함된 이동 표현을 로봇 명령으로 실행하지 않습니다.

## 2. llama.cpp와 한국어 LLM 준비

이 환경에서는 [llama.cpp](https://github.com/ggml-org/llama.cpp)의 고정 커밋과
[Qwen2.5-1.5B-Instruct GGUF](https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF)의
Q4_K_M 파일을 사용합니다. 기존 Ollama 7B 구성을 대체할 수 있는 CPU 실행 예입니다.
작은 양자화 모델의 응답 정확도는 별도 확인이 필요합니다.

```bash
mkdir -p build
git clone --no-checkout https://github.com/ggml-org/llama.cpp.git build/llama.cpp
git -C build/llama.cpp checkout --detach aa94f2086147cd4bfbcff818ce998dffce5fcb39
cmake -S build/llama.cpp -B build/llama-cpu \
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF -DGGML_VULKAN=OFF \
  -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF
cmake --build build/llama-cpu --parallel 6 --target llama-server
voice_tactical/.venv/bin/python - <<'PY'
from huggingface_hub import hf_hub_download
hf_hub_download(
    'Qwen/Qwen2.5-1.5B-Instruct-GGUF',
    'qwen2.5-1.5b-instruct-q4_k_m.gguf',
    revision='91cad51170dc346986eccefdc2dd33a9da36ead9',
    local_dir='voice_tactical/models/qwen2.5-1.5b',
)
PY
```

첫 터미널에서 로컬 모델 서버를 실행합니다. 모델 이름 대신 파일 경로를 지정하므로
서버 시작 시 모델을 다운로드하지 않습니다. 바인딩은 loopback입니다.

```bash
build/llama-cpu/bin/llama-server \
  -m voice_tactical/models/qwen2.5-1.5b/qwen2.5-1.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 11435 --alias wallaby-qwen2.5-1.5b \
  --ctx-size 4096 --threads 6 --parallel 1 --no-ui
```

## 3. HostPC 앱 실행

두 번째 터미널에서 실행합니다. 보드 없이 로컬 음성·보고·시뮬레이션을 확인하는 명령입니다.

```bash
HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
voice_tactical/.venv/bin/python -m voice_tactical.server --no-browser \
  --llm-url http://127.0.0.1:11435/v1 --llm-model wallaby-qwen2.5-1.5b \
  --llm-structured
```

브라우저에서 <http://127.0.0.1:8767>을 엽니다. **HostPC · PC / 이어셋 마이크**를 선택하거나
**WAV 불러오기**를 사용하세요. WAV는 16 kHz 모노 PCM16, 최대 30초입니다.
NU40의 현재 마이크 연결 확인 범위는 [실기 기록](../docs/demo/HARDWARE-CHECK.md)에 있습니다.

NU87과 OLED도 연결하려면 같은 명령에 아래 인수를 추가합니다. 주소는 실제 DHCP 값으로
바꾸세요. 이전 기록을 계속 쓰려면 같은 `--data-dir`을 지정합니다.

```text
--gateway 192.168.1.10:8080
--gateway-config firmware/nu87_lcd_gateway/local.conf
--auto-display
--data-dir /absolute/path/to/existing/data
```

비밀번호와 HTTP 토큰은 인수가 아닌 Git 제외 `local.conf`에서 읽습니다. 서버가 성공
응답을 받으면 결과 아래에 **OLED 드라이버 ACK 수신**을 표시합니다. 실제 픽셀은 키트를
보고 확인해야 합니다. 한글 본문은 HostPC에, OLED에는 ASCII 상태 카드를 표시합니다.

LLM을 끄려면 `--llm-url`, `--llm-model`을 생략합니다. 음성 인식·규칙 기반 보고·로컬
교범 검색은 계속 사용할 수 있습니다. LLM 응답이 원문 근거 검사에 실패하면 해당 결과는
규칙 기반으로 대체됩니다. “로컬 LLM 설정됨”은 주소가 설정됐다는 뜻이며, 실제 처리에
사용됐는지는 결과의 **LOCAL LLM** 표기와 `engine` 필드로 확인합니다.

`--llm-structured`는 JSON Schema를 지원하는 서버에만 사용합니다. 문장 선택과 출력
개수를 제약하며, 원문 대조·불확실성 보존 검사는 별도로 수행합니다. 기본 호환 모드는
JSON object입니다. CPU 추론을 위해 교범 근거 선택과 보고 분석은 각각 최대 60초를
기다립니다. 정지·경고 처리는 이 대기를 거치지 않습니다.

브라우저 읽어주기는 `localService=true`인 한국어 음성만 사용합니다. Windows Heami가
설치돼 있어도 WSL의 headless Chromium에는 그 음성이 노출되지 않을 수 있습니다.
Windows 브라우저에서 확인하고, 로컬 한국어 음성이 없으면 읽어주기를 비활성화합니다.

## 확인 명령

```bash
voice_tactical/.venv/bin/python -m unittest discover -s voice_tactical/tests
node --test simulation/*.test.mjs voice_tactical/web/*.test.mjs
WALLABY_PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs \
  node voice_tactical/tests/browser-smoke.mjs
```

브라우저 검사는 별도 서버·임시 SQLite를 사용합니다. 원문 복사만으로 처리하지 않는지,
선택한 보고 모드를 유지하는지, 수정 전·후 기록을 함께 보존하는지 확인합니다.
“훈련구역탐색 시작해.”로 실제 시뮬레이터 경로·머리 움직임이 시작되는지, 정지·경고가
중단시키는지, 교범 질의 모드와 꺼진 동작 체크박스가 이동을 실행하지 않는지도 검사합니다.
이미 실행 중인 앱의 데이터와 보드에는 접근하지 않습니다.

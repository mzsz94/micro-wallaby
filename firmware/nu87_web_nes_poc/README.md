# NU-87 Wi-Fi 웹 NES PoC

GitHub issue #35를 위한 독립 Zephyr 애플리케이션이다. NU-87은 조이스틱을
읽고 로컬 HTTP 페이지를 제공하며 최신 입력을 WebSocket으로 전송한다. NES
에뮬레이션과 화면 표시는 브라우저에서 실행한다. MCU에서 NES를 직접
에뮬레이션하는 것은 이번 단계의 필수 범위가 아니다.

이 코드는 통합 검증용 Spike이며 microWallaby 제품 제어 경로가 아니다.
모터를 구동하지 않고 터치 모듈도 사용하지 않는다.

## 구조와 선택 근거

- ADC/GPIO는 100 Hz 절대 주기로 읽고, 별도 스레드가 50 Hz로 WebSocket을
  보낸다. 느린 네트워크가 입력 샘플링 주기를 막지 않게 하기 위한 분리다.
- Ameba의 connect/disconnect 호출은 동기식이므로 전용 preemptible workqueue에서
  직렬화한다. system workqueue의 독립 timeout과 generation/phase 상태, disconnect
  결과 barrier로 늦게 도착한 이전 연결 이벤트가 새 시도를 완료 처리하지 못하게
  한다. vendor connect 자체는 강제 중단할 API가 없어 timeout 뒤 함수가 반환하는
  즉시 disconnect하는 한계가 있다.
- HTML과 JavaScript는 gzip 압축된 `static const` 데이터로 XIP flash에
  둔다. 에뮬레이터의 큰 작업 메모리는 브라우저가 부담한다.
- ROM은 브라우저 파일 선택기로만 열고 iNES 형식과 1 MiB 상한을 확인한다.
  보드로 업로드하거나 저장하거나 외부 서버에서 받지 않는다.
- 기존 `nu87_io_bringup`은 터치를 안전 입력으로 요구한다. 조이스틱 전용
  실험을 별도 앱으로 둬 기존 안전 모델을 약화하지 않는다.

## 보안 경계

이 펌웨어는 **인터넷 및 메인 가정 LAN과 분리된 시험망 전용**이다.

- 현재 pinned AmebaD 포트에는 검증된 하드웨어 entropy 구현이 없다. Wi-Fi
  binary ABI를 위해 복원한 ROM `RandBytes_Get()`도 timer 기반이라 CSPRNG로
  간주할 수 없다.
- 기본 설정은 비밀번호를 담지 않는 OPEN 시험 AP다. OPEN Wi-Fi와 평문
  HTTP/WebSocket에는 기밀성과 인증이 없다.
- WPA2 시험이 꼭 필요하면 폐기 가능한 별도 SSID/PSK만 쓰고
  `CONFIG_MW_ALLOW_INSECURE_TEST_PSK=y`를 명시한다. 시험 뒤 PSK를 폐기한다.
- 메인 가정 Wi-Fi 비밀번호, WPA3-SAE, EAP, 인터넷 공개 서버에는 사용하지
  않는다. 실제 entropy와 TLS가 검증되기 전에는 제품 보안 경로가 아니다.

## 조이스틱 배선

선을 바꾸기 전에 USB 전원을 분리한다.

| 조이스틱 표기 | NU-87 | 용도 |
| --- | --- | --- |
| `GND` | `GND` | 공통 접지 |
| `5V` | **`3V3`** | 모듈 전원. NU-87 입력에 5 V를 넣지 않는다. |
| `VRx` | `PB1 / ADC4` | 좌우 축 |
| `VRy` | `PB2 / ADC5` | 상하 축 |
| `SW` | `PB22` | 내부 pull-up을 쓰는 active-low 버튼 |

이번 polling 시험에는 외부 10 kohm 저항이 필요 없다. 터치 모듈은 연결하지
않는다.

## Wi-Fi 설정

west workspace 루트에서 예제를 Git 제외 파일로 복사한다.

```sh
cp micro-wallaby/firmware/nu87_web_nes_poc/wifi.example.conf \
  micro-wallaby/firmware/nu87_web_nes_poc/wifi.conf
```

`wifi.conf`의 SSID를 격리된 2.4 GHz 시험 AP로 바꾼다. 이 파일은 Git에서
제외되지만 생성된 `.config`와 펌웨어 이미지에는 설정값이 남으므로 빌드
산출물도 공개하지 않는다.

## 테스트와 빌드

west workspace 루트에서 다음 순서로 준비한다.

```sh
west update
./micro-wallaby/tools/apply_west_patches.sh

make -C micro-wallaby/firmware/nu87_web_nes_poc/tests/host test
node --test micro-wallaby/firmware/nu87_web_nes_poc/web/tests/*.test.js
```

patch 적용 스크립트는 pinned `hal_realtek`과 Zephyr에 다음 최소 호환 수정을
멱등하게 적용한다.

- 누락된 AmebaD Wi-Fi PMU/IPC/log/random/EFUSE ABI 공급
- single-core Wi-Fi archive가 호출하는 Zephyr RX 심볼 export
- 순환 참조가 있는 Wi-Fi archive 전체를 linker group으로 묶음
- no-blob 빌드에서는 binary 전용 glue 제외

PMU shim은 실제 Zephyr 저전력 연동 대신 always-awake 동작을 유지한다. 따라서
전력 측정이나 sleep 기능의 근거로 쓰면 안 된다. `west update`가 외부 모듈을
갱신한 뒤에는 patch 스크립트를 다시 실행한다.

독점 blob 없이 API와 resource linking만 검사하는 빌드는 다음과 같다. 이
이미지로 Wi-Fi 실기 판정을 하면 안 된다.

```sh
west build -b nucode_nu87 micro-wallaby/firmware/nu87_web_nes_poc \
  -d build/nu87-web-nes-noblob -p always -- \
  -DCONFIG_BUILD_ONLY_NO_BLOBS=y
```

실제 Wi-Fi 이미지는 Realtek blob과 로컬 설정을 포함한다.

```sh
west blobs fetch hal_realtek
python -m pip install -r modules/hal/realtek/ameba/scripts/requirements.txt
python -m pip install 'python-mbedtls==2.10.1'

NU87_SDK=/absolute/path/to/zephyr-sdk-1.0.1
NU87_SDK_COMPAT=$(mktemp -d /tmp/mw-sdk-compat.XXXXXX)
ln -s "$NU87_SDK/gnu/arm-zephyr-eabi" \
  "$NU87_SDK_COMPAT/arm-zephyr-eabi"

ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR="$NU87_SDK_COMPAT" \
west build -b nucode_nu87 micro-wallaby/firmware/nu87_web_nes_poc \
  -d build/nu87-web-nes -p always -- \
  -DZEPHYR_SDK_INSTALL_DIR="$NU87_SDK" \
  -DCMAKE_GDB="$NU87_SDK/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb" \
  '-DEXTRA_CONF_FILE=program.conf;wifi.conf'
```

임시 SDK 경로는 pinned Realtek image merger가 예전 SDK 디렉터리 구조를
기대하는 문제를 우회한다.

## 이미지 확인과 플래시

먼저 `micro-wallaby` 저장소 루트에서 쓰기를 하지 않는 dry-run으로 두 이미지와
주소를 확인한다.

```sh
python3 tools/nu87_flash/nu87_flash.py \
  --build-dir ../build/nu87-web-nes \
  --port /dev/cu.usbserial-10
```

그 다음 BOOT을 누른 채 RESET을 눌렀다 놓고 BOOT을 놓아 ROM download mode에
들어간 뒤에만 `--write`를 추가한다. 로컬 `wifi.conf`를 넣지 않은 placeholder
이미지는 보드에 쓰지 않는다.

```sh
python3 tools/nu87_flash/nu87_flash.py \
  --build-dir ../build/nu87-web-nes \
  --port /dev/cu.usbserial-10 --write
```

부팅 로그의 DHCP 주소를 보고 같은 격리 시험망의 브라우저에서
`http://<address>:8080/`을 연다. 상태 JSON은 `/api/status`에서 확인한다.

## 입력·진단 프로토콜

보드는 다음 JSON을 50 Hz로 보낸다.

```json
{"v":1,"seq":42,"sample_seq":84,"sample_ms":12340,"sent_ms":12345,"x":-1000,"y":720,"sw":true,"ok":true}
```

`seq`는 WebSocket 송신 시도 번호다. 포맷/송신 실패에서도 증가하고 브라우저는
재연결 때 값을 초기화하지 않으므로 다음 수신 frame에서 미수신 시도를 계산한다.
`sample_seq`는 100 Hz ADC/GPIO 표본 번호다. `x`, `y`는 `-1000..1000`, `sw`는
NES A로 변환한다. 브라우저는 450/250 engage/release hysteresis를 적용하고
펌웨어는 100 ms보다 오래된 표본을 `ok:false`와 중립 입력으로 바꾼다. 브라우저도
250 ms 동안 새 데이터가 없으면 하드웨어 입력을 모두 해제한다. 따라서 sampler와
전송 경로 중 어느 한쪽이 멈춰도 마지막 버튼을 계속 누른 상태로 두지 않는다.
화면과 키보드는 B/Start/Select를 보완하며, Y축 반전을 선택할 수 있다. 표준
WebSocket PING에는 PONG으로 답하고, 브라우저가 보내지 않아야 할 application
data는 연결을 닫아 protocol 상태를 단순하게 유지한다.

`/api/status`는 uptime, Wi-Fi/WebSocket 상태, 전송 수/오류, stale 입력 frame,
입력 표본/오류, deadline miss, 현재와 최저 system heap 여유를 제공한다. heap
통계는 전체 SRAM이 아니라 Zephyr system heap만 뜻한다. 현재 pinned Zephyr에는
동기화된 공개 system-heap 통계 API가 없어 `_system_heap` 내부 lock을 짧게 잡는
target-specific 구현이며, Zephyr revision을 바꿀 때 다시 검토해야 한다. 화면의 queue,
상대 전송 지연, HTTP RTT는 진단 추정치이며 실제 버튼-화면 종단 지연값이
아니다. 100 ms 목표 판정은 실기 시험 계획의 고속 촬영 절차를 사용한다.

## MCU 자체 NES 에뮬레이션 판단

현재 결론은 **조건부 가능성이 있으나 아직 검증되지 않았고 M0에서는 진행하지
않음**이다.

- pinned DTS의 Cortex-M33 260 MHz와 현재 linker RAM 여유는 compact NROM
  core를 검토할 출발점은 된다.
- 그러나 Wi-Fi 연결 뒤 heap 최저값과 emulator CPU/frame time을 아직 실기로
  측정하지 않았다. 현재 system heap 설정도 64 KiB다.
- 256×240 화면을 60 fps로 그대로 보내면 8 bpp도 약 3.7 MB/s, RGB565는 약
  7.4 MB/s이므로 raw streaming은 부적합하다.

후속 단계는 실제 Wi-Fi soak 결과를 먼저 확보한 뒤, 재배포 가능한 NROM으로
CPU/frame-time·RAM benchmark를 하고 tile delta 또는 압축 전송을 별도
Spike에서 검증해야 한다.

## ROM과 오디오 제한

- Nintendo, Mario, 상용 ROM, ROM URL, save data, 게임 이미지를 저장소나
  빌드에 포함하지 않는다.
- 자동 시험은 메모리에서 40,976-byte synthetic NROM을 생성해 pinned JSNES
  2.1.0에 한 frame을 실행한다. `.nes` 파일은 만들지 않는다.
- 개인 실기는 시험자가 적법하게 보유한 iNES 1.0 파일만 로컬에서 선택한다.
- 오디오는 껐다. 평문 LAN origin에서는 JSNES의 AudioWorklet 경로를 안정적으로
  사용할 수 없어 HTTPS/audio는 후속 범위다.

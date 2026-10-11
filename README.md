# microWallaby

## Military Wallaby · 밀리터리 왈라비

**[실제 NU87 IMU → NU40 OLED 데모](docs/demo/LIVE-IMU.md)** — 실제 센서를 기울여
자세 표시 → 기울기 경고·가상 정지 → 복귀·해제까지 확인했습니다.
[실측 GIF](docs/demo/media/07-live-imu.gif) · [MP4](docs/demo/media/military-wallaby-live-imu.mp4).
실측 화면은 `http://127.0.0.1:8767/hardware.html`입니다.
표시 자세는 부드럽게 보간하고 원본 데이터·경고 판정은 유지합니다.

**[상황별 데모 GIF·MP4 보기](docs/demo/README.md)** — 교범 조회, 상황 보고, 지시 정리,
훈련 경고 시 정지, 탐색·모의 폭발물 발견·컨트롤 타워 보고를 60초로 보여 줍니다.
상황에 맞춰 왈라비의 표정·관절·바퀴와 실제 모델 이동 궤적이 바뀝니다.
음성 입력은 자막 연출이며 실제 로컬 응답과 Motion Lab 시뮬레이션을 사용합니다.

**[프로젝트 보고서](docs/military-wallaby-report.md)** — 목적, 기술스택, 해결 과제와
NU87 중계 → BLE SMP → NU40 펌웨어 OTA 구성 및 검증 범위를 정리했습니다.

[Linux / WSL 음성·LLM 실행](voice_tactical/LINUX.md) ·
[NU87 Wi-Fi · NU40 BLE 실기 확인](docs/demo/HARDWARE-CHECK.md)

**[실제 로컬 음성 처리 GIF·MP4](docs/demo/VOICE-CHECK.md)** — 합성 한국어 WAV → Whisper →
llama.cpp → NU87 Wi-Fi → NU40 BLE 표시 ACK를 별도로 확인했습니다. 음성 오인식도 기록합니다.

`test/military-wallaby`는 **NU40DK 컨트롤 타워의 PDM 마이크 → BLE → NU87 Tiny 왈라비
→ Wi-Fi → HostPC 로컬 Whisper·LLM 서버 → NU87 명령 수락 → Motion Lab 동작**을
연결하는 미니 프로젝트입니다. 음성 질의, 상황 보고 요약·SALUTE 변환, 명령 정리,
로컬 기록 검색을 제공합니다. BTN4로 폭발물 감지를 모사하면 NU40의 MAX98357A·스피커가
경고음을 내고 NU87과 시뮬레이터가 경고 상태로 정지합니다.

```sh
python3 -m voice_tactical.server
# http://127.0.0.1:8767
```

모델 없는 텍스트 시연부터 시작할 수 있습니다. NU40 마이크를 사용하려면 음성 펌웨어,
로컬 Whisper·LLM 모델, NU87 연결 설정이 필요합니다. 새 NU40 펌웨어는 서명된 MCUboot
이미지를 BLE SMP로 OTA 업데이트합니다. 기존 OLED 전용 앱에는 OTA 수신 기능이 없으므로
초기 MCUboot 설치가 선행되어야 합니다.

**[실행·음성 시연·보드 연결·OTA 안내](voice_tactical/README.md)** ·
[NU40 음성 펌웨어](firmware/nu40_voice/README.md) ·
[기존 동작 시뮬레이터](simulation/README.md)

## NU40-DK BLE 오늘의 성장 놀이

Apple Silicon(M1) Mac에서 IDE 없이 NU40-DK, LSM6DS3TR IMU, 터치 센서,
Web Bluetooth 웹앱으로 0–5세 보호자 동반형 센서 놀이를 설계하고 실행하는
프로젝트입니다. 현재 실제 게임 규칙과 하드웨어 검증 범위는 문서에 따로 표시합니다.

- 보드: 센서를 10 Hz로 읽고 BLE 알림으로 전송
- 웹앱: 부모 프롬프트와 오늘 날짜로 색·수·모양·감정 놀이 생성
- 조작: 마우스·키보드 또는 IMU·터치·아날로그 조이스틱
- 피드백: 네 번의 성공을 NU40-DK LED0~LED3에 반영
- 학습 문서: [`docs/`](docs/README.md)

## NU40·NU87 IMU 실시간 대시보드

별도 Zephyr 펌웨어와 로컬 USB 브리지로 가속도·각속도·보드 기울기를 시각화합니다.
NU87은 I2C 통신 수정, 맥북 측 자이로 보정, 바닥 자세 기준 및 센서 장착 축 정렬을
포함합니다. NU87의 실제 검증 상태와 UART 셸 제한은 아래 문서에 기록했습니다.

- [대시보드 실행·보정 안내](web/IMU-DASHBOARD.md)
- [NU40 펌웨어](firmware/nu40_imu_bringup/README.md)
- [NU87 펌웨어·빌드·플래싱](firmware/nu87_imu_bringup/README.md)
- [NU87 하드웨어 검증 결과](firmware/nu87_imu_bringup/HARDWARE-RESULT.md)

현재 NU87 및 센서의 보정값은 `firmware/nu87_imu_bringup/calibration/`에 보관합니다.
SDK·의존성·펌웨어 바이너리와 원본 연속 로그는 포함하지 않으며 제공된 스크립트로
환경을 구성하고 다시 빌드합니다.

## NU87 Wi-Fi / NU40-KIT BLE 화면 제어

`bugfix/zephyr-ble` 브랜치는 NU87 Wi-Fi 게이트웨이에서 기존 NU40-KIT-DISPLAY 앱으로
문자열을 전송합니다. 테스트 Wi-Fi 연결, 인증 HTTP 응답, NU87↔NU40 BLE 연결,
웹 경유 텍스트 전송과 NU40의 성공 ACK를 실기기로 확인했습니다.
로컬 제어 화면은 `python3 web/tools/lcd_bridge.py`로 실행합니다.
현재 KIT 앱은 OTA를 지원하지 않으며 NU40 펌웨어는 변경하지 않았습니다.

별도의 `nu40_ble_lcd` 앱과 Adafruit/S140 레거시 DFU 예제도 포함합니다.
이 예제는 현재 KIT 앱과 프로토콜·플래시 레이아웃이 다르며, OTA 전송은 실기기 미검증입니다.
타이머 래퍼 수정은 [Zephyr PR #121773](https://github.com/zephyrproject-rtos/zephyr/pull/121773)로
제출했으며, HAL 공존 보고 보완은 이 저장소의 앱 호환 계층에만 반영되어 있습니다.

- [구성, 빌드, 제어 화면과 OTA 실행 안내](docs/WIRELESS-LCD.md)
- [현재 NU40-KIT-DISPLAY 앱과 연결](firmware/nu87_lcd_gateway/KIT-DISPLAY.md)
- [NU40 화면 펌웨어와 디스플레이 프로필](firmware/nu40_ble_lcd/README.md)
- [레거시 DFU 전송 구현과 복구 제한](firmware/wireless_lcd_common/dfu/README.md)

## 확인된 보드 상태

- 보드: NUCODE NU40-DK / nRF52840
- USB 제품명: `PCA10056`
- UF2 볼륨: `/Volumes/NRF52BOOT`
- 부트로더: Adafruit nRF52 UF2 0.9.1
- Board-ID: `nRF52840-pca10056-v1`
- SoftDevice: S140 6.1.1

이 부트로더는 현재 Zephyr의 기본 `BARAM-NU40` 메모리 배치와 다릅니다.
따라서 `boards/nucode_nu40_nrf52840.overlay`에서 애플리케이션을
`0x26000`부터 배치하며, 모든 빌드와 플래시 전에 UF2 주소가
`0x26000..0xED000` 범위인지 자동 검사합니다.

## 최초 설치

```bash
make deps
make setup
```

`make deps`는 Homebrew로 CMake, Ninja, devicetree compiler, ccache,
Python 3.12를 설치합니다. `make setup`은 이 폴더 안에 Zephyr 소스,
NU40-DK에 필요한 CMSIS/CMSIS 6/Nordic HAL, 빌드용 Python 패키지, Python
가상환경, ARM Zephyr SDK만 설치합니다. Zephyr 버전은
`.zephyr-revision`의 검증된 커밋으로 고정됩니다.

## 빌드와 플래시

```bash
make detect
make build
make flash-check
make flash
```

- `make detect`: 연결된 보드와 부트로더 호환성을 확인합니다.
- `make build`: `nucode_nu40/nrf52840` 대상으로 센서 모니터를 빌드합니다.
- `make flash-check`: UF2와 연결된 보드를 검사하되 기록하지 않습니다.
- `make flash`: 현재 펌웨어를 `backups/`에 복사한 다음 검증된 UF2를 전송합니다.

빌드 결과는 `build/zephyr/zephyr.uf2`입니다.

## IMU·터치 센서와 BLE

- IMU: NU-IMU-LSM6DS3TR, I2C0 `SDA=P0.26`, `SCL=P0.27`
- 터치 D0: `P0.02`
- 터치 A0: `P0.03/AIN1`
- 조이스틱 X: `P0.04/AIN2`
- 조이스틱 Y: `P0.28/AIN4`
- 조이스틱 버튼: `P1.03`, 내부 풀업·Active Low
- 샘플 주기: 100 ms
- BLE 장치명: `MZSZ-TOY`

펌웨어가 실행 중일 때 다음 명령으로 USB 시리얼 값을 확인합니다.

```bash
make monitor
```

종료는 `Ctrl+C`입니다. `D0`는 터치 디지털 상태, `ADC`와 `mV`는 터치
아날로그 값입니다. `JX/JY/JSW`는 조이스틱, `AX/AY/AZ`는 `mg`,
`GX/GY/GZ`는 `mdps` 단위입니다.

## 오늘의 게임 웹앱

```bash
make web-install  # 최초 한 번
make web-dev
```

Chrome 또는 Edge에서 `http://localhost:3000`을 엽니다. 부모 프롬프트로
오늘의 게임을 만든 뒤 마우스 또는 방향키/Enter로 먼저 시험할 수 있습니다.
실제 리모컨을 사용하려면 `Bluetooth 연결`을 누르고
`MZSZ-TOY`를 선택합니다.

```bash
make web-build
```

웹 소스는 `web/`, 상세 구조와 프로토콜은 `docs/`에 있습니다.

UF2 볼륨이 보이지 않으면 NU40-DK의 RESET을 빠르게 두 번 누른 후 다음을
확인합니다.

```bash
make detect
```

## Zephyr 명령 직접 사용

환경 변수와 경로 설정은 `scripts/west.sh`가 처리합니다.

```bash
./scripts/west.sh build --pristine=always \
  -b nucode_nu40/nrf52840 \
  -d build \
  .
```

플래시는 `west flash` 대신 아래 명령을 사용합니다.

```bash
./scripts/flash.sh build/zephyr/zephyr.uf2
```

현재 Zephyr 보드 정의의 UF2 runner는 `BARAM-NU40` 볼륨을 전제로 하지만,
이 보드에는 레거시 `NRF52BOOT` 부트로더가 있기 때문입니다.

## 셸 환경 활성화(선택)

```bash
source .venv/bin/activate
export ZEPHYR_BASE="$PWD/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$PWD/.zephyr-sdk"
```

종료할 때는 `deactivate`를 실행합니다.

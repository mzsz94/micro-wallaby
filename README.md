# NU40-DK BLE 오늘의 성장 놀이

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

# NU40 BLE LCD endpoint

독립 NU40 BLE 주변장치 앱입니다. BLE 이름은 `MZSZ-LCD`이며 NU87 게이트웨이와
`../wireless_lcd_common/include/lcd_protocol.h`의 20바이트 명령 / 8바이트 완료 상태를
공유합니다. 기존 루트 놀이 앱과 IMU 앱은 수정하지 않습니다.

## 하드웨어 프로필

기본 빌드는 **LCD 미설정**입니다. BLE 상태 확인은 가능하지만 LCD에 명령을
기록하지 않습니다. 제품 크기나 I2C 주소만으로 컨트롤러를 자동 추정하지 않습니다.
아래는 선택 가능한 예제이며 실제 연결 모듈의 확인 결과가 아닙니다.

| 프로필 | 필요한 확인 | 텍스트 크기 | 예제 주소 |
| --- | --- | --- | --- |
| `ssd1306-example` | SSD1306, 128×32, 내부 charge pump, I2C | 16열 × 4행 | `0x3C` |
| `pcf8574-example` | HD44780 + PCF8574, 아래 백팩 비트 배치 | 16열 × 2행 | `0x27` |

두 예제의 NU40 핀은 SDA=P0.26, SCL=P0.27, I2C 100 kHz입니다.
실제 핀, 전원 전압, I2C 신호 전압, 컨트롤러, 해상도/문자 수와 주소를 확인한 뒤
프로필을 사용해야 합니다. 전원을 끈 상태에서 배선합니다. 이 앱은 화면 종류를
감지하거나 I2C 주소를 스캔하지 않습니다.

SSD1306 예제는 8×8 셀에 5×5 ASCII 글꼴을 표시합니다. `BACKLIGHT`는 OLED의
화면 켜기/끄기 명령으로 동작하며 픽셀 데이터는 유지합니다. SH1106 등 다른
컨트롤러, 128×64 패널, 별도 RESET 핀이 필요한 모듈은 이 프로필의 대상이 아닙니다.

PCF8574 예제 비트는 P0=RS, P1=RW, P2=E, P3=백라이트(active high), P4..P7=DB4..DB7입니다.
RW는 항상 LOW이며 busy 플래그를 읽지 않습니다. `pcf8574-example.conf`에서
주소, 문자 크기와 비트 배치를 명시적으로 바꿀 수 있습니다.

문자는 ASCII `0x20..0x7E`만 허용합니다. 한글/UTF-8, 자동 줄바꿈은 지원하지 않습니다.
행/열은 0부터 시작합니다. 1회 WRITE는 최대 14자이며 행을 넘지 않아야 합니다.

## 빌드 및 검증

저장소 루트에서 실행합니다. 빌드 스크립트는 **플래시하지 않습니다**.

```sh
make -C firmware/nu40_ble_lcd/tests test
bash firmware/nu40_ble_lcd/build.sh
bash firmware/nu40_ble_lcd/build.sh ssd1306-example
bash firmware/nu40_ble_lcd/build.sh pcf8574-example
```

각 이미지 디렉터리는 `build/nu40-ble-lcd`, `build/nu40-ble-lcd-ssd1306-example`,
`build/nu40-ble-lcd-example`입니다. 마지막에 기존 IMU 플래셔의 `--verify-only`로
UF2 주소, 벡터, 연속 블록과 체크섬을 검증합니다.

현재 이 앱의 플래시 레이아웃은 **NRF52BOOT / Adafruit 0.9.1 / S140 6.1.1**용입니다.
앱은 `0x26000..0xDC000`, 기존 교정 NVS는 `0xDC000..0xE0000`입니다. RC 저속 클록을
사용합니다. BARAM-NU40 / SoftDevice 없음 보드용 이미지는 아닙니다.
UF2 기록 전 실제 `INFO_UF2.TXT`와 일치하는지 확인해야 합니다.

확인된 예제 이미지를 기존 플래셔로 사전 검사하는 명령:

```sh
python3 firmware/nu40_imu_bringup/flash.py --layout legacy \
  --image build/nu40-ble-lcd-ssd1306-example/zephyr/zephyr.uf2
```

실제 기록은 해당 플래셔의 `--write`로 수행하며 보드 정보를 검사하고 원본을 백업합니다.
본 구현 작업에서는 보드 기록 및 화면/BLE 실측 검증을 수행하지 않았습니다.

## 명령 실행과 상태

GATT 쓰기 성공은 작업을 큐에 넣었다는 뜻입니다. `STATUS`(op 0)는 하드웨어를
변경하지 않고 최신 준비 상태와 크기를 반환합니다. 나머지 LCD 명령도 큐의
작업 스레드에서 실행한 뒤 같은 명령/시퀀스의 완료 상태를 읽기/알림 특성에 기록합니다.
게이트웨이는 이 완료 상태를 기다려야 합니다. 준비되지 않은 화면을 성공으로 보고하지 않습니다.

오프셋 쓰기, prepared write, 잘못된 길이/문자/행/열은 GATT 오류로 거부합니다.
연결이 끊기면 그 연결의 대기 명령은 폐기하며 새 연결에서 재생하지 않습니다.
I2C 오류 후에는 부분 기록 가능성을 보존하여 READY를 내리고 후속 기록을 중단합니다.
하드웨어를 확인한 뒤 재부팅하여 다시 초기화합니다.

## 레거시 BLE DFU 진입

`CONFIG_LCD_LEGACY_DFU=n`이 기본입니다. 이때 `ENTER_DFU`는
`LCD_RESULT_DFU_DISABLED`를 반환합니다. 이 앱에는 MCUboot/MCUmgr 이미지 슬롯이나
자체 펌웨어 전송기가 없습니다.

실제 보드의 Adafruit 0.9.1 + S140 OTA 동작과 USB 복구 경로를 확인한 이후에만
`CONFIG_LCD_LEGACY_DFU=y`를 명시적으로 설정합니다. 예를 들어 별도의 빌드에서:

```sh
scripts/west.sh build --pristine=always --board nucode_nu40/nrf52840 \
  --build-dir build/nu40-ble-lcd-dfu-verified firmware/nu40_ble_lcd -- \
  -DDTC_OVERLAY_FILE=app.overlay -DEXTRA_CONF_FILE=ssd1306-example.conf \
  -DCONFIG_LCD_LEGACY_DFU=y
```

이 설정으로 컴파일했다는 사실은 보드 검증을 대체하지 않습니다.
`ENTER_DFU`의 `DFU!`는 우발적인 명령을 줄이는 표식이며 인증 수단이 아닙니다.
상태 알림 구독과 전송 완료를 확인한 뒤에만 GPREGRET=0xA8 및 시스템 리셋으로
레거시 부트로더에 들어갑니다. 알림 실패/시간 초과 시 리셋하지 않습니다.

이후 NU87 또는 호스트의 **레거시 Nordic/Adafruit DFU 전송기**가 앱 전용 패키지를
전송해야 합니다. SoftDevice/부트로더 패키지는 이 앱의 OTA 대상이 아닙니다.
레거시 부트로더는 MCUboot 서명/rollback 기능을 제공하는 것으로 간주하지 않습니다.
OTA의 삭제/스테이징이 NVS를 보존하는지는 별도로 검증해야 합니다. UF2 주소 검증만으로
OTA의 NVS 보존을 보장하지 않습니다.

근거: [Adafruit 0.9.1 README](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/README.md),
[0.9.1 리셋 진입](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/src/main.c).

## 테스트와 글꼴

호스트 테스트는 PCF8574 초기화 15개, SSD1306 초기화 22개의 각 I2C 전송 실패 지점,
타이밍/제어 바이트, ASCII, 경계, 부분 전송 오류와 명령 파서를 검증합니다.
BLE 무선 연결, 실제 표시와 OTA 복구는 하드웨어에서 추가 검증해야 합니다.

`src/oled_font.h`는 저장소에 고정된 Zephyr `drivers/display/mb_font.c`
(commit `8dafb9a897da0f79a1a3724a108b21fc28719915`)의 Intel 5×5 글꼴을
세로 열 형식으로 변환한 것입니다. Apache-2.0, 원문 라이선스는 `LICENSE.font`입니다.

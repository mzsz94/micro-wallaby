# NU87 Wi-Fi → NU40-KIT BLE 화면 제어와 별도 레거시 OTA 예제

작업 브랜치: `bugfix/zephyr-ble`.

현재 연결된 **NU40-KIT-DISPLAY**는 아래의 MZSZ-LCD/S140 예시와 다른 프로토콜과
부트로더를 사용합니다. 기존 NU40 앱을 유지하는
[NU40-KIT 연결 안내](../firmware/nu87_lcd_gateway/KIT-DISPLAY.md)를 먼저 확인하세요.
KIT 모드는 화면 문자열 전송을 지원하며, 현재 NU40 앱에 없는 전원 제어와 OTA는 비활성화합니다.

```text
Mac의 제어 화면 :8766
        │ HTTP / 테스트 Wi-Fi
        ▼
NU87-TinyDK (Zephyr 게이트웨이 :8080)
        │ BLE GATT 문자열 / 처리 결과 ACK
        ▼
NU40-DK (기존 NU40-KIT-DISPLAY 앱, OTA 미지원)
        │ I²C
        ▼
0.91인치 화면
```

두 보드는 BLE로 연결합니다. NU40에는 Wi-Fi를 요구하지 않습니다. 현재 KIT 경로의
실기기 검증은 문자열 전송과 처리 결과 ACK까지이며, KIT 앱에는 OTA가 없습니다.
아래의 `nu40_ble_lcd`와 Nordic/Adafruit DFU는 다른 레이아웃을 사용하는 별도 예제입니다.
이 예제의 OTA 대상은 NU40 앱뿐이며, NU87 자체 펌웨어·SoftDevice·부트로더는 대상이 아닙니다.
NU87 게이트웨이는 기존 NU87 IMU 앱을 대체하는 별도 앱입니다. IMU 측정과의 동시
실행은 아직 통합하지 않았습니다. 기존 IMU 소스와 교정값은 수정하지 않았습니다.

## 현재 검증 범위

- 별도 `nu40_ble_lcd` 예제의 기본/SSD1306/PCF8574/DFU 활성화 구성 컴파일과 UF2 주소 검사 통과.
- NU87 Wi-Fi STA + BLE Central 동시 구성의 컴파일 및 KM0/KM4 이미지 병합 통과.
- LCD 드라이버·명령 파서, DFU 상태 전이, HTTP 프레이밍, 호스트 패키지/제어 테스트.
- 로컬 제어 화면의 미연결 상태와 Wi-Fi 연결 / NU40 설정 대기 상태 확인.
- 새 NU87에 게이트웨이 이미지를 기록하고 Zephyr 앱 부팅, WPA2 테스트 AP 연결,
  DHCP 주소 할당, 맥북의 인증 HTTP 상태 요청과 미인증 요청 거부를 확인했습니다.
  [새 NU87 기록·부팅 결과](../firmware/nu87_lcd_gateway/HARDWARE-RESULT.md)를 참고하세요.
- 기존 `NU40-KIT-DISPLAY`의 `90E2C000-…` 프로토콜을 반영해 NU87에서 BLE 연결·GATT 검색·
  알림 구독을 완료했습니다. 맥북 웹 제어 경로로 텍스트를 보내 NU40의 성공 ACK를 확인했고,
  시리얼 모니터 종료 후에도 Wi-Fi·BLE 연결과 화면 제어 준비 상태를 확인했습니다.
- **NU40 펌웨어는 변경하지 않았습니다. 현재 KIT 앱은 OTA 미지원이며, OTA 전송과 전원 중단 복구는 미검증입니다.**

다음 화면 정보는 별도 `nu40_ble_lcd` 예제에 관한 초기 확인 기록입니다.
현재 KIT의 21열×4행 화면과 P0.22/P0.21 배선은 [KIT 문서](../firmware/nu87_lcd_gateway/KIT-DISPLAY.md)를 따릅니다.
예제 작업 당시 사용자는 SDA=P0.26, SCL=P0.27, VCC=3.3V, GND 배선을 확인했습니다.
상품은 [제공된 0.91인치 모듈](https://smartstore.naver.com/higenis/products/5083669090)입니다.
상품 페이지를 확인하지 못했으므로 **SSD1306 / 128×32 / I²C 0x3C는 아직 예제 가정**입니다.
이 예제는 16자 × 4줄 ASCII를 표시하며 한글 글꼴은 포함하지 않습니다.

## 로컬 화면

보드 없이 확인:

```sh
.venv/bin/python web/tools/lcd_bridge.py
# http://127.0.0.1:8766
```

KIT 모드에서는 입력 텍스트 전송과 처리 결과 ACK를 제공하며, 화면 전원과 OTA 기능은 비활성화됩니다.
별도 레거시 모드는 600ms 입력 후 자동 전송, 화면 켜기/끄기, 업데이트 패키지 검사,
OTA 진행 상태와 중단 요청을 제공합니다. 이 OTA 진입은 레거시 LCD 앱이 설치된 NU40을 대상으로 합니다.
패키지 선택은 검사를 수행하며, 업데이트
버튼을 누르기 전에는 보드를 재부팅하지 않습니다. 미연결 상태에서 전송 버튼은 꺼집니다.
전체 화면 갱신은 여러 BLE 명령으로 나뉘므로 픽셀 단위로 동시에 바뀌는 것은 아닙니다.

## 별도 레거시 LCD 앱의 첫 설치

현재 KIT 펌웨어를 유지하는 경우 이 절차를 적용하지 않고 [KIT 연결 안내](../firmware/nu87_lcd_gateway/KIT-DISPLAY.md)를 따릅니다.
아래는 `nu40_ble_lcd` 예제의 USB 기본 첫 설치 절차입니다. 이후 NU40 화면 제어와 앱 업데이트를
BLE로 수행하도록 구현했으나, 이 예제의 OTA 전송은 실기기에서 검증하지 않았습니다. 기존 IMU 앱에는 이 LCD 서비스와 OTA 진입 명령이 없습니다.

**USB가 하드웨어상 필수인 것은 아닙니다.** 이전에 확인한 Adafruit 0.9.1 / S140 6.1.1
부트로더라면 전원이 공급된 NU40의 SW0(P0.11)와 SW1(P0.12)을 함께 누르고 RESET을
한 번 눌렀다 떼면 BLE DFU 진입 조건이 됩니다. 이후 두 버튼을 놓고 `AdaDFU` 광고와
실제 DFU 서비스를 확인해야 합니다. RESET 두 번은 USB/serial 모드이므로 구분합니다.
현재 Mac 제어 화면에는 수동 부트로더 상태에서 첫 설치하는 전송 경로가 아직 없으며,
이 경로에는 별도 BLE DFU 호스트 연결이 필요합니다. Mac 시스템 설정의 페어링만으로
펌웨어가 설치되지 않습니다. 다른 부트로더의 보드에는 같은 방법을 가정하지 않습니다.
근거: [Adafruit 0.9.1 진입 조건](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.1/src/main.c),
[NU40 버튼 매핑](https://docs.zephyrproject.org/latest/boards/nucode/nucode_nu40/doc/index.html#push-buttons).

1. NU40의 실제 `INFO_UF2.TXT`가 **Adafruit 0.9.1 / S140 6.1.1 / NRF52BOOT**에 해당하는지
   확인하고, 원본 백업 및 USB 복구 경로를 확인합니다. 다른 부트로더에 적용하지 않습니다.
2. 화면 컨트롤러/해상도/주소를 확인하고
   [NU40 앱 지침](../firmware/nu40_ble_lcd/README.md)에 따라 해당 프로필을 빌드합니다.
   기본 빌드는 화면과 DFU 진입이 비활성화되어 있습니다. 아래 명령은 **보드 검증을 대신하지 않습니다**.

   ```sh
   scripts/west.sh build --pristine=always --board nucode_nu40/nrf52840 \
     --build-dir build/nu40-ble-lcd-dfu-verified firmware/nu40_ble_lcd -- \
     -DDTC_OVERLAY_FILE=app.overlay -DEXTRA_CONF_FILE=ssd1306-example.conf \
     -DCONFIG_LCD_LEGACY_DFU=y
   python3 firmware/nu40_imu_bringup/flash.py --layout legacy \
     --image build/nu40-ble-lcd-dfu-verified/zephyr/zephyr.uf2 --verify-only
   ```

   실제 초기 기록은 기존 플래셔의 `--write`로 수행합니다. 현재 연결된 보드를 식별하고
   부트로더 정보를 검사한 뒤에만 기록합니다.
3. NU40 앱의 BLE identity 주소와 부트로더의 BLE 주소를 각각 확인합니다. 공식 부트로더는
   일반적으로 마지막 주소 바이트를 1 증가시켜 광고하므로 두 주소가 다를 수 있습니다.
   이름 `MZSZ-LCD`만 보고 대상을 선택하지 않습니다.
4. NU87은 먼저 `bash firmware/nu87_imu_bringup/setup.sh`로 준비한 고정 SDK를 사용합니다.
   `bash firmware/nu87_lcd_gateway/setup.sh`로 고정된 crypto 빌드 의존성을 준비합니다.
   `local.example.conf`를 **Git에서 제외된** `local.conf`로 복사하여 테스트 Wi-Fi,
   두 BLE 주소 및 16~128자의 `CONFIG_GATEWAY_API_TOKEN`을 설정합니다.
   `bash firmware/nu87_lcd_gateway/build.sh`는 빌드만 수행합니다. 기록할 병합 이미지는
   `build/nu87-lcd-gateway/images/km0_km4_app.bin`입니다.
   기존 NU87 플래셔에 `--build-dir build/nu87-lcd-gateway`를 지정합니다. 이 도구는
   앱만 기록하는 옵션이 없으며, 빌드에 포함된 NU87 부트로더도 함께 기록합니다.
   기록 전 두 이미지의 해시·주소 범위와 실제 USB 장치 식별 정보를 확인합니다.
5. 게이트웨이가 테스트 AP에 연결하여 받은 IPv4 주소를 확인합니다. Mac도 같은 LAN에 연결합니다.
   로컬 셸에 `NU87_GATEWAY_TOKEN`을 설정한 뒤 실행합니다. 토큰과 Wi-Fi 비밀번호를 저장소에 넣지 않습니다.

   ```sh
   .venv/bin/python web/tools/lcd_bridge.py --gateway 192.168.4.20
   ```

   `192.168.4.20`은 예시이며 실제 NU87 주소로 바꿉니다. 기본 서버는 맥북의
   `127.0.0.1`에만 바인딩하므로 Wi-Fi 상태 확인과 화면 명령에 LAN 수신 포트는 필요하지 않습니다.
   OTA 파일 전송에는 별도로 `--serve-firmware-lan`을 지정해야 합니다. 이 경우에도
   제어 API/UI는 localhost 요청만 받으며, LAN에는 설정한 NU87 IP가 읽을 수 있는
   불변 펌웨어 경로만 제공합니다.

### 맥북과 같은 테스트 Wi-Fi 설정

웹 화면에는 Wi-Fi 비밀번호 입력란이 없습니다. Wi-Fi 이름을 확인하고 아래 설정 도구를 실행합니다.
비밀번호는 터미널의 화면에 표시되지 않는
입력으로 받으며 명령 인수에 넣지 않습니다. 이미 저장된 NU40 주소와 API 토큰은 유지됩니다.

```sh
.venv/bin/python firmware/nu87_lcd_gateway/configure.py \
  --ssid '테스트 Wi-Fi 이름' --security wpa2 --test-network
bash firmware/nu87_lcd_gateway/build.sh
```

설정 도구는 `local.conf`를 소유자만 읽고 쓸 수 있는 0600 권한으로 저장합니다.
이 포트의 난수 제한 때문에 WPA2는 `--test-network`로 확인한 테스트망에서만 설정합니다.
생성한 이미지와 빌드 캐시에도 Wi-Fi 설정이 포함되므로 외부에 배포하지 않습니다.
설정은 이미지에 포함되므로 변경 후 다시 빌드·기록해야 합니다.

기록 후 아래 도구로 해당 USB 일련번호의 부팅과 DHCP 주소를 확인할 수 있습니다.
시리얼 포트를 여는 과정에서 USB 어댑터의 제어 신호로 보드가 재시작될 수 있으므로,
이미 네트워크가 연결됐다면 HTTP 상태 확인을 우선 사용합니다. 필요하면 도구를 실행한
상태에서 RESET을 한 번 누릅니다. 이 보드는 vendor 초기화 중
`Efuse empty!` 경고에서 입력을 기다립니다. 도구는 전체 경고를 실제로 관찰한 경우에만
개행 한 번을 보내며 eFuse를 기록하지 않습니다. 전원을 다시 켰을 때도 이 단계가 필요할
수 있습니다. 독립 전원만으로 시작하는 동작은 아직 확인하지 않았습니다.

```sh
.venv/bin/python firmware/nu87_lcd_gateway/connect.py \
  --serial-number '<NU87-USB-SERIAL>' \
  --acknowledge-efuse-warning
```

일련번호는 실제 연결 대상에 맞게 지정합니다. 연결 주소는 Git에서 제외한
`build/nu87-lcd-gateway/gateway-connection.json`에 저장합니다. 시리얼의 주소 출력과
별도로 Mac에서 인증 HTTP 응답을 확인해야 실제 네트워크 통신을 검증할 수 있습니다.

NU87이 DHCP로 받은 주소를 확인한 뒤, 토큰을 셸 인수나 화면에 출력하지 않고
비공개 설정에서 직접 읽어 제어 서버를 실행할 수 있습니다.

```sh
.venv/bin/python web/tools/lcd_bridge.py --gateway 192.168.0.123 \
  --gateway-config firmware/nu87_lcd_gateway/local.conf
```

위 주소는 실제 NU87 주소로 바꿉니다. `http://127.0.0.1:8766`의 연결 표시에서
맥북 로컬 서버, NU87 Wi-Fi 응답, NU40 BLE 상태를 각각 확인합니다.
NU40이 아직 연결되지 않아도 NU87 Wi-Fi 응답은 별도로 확인할 수 있습니다.

### 고정 SDK의 Wi-Fi 호환 보완

앱은 연결 시도 전에 STA 인터페이스를 활성화하며, 무선 초기화가 끝난 뒤 실제
무선 MAC 주소를 읽어 Zephyr의 링크 주소와 맞춥니다. eFuse를 변경하거나 임의의
MAC 주소를 생성하지 않습니다. 기존 드라이버는 비동기 초기화 전에 eFuse 주소를
읽으므로 이 보드에서 잘못된 링크 주소로 인터페이스 활성화가 실패했습니다.

또한 SDK의 소프트웨어 타이머는 인증 콜백을 인터럽트에서 직접 호출해 mutex
사용 시 커널 패닉을 일으켰습니다. 앱의 `ameba_timer_thread.c`가 링커 래퍼로
Realtek 타이머 API를 전용 스레드에 연결합니다. SDK 원본 파일은 수정하지 않습니다.
실행 중 삭제는 콜백 반환 후 메모리를 해제하며, 콜백에서의 재시작·주기 변경,
0ms 단발 예약과 늦어진 주기의 중복 실행 방지를 호스트 테스트로 확인합니다.
호스트 테스트는 실제 Wi-Fi 인증과 Zephyr 스케줄링 검증을 대신하지 않습니다.
Zephyr 본 저장소의 타이머 래퍼 수정은 [PR #121773](https://github.com/zephyrproject-rtos/zephyr/pull/121773)로
제출했습니다. 이 앱의 HAL 공존 보고·IRQ 호환 계층은 해당 PR에 포함되지 않습니다.

HTTP 파서와 vendor의 동기 Wi-Fi 송신 경로가 main 스택을 함께 사용하므로
기존 4KiB에서 실제 상태 요청 중 스택 초과가 발생했습니다. main 스택을 12KiB로
확보하고, 첫 HTTP 요청 처리 후 남은 스택을 콘솔에 기록해 실제 여유량을 확인합니다.

연결/DHCP 대기 예산은 60초입니다. 이 SDK는 연결 해제 요청 후 Zephyr 완료 이벤트를
전달하지 않는 경로가 있어, 공개 API의 실제 무선 상태를 250ms 간격으로 확인합니다.
명시적인 해제 완료 상태를 연속 두 번 확인하고 이전 DHCP 임대를 정리한 뒤 재시도합니다.
10초 안에 해제를 확인하지 못하면 중복 연결을 시도하지 않고 재시작이 필요하다고 기록합니다.
현재 연결 시도가 반환된 뒤 무선 연결 상태와 실제 DHCP 임대가 일치할 때만 준비 완료를 표시합니다.

## 별도 레거시 앱의 OTA 패키지와 진행

```sh
.venv/bin/python web/tools/lcd_package.py \
  --build build/nu40-ble-lcd-dfu-verified --version 1 \
  --out build/nu40-lcd-v1.zip
```

ZIP은 `manifest.json`과 `application.bin` 두 파일만 포함합니다. 0x26000 시작 주소,
nRF52840 벡터, 최대 0xB6000 바이트, 4바이트 정렬, SHA-256, CRC16과 S140 FWID 0xB6를
검사합니다. NU87 raw bin이나 UF2를 그대로 업로드할 수 없습니다. 패키지는 서명된
배포물이 아니며 manifest의 버전은 메타데이터입니다.

Mac은 검사한 앱을 불변 메모리에 보관합니다. NU87은 요청한 Mac의 IP에서 4KB 범위
단위로 읽고, 전체 CRC와 벡터 검사를 마친 뒤에만 NU40에 DFU 진입을 요청합니다.
명시한 부트로더 주소, 서비스, revision 0x0008, 알림 구독을 확인한 뒤 전송합니다.
20바이트 패킷, PRN 흐름 제어, CRC 검증, 활성화, 재부팅 후 앱 응답 확인을 수행합니다.
대상 보드의 실제 S140/부트로더 버전은 **초기 설치 때 확인해야 합니다**. revision 0x0008은
프로토콜 버전만 확인하며, 원격 기종·펌웨어 버전을 증명하지 않습니다.

표시된 전송률은 부트로더가 수신 확인한 바이트 기준입니다. 100%는 앱 실행 성공과
같지 않습니다. 마지막 `app_responding`은 LCD 앱의 STATUS 응답을 확인한 상태이며,
설치 이미지 버전의 원격 증명이나 실제 OLED 표시 확인을 의미하지 않습니다.
중단 요청은 활성화 명령을 보내기 전까지만 허용됩니다. 실패한 OTA를 자동 재시도하거나
다른 장치에 이어서 보내지 않습니다.

## 현재 제한과 복구

이 버전은 **분리된 개발용 테스트 LAN**에 한정됩니다. 현재 AmebaD 포트의 난수 원천은
보안 품질이 확인되지 않았고, BLE SMP와 TLS를 사용하지 않습니다. HTTP bearer 토큰과
명시적 BLE 주소는 전송 암호화나 장치 암호 인증을 제공하지 않습니다. WPA2 테스트도
`CONFIG_GATEWAY_ALLOW_INSECURE_TEST_PSK=y`를 명시해야 하며 일상용 네트워크 자격증명을
이 설정에 넣지 않습니다.

Adafruit 레거시 부트로더는 앱을 한 영역에 직접 기록합니다. 시작 후 전원/무선 연결이
끊기면 USB 복구가 필요할 수 있고 자동 롤백은 없습니다. CRC는 전송 오류 검출이며
서명 검증이나 악성 펌웨어 방지가 아닙니다. 앱 크기 제한은 검사한 0.9.1 소스 기준으로
0xDC000 이상을 지우지 않도록 설정했지만, 실제 설치된 부트로더에서 교정값 보존은
하드웨어로 다시 확인해야 합니다. 자세한 근거와 오류 상태는
[DFU 구현 문서](../firmware/wireless_lcd_common/dfu/README.md)에 있습니다.

실사용 배포 전에는 검증된 난수/전송 보안, 인증된 이미지와 부트로더, 전원 중단 복구를
검증해야 합니다. 현재 빌드 성공을 실제 OTA 성공으로 보고하지 않습니다.

## 검증 명령

```sh
make -C firmware/nu40_ble_lcd/tests test
bash firmware/wireless_lcd_common/dfu/tests/run_tests.sh
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Ifirmware/nu87_lcd_gateway/include firmware/nu87_lcd_gateway/src/http_parse.c \
  firmware/nu87_lcd_gateway/tests/test_http_parse.c -o /tmp/lcd-http-test
/tmp/lcd-http-test
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Ifirmware/nu87_lcd_gateway/include firmware/nu87_lcd_gateway/src/timer_core.c \
  firmware/nu87_lcd_gateway/tests/test_timer_core.c -o /tmp/lcd-timer-test
/tmp/lcd-timer-test
.venv/bin/python -m unittest discover -s firmware/nu87_lcd_gateway/tests -p 'test_*.py'
.venv/bin/python -m unittest discover -s web/tests -p 'test_lcd_bridge.py'
```

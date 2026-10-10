# NU87 → NU40DK-KIT BLE 중계 앱

NU87의 AmebaD Arduino 3.1.9 / FreeRTOS 앱입니다. 기존 IMU 대시보드 펌웨어는
그대로 두고 별도 폴더에 추가했습니다. 이 앱은 IMU를 읽지 않으며 NU40 KIT에
문자열·움직이는 얼굴을 보내고, PC에서 받은 MCUmgr SMP 요청을 BLE로 중계합니다.
NU40은 Zephyr + MCUboot를 사용합니다.

```text
기본 시연: NU87 얼굴 상태 → BLE → NU40 OLED → ACK → NU87
펌웨어: PC의 서명 이미지 → USB UART → NU87 → BLE SMP → NU40
KIT 제어: PC → NU87 → NU40의 버튼/RGB/마이크/스피커/SD 상태·동작
```

수신 앱·서명 키·OTA 패키지·KIT 제어 도구는
[nu40-ble-display의 feat/nu40-kit-ota](https://github.com/mzsz94/nu40-ble-display/tree/feat/nu40-kit-ota)에
있습니다. NU40을 PC에 직접 연결하지 않고 보조배터리 전원만으로 업데이트할 수 있습니다.
현재 인터넷에서 이미지를 내려받는 기능은 없습니다.

## 빌드

이 폴더를 작업 디렉터리로 사용합니다. Arduino CLI, 설치된 AmebaD core **3.1.9**와
ameba_d_tools **1.1.3**, Python 3, x86_64 Linux/WSL이 필요합니다.
FQBN은 `realtek:AmebaD:Ameba_AMB26`입니다. SDK와 바이너리는 저장소에 넣지 않습니다.

```sh
cd firmware/nu87_nu40_bridge
python3 -m pip install -r requirements.txt
python3 tools/nu87_native_build.py --build-dir build/nu87-ota-relay \
  --tools-dir build/arduino-tools --cache-dir build/arduino-cache
```

빌드 도구는 벤더 postbuild가 설치 폴더에 쓰지 않도록 tools를 `build/`에 복사합니다.
기존 SDK 링커 플래그를 보존하고 `otp_guard.cpp`의 두 EFUSE 쓰기 차단 래퍼를 추가합니다.
새 이미지 3개를 검증한 뒤 소스·SDK·ELF·이미지 해시를 manifest에 기록하며 플래시는 실행하지 않습니다.
`shared/`는 NU40 저장소와 같은 프레임 정의이며 빌드 도구가 include 경로를 지정합니다.

## 기록과 캡처

```sh
python3 tools/nu87_flash.py --image-format arduino \
  --build-dir build/nu87-ota-relay --port /dev/ttyUSB0 \
  --serial-number ACTUAL_USB_SERIAL --write
python3 tools/nu87_capture.py --serial-number ACTUAL_USB_SERIAL --seconds 30
```

업로더 안내가 나오면 BOOT를 누른 채 RESET을 눌렀다 놓고, BOOT를 놓은 뒤 Enter를 누릅니다.
자동 RESET은 꺼져 있습니다. 기록 후 BOOT 없이 RESET으로 앱을 시작합니다.
CP2102N bridge의 VID:PID는 `10c4:ea60`이며 실제 USB 고유번호를 명시합니다.
console은 1,500,000 baud이고 host는 DTR/RTS false로 엽니다.
호스트에서 UART를 열 때 새 BOOT가 관측된 적이 있어 DTR/RTS 설정만으로 무리셋을 보장하지 않습니다.

부트 로그는 `app=ble_display_native firmware=3 ... features=smp_relay`입니다.
세 이미지의 기록 위치는 `0`, `0x4000`, `0x6000`이며 Zephyr의 두 이미지 배치와 다릅니다.
플래셔는 SDK 부트/KM0 해시, 헤더, 패딩, `0xfc000` 이전 경계를 검사합니다.
네이티브 SDK의 BLE 초기화는 일반 flash FTL 영역을 쓸 수 있습니다. OTP 쓰기는 차단합니다.

## 통신과 명령

NU40이 광고하는 서비스 `90e2c000-3c0d-4ef3-9d78-574cd1a64087`로 스캔합니다.
광고 필드 길이·UUID를 먼저 검사하고 SDK에 UUID 하나만 전달해 광고 파서의 범위를 제한합니다.
ACK 특성 `90e2c001…`을 구독하고 `90e2c002…`에 최대 20바이트 Write Request를 보냅니다.
동일 sequence의 status 0 ACK가 와야 `SEND result=0`입니다. 실제 OLED 표시는 별도로 관찰합니다.

USB 콘솔의 LF 종료 명령은 다음과 같습니다.

| 명령 | 동작 |
| --- | --- |
| `face` | 기본 움직이는 얼굴 전송 |
| `text` | Count/Uptime 문자열 전송 |
| `pause` | 화면 데모 전송 일시 정지 |
| `relay-status` | firmware 3, SMP 준비·정지 상태 조회 |
| `smp <hex>` | 최대 512바이트 SMP 요청 중계 |

SMP 서비스 `8d53dc1d-1db7-4cd3-868b-8a527460aa84`, 특성
`da2e7828-fbce-4e01-ae9e-261174997c48`을 사용합니다.
요청을 20바이트 Write Without Response로 나누고 응답을 외부 512바이트 버퍼에서 재조립합니다.
헤더 길이, group, sequence, command와 작업 상태를 검사합니다.
현재 ATT MTU 23에서 알림 단편은 최대 20바이트입니다. SDK의 512바이트 수신 버퍼 요청은
상한 230을 넘어 거절되고 기본 20바이트가 유지되지만 이 MTU의 수신은 실기 검증됐습니다.

SDK UART ring은 128바이트(실사용 127바이트)이므로 호스트는 32바이트씩 20ms 간격으로
보냅니다. 큰 SMP 요청 전 `pause` 처리 응답을 기다립니다. 연결·원격 객체·ACK·SMP 수신은
각각 mutex와 semaphore로 보호하며 SDK가 삭제한 원격 객체를 사용하지 않도록 합니다.

## 검증

```sh
PYTHONPATH=tools python3 -m unittest discover -s tools -p 'test_*.py'
python3 -m unittest discover -s nu87_ble_display_arduino/tests -p 'test_*.py'
c++ -std=c++11 -Wall -Wextra -Werror -Ishared \
  nu87_ble_display_arduino/tests/ack_gate.cpp -o build/ack-test
build/ack-test
```

2026-10-10 실기에서 firmware 3 부팅·BLE 연결·얼굴/ACK 211건을 확인했습니다.
이 중계 이미지로 NU40 `0.1.0 → 0.1.1`의 서명 앱 167,152바이트를 전송했고,
새 primary의 해시·active·confirmed 상태와 사용자의 OLED 얼굴 재개 관찰을 확인했습니다.
재부팅 뒤 같은 세션의 SMP 조회가 시간 초과된 적이 있으며, 새 UART 세션 조회는 성공했습니다.
NU40 저장소의 호스트 도구에는 확인 전용 새 세션 재시도가 포함됩니다.

같은 중계 펌웨어로 KIT 통합 앱 `0.1.1 → 0.1.2` 208,802바이트도 무선 전송했습니다.
첫 세션의 확인 시간 초과 후 도구가 UART 세션을 한 번 다시 열어 대상 해시와
`active=true`, `confirmed=true`를 확인했습니다. 추가 업로드·NU40 RESET은 없었습니다.
사용자가 OLED·RGB·비프·녹음 재생을 모두 확인했고, 중계 경유 화면·RGB 변경,
비프·녹음·재생·SD 요청의 완료 및 WAV 저장·읽기 대조를 확인했습니다.
[NU40 실기 결과](https://github.com/mzsz94/nu40-ble-display/blob/feat/nu40-kit-ota/docs/kit-hardware-results.md)에
세부 관측과 제한을 기록했습니다. KIT 업데이트 때문에 NU87을 재기록하지는 않았습니다.

이 폴더의 호스트 검사 102개, 광고 필터 검사 1개와 C++ ACK gate 검사가 통과했습니다.
장기 안정성·전원 차단·롤백 실기는 별도 검증 대상입니다.

설치 이미지2는 843,776바이트이며 SHA-256은
`ed42e5971d12b82655a7b4b5bb8322b03f85872a04c24d05f5ba85935289018f`입니다.
기존 Zephyr NU87 HCI 초기화 실패와 이번 성공 사이에는 보드 교체와 SDK 변경이 함께 있어,
둘 중 하나만을 원인으로 확정하지 않습니다.

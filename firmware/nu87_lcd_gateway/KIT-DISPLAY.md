# NU40-KIT-DISPLAY 연결

이 문서는 기존 표시 전용 KIT 펌웨어의 기준 동작입니다. 현재 브랜치의 선택적
마이크·BTN4/스피커·NU87 모션 상태·인증된 SMP OTA 확장은
[Voice Tactical 연결](VOICE-TACTICAL.md)을 확인하세요. 기존 KIT에 없는 기능은
연결 준비로 간주하지 않으며, 아래 레거시 OTA 제한은 그대로 유지합니다.

현재 NU40의 [mzsz94/nu40-ble-display](https://github.com/mzsz94/nu40-ble-display)
펌웨어에 맞춘 NU87 중앙 장치 모드입니다. 기준 커밋은
`230a1300d121b3bf25d3125a0cf247679148a40a`입니다. NU40의 기존 펌웨어를 유지합니다.

## 사용

1. Git에서 제외된 `local.conf`에 Wi-Fi 설정과 `CONFIG_GATEWAY_NU40_KIT=y`를 지정합니다.
   `kit-display.conf`는 이 모드를 선택하는 공개 예시입니다.
2. `bash firmware/nu87_lcd_gateway/build.sh`로 빌드한 뒤 기존 NU87 절차로 기록합니다.
3. NU87의 DHCP 주소를 확인하고 맥북에서 제어 서버를 실행합니다.

   ```sh
   .venv/bin/python web/tools/lcd_bridge.py --gateway '<NU87-IP>' \
     --gateway-config firmware/nu87_lcd_gateway/local.conf
   ```

4. `http://127.0.0.1:8766/`에서 장치를 검색하고 표시된 **주소와 종류**를 선택해 연결합니다.
   서비스 UUID와 이름이 일치하는 최근 광고만 후보로 표시하며, 다른 장치로 자동 대체하지 않습니다.
   macOS가 표시하는 CoreBluetooth UUID는 Bluetooth MAC 주소가 아니므로 이 입력에 쓰지 않습니다.
5. 영문·숫자·기호와 줄바꿈으로 1~84바이트를 입력합니다. 화면은 21열×4행이며 긴 행은
   자동으로 줄바꿈됩니다. 전송 성공은 같은 시퀀스의 OLED 처리 완료 ACK를 받은 경우입니다.
   실제 글자가 보이는지는 화면에서 별도로 확인합니다.

NU40은 한 대의 중앙 장치만 연결할 수 있습니다. 다른 BLE 앱이 연결 중이면 먼저 해제합니다.
현재 선택은 NU87 실행 중에 유지되며 재부팅 후 다시 선택합니다. 잠시 연결이 끊기면
선택했던 주소에만 재연결하고 미완료 화면 명령을 다시 보내지 않습니다.
Bluetooth 컨트롤러 초기화, 주변 검색과 새 연결은 Wi-Fi 주소를 받은 뒤 시작합니다.
초기화는 한 번만 시도하며 Wi-Fi 주소 대기에 제한 시간을 두지 않습니다. Wi-Fi 재접속 중에는
검색을 멈추고 이전 광고 후보를 지우며, 이미 연결된 BLE 링크는 유지합니다.

## 지원 기능과 차이

- 문자열 전체 전송과 처리 결과 ACK를 지원합니다. CCCD 구독은 전송 준비 상태이며,
  OLED 상태를 증명하지 않습니다. 해당 메시지의 ACK 0이 드라이버 처리 성공을 뜻합니다.
- 현재 NU40 앱에는 화면 전원 제어와 BLE OTA 진입 명령이 없습니다. 이 모드에서는
  관련 버튼과 API를 막고 레거시 DFU 전송 코드를 빌드에 포함하지 않습니다.
- NU40 저장소의 보드 구성은 BARAM-NU40 / SoftDevice 없음 / 앱 시작 `0x1000`입니다.
  이전 S140 / 앱 시작 `0x26000`용 OTA 이미지를 이 보드에 적용하면 안 됩니다.
- 무선 펌웨어 업데이트에는 NU40 쪽의 호환 부트로더와 업데이트 기능 설치가 먼저 필요합니다.
  현재 펌웨어가 제공하는 소프트웨어 부트로더 진입은 USB 콘솔 `uf2` 또는 1200-baud 동작뿐입니다.
- 현재 NU40-KIT의 OLED 배선은 SDA=P0.22, SCL=P0.21, I²C 400kHz입니다.
  이전 별도 모듈 예시의 P0.26/P0.27 배선과 다릅니다. 이번 변경은 NU87 송신 측에만 적용합니다.

## HTTP와 BLE

모든 NU87 HTTP 경로는 기존 bearer 인증이 필요합니다.

| HTTP | 내용 |
| --- | --- |
| `GET /v1/status` | `protocol=nu40-kit-display`, 21×4, 전원/OTA 미지원과 연결 상태 |
| `GET /v1/peers` | 최근 서비스·이름이 일치하는 광고 후보 배열 |
| `GET /v1/scan` | UART·HCI·광고 필터·공존 보고·RF 보정의 숫자 진단, 주변 장치 정보 없음 |
| `POST /v1/scan` | 원문 `active` 또는 `passive`로 검색 방식 변경 요청, 연결 중이면 409 |
| `POST /v1/peer` | ASCII `AA:BB:CC:DD:EE:FF random` 또는 `… public`으로 명시적 선택 |
| `POST /v1/display` | 최대 84바이트 원문 문자열, matching ACK 0일 때만 HTTP 200 |

BLE 서비스는 `90e2c000-3c0d-4ef3-9d78-574cd1a64087`이며 ACK는 `…c001`,
TEXT는 `…c002`입니다. ACK 구독을 완료한 뒤 14바이트 조각을 순서대로
write-with-response로 보냅니다. 메시지 조립 제한 2초와 최종 ACK 제한을 별도로 적용합니다.
연결 해제·시간 초과 후 오래된 응답을 새 요청의 성공으로 처리하지 않습니다.

## 드라이버 호환 및 검사

고정된 Realtek 드라이버는 HCI UART1의 IRQ 35를 하드웨어 우선순위 7로 등록합니다.
Zephyr는 예약된 우선순위 한 단계를 더하므로 그대로 전달하면 범위 검사에서 중단됩니다.
`ameba_irq.c`는 IRQ 35 / 우선순위 7 / 플래그 0 조합만 Zephyr의
`IRQ_PRIO_LOWEST`로 변환합니다. SDK 파일과 다른 인터럽트 등록은 변경하지 않습니다.

`radio_diagnostics.c`는 Wi-Fi 접속과 Bluetooth 초기화 전후 실제 시스템 힙 사용량을
기록합니다. Wi-Fi 드라이버가 모든 실패를 같은 오류로 바꾸기 전에 원래 결과와
연결 상태 번호도 기록하며, 네트워크 비밀번호나 API 토큰은 출력하지 않습니다.
진단용 `/v1/scan`은 UART 수신, HCI 전달, 앱 콜백·필터 통계를 구분합니다.
RF 보정 호출의 원래 인수와 결과는 유지하고 숫자 결과만 집계합니다.
HCI 진단에는 실제 이벤트 마스크·검색 명령과 컨트롤러의 완료 상태가 포함됩니다.
`commands`의 각 값은 `[전송 횟수, 마지막 상태]`이며 상태 -1은 미확인/응답 대기,
0은 컨트롤러가 명령을 수락했음을 뜻합니다. 검색 방식 변경은 비동기이며,
`requestedMode`와 `activeMode`, `generation`으로 실제 적용을 확인합니다.
수동 검색(`passive`)은 검색 응답을 요청하지 않으므로 이름이 검색 응답에만 담긴
NU40이 후보로 완성되지 않을 수 있습니다. 원인 분리용이며 정상 연결에는
기본값인 `active`를 사용합니다. 검색 시작 때 공존 설정을 읽기만 하며 안테나나
eFuse 설정을 변경하지 않습니다.

### 연결 감시 시간

NU87은 `BT_LE_CONN_PARAM_DEFAULT`의 초기 연결 감시 시간 4초를 유지하도록, 선택한
NU40의 더 짧은 매개변수 요청을 4초로 조정합니다. 현재 NU40 Zephyr 빌드의 주변장치
기본 선호값 `42`는 10ms 단위이므로 420ms입니다. 연결 간격·지연은 그대로 협상하고,
더 긴 감시 시간은 줄이지 않습니다. 다른 장치의 요청은 바꾸지 않습니다.

`/v1/scan`의 `link.paramRequests`, `shortTimeoutRequests`, `requestedTimeout`은
수신 요청과 조정 횟수입니다. `paramUpdates`, `paramsKnown`, `params`는 실제 완료
콜백의 `[연결 간격(1.25ms), 지연, 감시 시간(10ms)]`입니다. 요청 수락만으로 적용을
주장하지 않습니다. 연결 종료/새 연결에는 `paramsKnown=false`로 바뀌며 마지막 값은
남깁니다. `minTimeout=400`은 정책값입니다. 이것은 연결 감시이며 HostPC 센서의 1.5초
신선도나 OLED 데이터의 3초 만료를 연장하지 않습니다.

### 수신 경로별 카운터

기존 `radio_diagnostics.c`의 `bt_coex_process_rx_frame` 훅은 **Realtek의 H4 이벤트
풀에서 패킷을 할당하고 RX 큐로 전달한 뒤** 실행됩니다. Zephyr 호스트 버퍼 할당보다
앞이지만, Realtek 이벤트 풀보다 앞인 것은 아닙니다. 따라서 이 훅의 광고 수가 낮다는
사실만으로 무선 수신 자체가 실패했다고 결론 내릴 수 없습니다.

`hci_transport_diagnostics.c`는 기존 `hci_uart_read`의 반환 바이트를 관찰하여
Realtek 풀 할당 전의 H4 프레임 수를 별도로 셉니다. 원래 바이트와 반환값은 유지하고,
프레임 헤더·길이·LE 하위 이벤트 종류만 확인합니다. 장치 주소나 광고 내용은 저장하지 않습니다.

| 단계 | 주요 값 | 해석 |
| --- | --- | --- |
| UART, Realtek 풀 할당 전 | `uartBytes`, `eventHeaders`, `leHeaders`, `advHeaders`, `advFrames` | 실제 읽힌 바이트, 광고 헤더와 끝까지 읽힌 광고 프레임 수 |
| Realtek 풀·RX 큐 통과 후 | `hci.rx_events`, `hci.le_events`, `hci.adv_events` | 기존 HCI 훅까지 전달된 이벤트 수 |
| Zephyr 앱 콜백 이후 | 앱 광고 수와 서비스·이름 필터 통계 | 앱이 받은 광고와 NU40 후보 조건 충족 여부 |

`advHeaders`와 `advFrames`는 legacy/extended 광고 이벤트 모두를 세며, 기존
`hci.adv_events`는 legacy 광고 이벤트를 셉니다. 현재 빌드는 legacy 검색을 사용합니다.
`inFrame`과 `pendingBytes`는 여러 UART 읽기에 걸친 미완료 프레임을 나타내며,
한 번의 스냅샷에서 헤더 수와 완료 수가 다르다고 유실로 판단하지 않습니다.
`uartOverruns`, `uartParity`, `uartFraming`, `uartBreak`는 드라이버가 이미 요청한
UART 상태 읽기의 결과를 관찰합니다. 추가 레지스터 읽기는 하지 않습니다.
오류가 있으면 프레임 경계와 카운터 비교가 불확실해지므로 먼저 오류를 확인합니다.
`unknownBytes`, `invalidLengths`, `readErrors`도 함께 확인하며, 카운터 차이는
유실 구간을 좁히는 근거이지 특정 풀의 고갈을 단독으로 증명하지는 않습니다.

### BLE 검색 상태를 Wi-Fi 공존 드라이버에 전달

고정된 `hal_realtek` 커밋 `9a4caf9846d6f0ceb6b38f971a771b1325c1e4b9`의
`ameba/common/bluetooth/rtk_coex/rtk_coex.c`는 TX에서 ACL만 처리하며,
BLE 검색 명령의 상태를 Wi-Fi 공존 드라이버에 보고하는 경로가 없습니다.
공식 Realtek SDK의 [rtk_coex.c](https://github.com/Ameba-AIoT/ameba-rtos/blob/49e4ae8e7e760a843e75e48ade5e00bfff7e7419/component/bluetooth/rtk_coex/rtk_coex.c)와
[rtk_coex.h](https://github.com/Ameba-AIoT/ameba-rtos/blob/49e4ae8e7e760a843e75e48ade5e00bfff7e7419/component/bluetooth/rtk_coex/rtk_coex.h)
(커밋 `49e4ae8e7e760a843e75e48ade5e00bfff7e7419`)는 AmebaD에서 호스트가 검색 상태를
자동 보고하는 구현과 mailbox 형식을 제공합니다. 현재 링크된 `lib_coex.a`의 수신
처리에서도 같은 8바이트 형식과 필드 위치를 확인했습니다.

저장소의 `ameba_coex_scan.c`는 이 형식에 맞춰 일반 검색의 시작·종료, interval과
window를 전달합니다. HCI 명령 전송만으로 상태를 확정하지 않고, 해당 명령의 성공한
Command Complete를 받은 뒤 반영합니다. 실패·불완전 패킷·대응이 불분명한 중복 명령은
적용하지 않습니다. 보고는 별도 스레드에서 수행하며 원래 HCI 패킷과 ACL 처리는 유지합니다.

기존 H4 RX 처리와 보고 스레드는 같은 mutex로 공존 처리와 상태 갱신을 직렬화합니다.
BLE 링크·프로파일 상태를 보고 도중 오래된 값으로 덮어쓰는 경쟁을 막기 위한 처리입니다.
TX/인터럽트 경로에서는 이 mutex를 기다리거나 vendor 보고 함수를 호출하지 않으며,
Zephyr 호스트의 HCI 수신 호출은 mutex 밖에 있습니다.
보고 스레드는 H4 RX보다 낮은 우선순위로 실행합니다.

LE Create Connection은 성공한 Command Status를 받은 뒤 initiating 상태를 보고합니다.
일반 검색과 다른 scan-type 비트(0x02), window/interval mailbox(0x49),
연결 설정 단계 mailbox(0x46)를 제조사 형식으로 전달합니다. 취소는 성공한
Command Complete 이후 반영하며, 이전 시도의 늦은 취소 응답이 새로운 연결
시도를 지우지 않도록 구분합니다. 연결 성공·실패 이벤트가 initiating 상태를 종료하고,
성공 후에는 제조사 계산식에 따른 최대 1초의 설정 유예만 유지합니다.
네이티브 extra-info의 다른 필드는 보존합니다. `create`, `cancel`, `connection`은
각각 `[관측 횟수, 마지막 HCI 상태]`이며 -1은 미확인, 0은 성공입니다.
실기기 비교를 위해 기존 3초 연결 생성 제한은 변경하지 않았습니다.

네이티브 BT-info의 다른 플래그를 보존하고 검색 비트만 변경합니다. 한 번이라도 BLE 링크가
성립하면 성공한 HCI Reset까지 합성 BT-info 보고를 중단합니다. 연결 해제 후 남은 프로파일
플래그를 다시 적용하지 않기 위한 제한이며, 이때도 검색 종류·interval·window 보고는 유지합니다.
전체 연결·프로파일 공존 동작을 공식 SDK 전체와 동일하게 이식한 것은 아닙니다.
공존 진단의 `commits`는 성공 응답을 반영한 수, `batches`와 `reports`는 vendor 함수 호출 수입니다.
이 수치만으로 무선 수신 개선이나 펌웨어 내부 적용 완료를 증명하지는 않습니다.

이 보완의 프레임 형식, 실패·Reset 처리, 짧은 패킷, 네이티브 연결과 보고가 겹치는 경쟁은
호스트 테스트로 검사합니다. 실기기에서는 광고 수신 증가, NU40 검색·연결·GATT 준비,
웹 경유 텍스트 전송과 성공 ACK를 확인했습니다. 수치와 남은 제한은
[실기기 기록](HARDWARE-RESULT.md)에 정리했습니다.

공존 보고 누락과 HCI UART IRQ 우선순위 처리는 소유 소스가
`zephyrproject-rtos/hal_realtek`에 있어 해당 저장소가 우선적인 upstream 검토 대상입니다.
별도의 timer 래퍼 문제는 Zephyr 본 저장소의
`modules/hal_realtek/ameba/os_wrapper/os_wrapper_timer.c`에 속합니다.
타이머 래퍼 수정은 [Zephyr PR #121773](https://github.com/zephyrproject-rtos/zephyr/pull/121773)로
제출했습니다. PR은 타이머 문제만 다루며 이 앱의 공존 보고·IRQ 보완은 포함하지 않습니다.
HAL 공존 보고 보완은 아직 `hal_realtek`에 제출하지 않았습니다.

The [English hardware-evidence report in micro-wallaby #8](https://github.com/mzsz94/micro-wallaby/issues/8#issuecomment-6096231052)
separates measured coexistence results, operator confirmation, and remaining limitations.
The exact upstream timer port was tested with QEMU but has not been flashed onto this board;
the working gateway uses the application-local timer compatibility layer.

```sh
bash firmware/nu87_lcd_gateway/tests/run_kit_tests.sh
sh firmware/nu87_lcd_gateway/tests/run_hci_transport_tests.sh
.venv/bin/python -m unittest discover -s firmware/nu87_lcd_gateway/tests -p 'test_*.py'
```

프로토콜 및 BLE 콜백 순서, HTTP 인증·요청 제한, 인터럽트 변환 범위,
UART 프레임 관찰과 공존 보고의 상태·순서·메모리 접근을 검사합니다.
실제 BLE 송수신 확인은 아래 실기기 기록과 구분합니다.

실기기 검증 결과와 남은 제한은 [HARDWARE-RESULT.md](HARDWARE-RESULT.md)를 확인하세요.

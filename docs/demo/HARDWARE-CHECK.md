# Linux / WSL 실기 연결 확인 — 2026-10-11

`test/military-wallaby`에서 분기한 `feat/military-wallaby-demo-ble`의 확인 결과입니다.
상황별 GIF는 별도의 시뮬레이션 녹화이며, 아래 실기 확인과 구분합니다.

## 구성과 확인 결과

```text
HostPC / WSL · voice_tactical.server
  └─ 같은 로컬 Wi-Fi · 인증된 HTTP → NU87 TinyDK
       └─ BLE GATT → 발견 목록에서 선택한 NU40-KIT-DISPLAY
            └─ OLED 드라이버 처리 ACK → NU87 → HostPC
```

| 항목 | 관측 결과 | 확인 범위 |
| --- | --- | --- |
| NU87 기록 | 벤더 자동 업로드 성공, 새 Zephyr 앱 부팅 로그 | 별도 플래시 readback은 하지 않음 |
| Wi-Fi | DHCP 완료, 인증된 상태 요청 3회 성공 | 시작 중 연결 실패와 재시작도 관측; 원인 미확정 |
| HTTP 인증 | 인증 없는 상태 요청은 401 | 격리된 개발 네트워크에서 확인 |
| BLE | 서비스와 이름이 일치하는 후보 1개를 선택, 연결·OLED 준비 확인 | 암호학적 피어 신원이나 과거 보드 주소와의 동일성은 확인하지 않음 |
| OLED | HostPC 표시 요청 HTTP 200, matching ACK, 약 3.3초 | 실제 픽셀은 사람이 관찰하지 않음 |
| NU40 오디오 | 제어 시작·정지 수락, ADPCM 4프레임 / 0.08초 | 수신 PCM은 무음. 연속 발화 수집과 음성 품질은 미확인 |
| OTA | 이번 확인에서 NU40를 업데이트하지 않음 | 인증 코드·서명 키와 실물 복구 검증이 별도로 필요 |
| 로봇 | Motion Lab 동작 시뮬레이션 | 실물 모터나 IMU 입력은 사용하지 않음 |

표시 시험에는 다음 ASCII 카드를 사용했습니다. ACK는 선택된 서비스의 드라이버
처리 응답이며 화면 사진이나 영상이 아닙니다.

```text
WALLABY VOICE
HOST -> NU87 -> NU40
OLED ACK OK
LOCAL / OFFLINE
```

## 빌드 식별

NU87 코어 소스와 배포 도구 기준 커밋은 `b8c5635`입니다. Zephyr는 전역 체크아웃이
아닌 전용 West 워크스페이스의 `e70694102ad6f910485155a824c5483daf605a9f`를 사용했습니다.
`program.conf;local.conf`와 앱의 HAL 호환 수정을 포함했습니다.

| 이미지 | 기록 위치 | 바이트 | SHA-256 |
| --- | --- | --- | --- |
| bootloader_all.bin | 0x000000 | 28,928 | `74d86e9c144fe5018835de4dfb308c1350057d614fda016576099ea379a60d6e` |
| km0_km4_app.bin | 0x014000 | 678,432 | `b817d26b49c3a2ca14950a20db009bac67220a750892d420c0b23541bafd520f` |

개인 네트워크 설정을 포함한 이미지는 Git에 올리지 않습니다. NU40의 현재 펌웨어
버전은 BLE 검색 이름만으로 확정하지 않았습니다.

## 시작과 복구

[Linux / WSL 안내](../../firmware/nu87_lcd_gateway/LINUX.md)의 `flash_linux.py`로
이번 보드에서는 BOOT·RESET 버튼 없이 자동 ROM 진입과 업로드가 됐습니다. 모든 보드의
버튼 없는 기록을 보장하는 것은 아닙니다.

빈 eFuse 경고를 지나가려면 관측 도구에 `--acknowledge-efuse-warning`을 지정합니다.
이번에는 재부팅 후에도 그 경고가 다시 나왔기 때문에, 새 ROM 부팅 표시와 완전한 경고를
확인할 때만 최대 3회 개행을 보내는 옵션을 사용했습니다. eFuse를 기록한 것은 아닙니다.
Wi-Fi 연결 후에는 시리얼을 다시 열지 않고 HTTP로 확인합니다. 포트 열기가 보드를
재시작할 수 있습니다.

원본 부팅 로그, 연결 IP, USB 고유번호와 인증 설정은 로컬 Git 제외 경로에 보관합니다.
공유용 [실기 근거 요약](hardware-check.json)에는 그 값이나 원시 음성을 넣지 않았습니다.

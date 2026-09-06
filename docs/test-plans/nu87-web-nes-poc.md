# NU-87 Wi-Fi 웹 NES PoC 실기 시험 계획

이 문서는 GitHub issue #35의 재현 가능한 실기 확인 및 결과 기록 양식이다.
상용 ROM은 프로젝트 입력이나 산출물이 아니다. 시험자가 사용 권한을 가진
로컬 iNES 파일을 브라우저에서 직접 선택하며 파일은 NU-87로 전송되지 않는다.

## 시험 질문과 선택 근거

1. NU-87의 Zephyr Ameba Wi-Fi 경로가 격리된 2.4 GHz AP에서 association과
   DHCP를 완료하는가?
2. 100 Hz ADC/GPIO sampler를 방해하지 않고 HTTP와 50 Hz WebSocket을 함께
   운용할 수 있는가?
3. 브라우저가 로컬 40 KiB급 NROM을 실행하면서 실제 조이스틱의 방향/A 입력을
   100 ms 이내 지연과 10분 안정성으로 사용할 수 있는가?

ROM을 보드 flash에 넣지 않는 이유는 현재 storage partition 24 KiB가 목표
ROM보다 작고 ROM 배포·저장 책임을 펌웨어에 추가할 필요가 없기 때문이다.
브라우저 실행은 MCU의 emulator/framebuffer RAM 소비도 피한다.

## 준비물과 안전 경계

- NU-87과 데이터 통신 가능한 USB-C 케이블
- 조이스틱 모듈 `GND/5V/VRx/VRy/SW`
- 240 fps 촬영이 가능한 휴대전화 또는 카메라
- NU-87과 시험용 Mac만 통신하도록 홈 LAN/인터넷에서 분리한 2.4 GHz AP
- 시험자가 사용 권한을 가진 iNES 1.0 ROM 한 개

전원을 끈 상태에서 `GND→GND`, 모듈의 `5V` 표기 핀→**NU-87 3V3**,
`VRx→PB1`, `VRy→PB2`, `SW→PB22`를 연결한다. 5 V를 입력하지 않는다.
터치 모듈은 연결하지 않는다. SW는 내부 pull-up과 polling을 사용하므로 외부
10 kohm 저항은 필요 없다.

현재 AmebaD random 경로는 CSPRNG로 검증되지 않았고 HTTP/WebSocket은
평문이다. 메인 가정 Wi-Fi SSID/PSK를 쓰지 않는다. 기본 OPEN 시험 AP를
권장한다. WPA2가 필요하면 별도 폐기 가능 PSK와
`CONFIG_MW_ALLOW_INSECURE_TEST_PSK=y`를 사용하고 시험 뒤 폐기한다.

## 사전 게이트

- [x] pinned west revision에서 `./micro-wallaby/tools/apply_west_patches.sh` 통과
- [x] portable C host test 통과
- [x] browser/controller/JSNES test 통과
- [x] 추적 중인 `.nes`/`.rom` 파일이 없음(확장자 대소문자 무관)
- [x] no-blob Zephyr 빌드 통과. 컴파일 확인일 뿐 실기 Wi-Fi 판정에는 사용 금지
- [x] Realtek blob + `program.conf` + 공개 placeholder `wifi.example.conf` 빌드와
      image merge 통과
- [x] flash wrapper dry-run이 `0x000000`, `0x014000` 두 이미지를 확인
- [x] Realtek blob + `program.conf` + 로컬 `wifi.conf` 빌드와 image merge 통과
- [ ] 설정한 SSID가 격리 시험망이며 firmware에 placeholder가 아님을 재확인.
      2026-09-06 smoke에는 가정용 WPA 망을 사용해 정식 격리망 조건을 충족하지 않음

## 실기 절차와 합격 기준

| 단계 | 조작 | 합격 기준 | 기록 |
| --- | --- | --- | --- |
| 1 | 보드를 flash하고 재부팅. `Efuse empty` 입력 대기가 나오면 UART에서 Enter | 초기화 대기 중 연결을 강행하지 않고 재시도하며, Enter 뒤 assert/reset 없이 계속 진행 | UART 로그 |
| 2 | Wi-Fi 연결 대기 | 30초 이내 DHCP URL 출력. 실패 시 명확한 오류와 재시도 | SSID 비공개, 소요 시간 |
| 3 | 같은 격리 LAN에서 `/api/status` 접속 | HTTP 200, `wifi_ipv4:true`, `heap_stats_ok:true` | 원본 JSON |
| 4 | 웹 페이지 접속 | HTML/JSNES가 외부 요청 없이 로드 | 브라우저 Network 기록 |
| 5 | 조이스틱 X/Y 끝점, 대각선과 중앙 이동 | 값이 `-1000..1000` 안에서 변함. 좌우 조작 시 `abs(y) < 250`, 상하 조작 시 `abs(x) < 250`; 대각선에서는 두 축이 방향 임계값을 넘고 중앙에서는 두 방향 모두 해제 | 축별 범위/반전·비조작축 값 |
| 6 | SW 10회 누름 | SW 상태와 NES A edge가 누름/해제마다 1회 대응 | 성공 횟수 |
| 7 | B/Start/Select 화면 버튼과 키보드 사용 | 하드웨어 방향/A와 동시에 눌러도 서로 해제시키지 않음 | 조합 결과 |
| 8 | 로컬 iNES 선택 | upload 요청 없이 브라우저에서 시작. 잘못된/과대 파일 거부 | 파일 크기만 기록 |
| 9 | 플레이 중 브라우저 Wi-Fi를 잠시 끊음 | 250 ms 이후 입력이 모두 해제되고 재연결 후 복구 | stuck input 유무 |
| 10 | 아래 고속 촬영 지연 시험 | 20회 측정, p95가 100 ms 이하 | mean/p95/max |
| 11 | 10분 연속 조작 | reset/assert/멈춤/stuck 없음 | 시작/종료 status JSON |

4·7·8·9단계는 각각 정적 자산 로드, 화면/키보드와 하드웨어 input mux, ROM
선택부터 첫 frame, WebSocket close부터 stale 해제와 재연결까지 `app.js` 통합
경로를 확인한다. 자동 시험은 순수 입력·ROM 경계와 synthetic frame 실행을 맡고,
브라우저 API/DOM 배선은 이 실기 절차에서 판정한다.

## 실제 종단 지연 측정

페이지의 queue/상대 전송 지연/HTTP RTT는 서로 다른 시계 또는 왕복시간을
사용하므로 100 ms 합격 근거로 쓰지 않는다.

1. 휴대전화 240 fps 영상 한 화면에 손가락·조이스틱 SW와 브라우저의 `SW / A`
   표시가 함께 보이도록 고정한다.
2. 1초 이상 간격으로 SW를 눌렀다 놓는 동작을 20회 수행한다.
3. 각 누름에서 물리 접점 동작이 보이는 첫 frame과 화면이 `눌림`으로 바뀐 첫
   frame의 차이를 센다.
4. `frame 차이 × 1000 / 실제 촬영 fps`로 ms를 계산한다.
5. 오름차순 20개 중 19번째 값을 p95로 기록한다. 목표는 **p95 ≤ 100 ms**다.
   mean, p95, max와 촬영 fps도 함께 남긴다.

게임 화면 반응을 추가로 측정할 수 있지만 게임 자체 frame/logic 지연이 섞이므로
SW 상태 측정을 기본 판정값으로 사용한다.

## 10분 soak 기록 방법

시작 직후와 10분 종료 직전에 `/api/status` 원본 JSON을 각각 저장한다. 다음
delta와 최저값을 기록한다.

- `input_samples`: 약 60,000 증가 여부
- `input_errors`: 증가량 0
- `deadline_misses`: 증가량 0
- `packets_sent`: WebSocket 연결 중 약 30,000 증가 여부
- `send_errors`: 의도한 단절 시험을 제외한 증가량 0
- `stale_input_frames`: 정상 sampler 동작 중 증가량 0
- `heap_free_bytes`: Wi-Fi 연결 뒤 현재 여유
- `heap_min_free_bytes`: 실행 중 system heap 최저 여유

의도적으로 Wi-Fi를 끊는 9단계는 soak 전 또는 별도 실행으로 수행해야 전송 오류
판정과 섞이지 않는다.

## 정적 resource와 실기 결과

최종 빌드가 갱신될 때마다 `west build`의 출력값을 아래에 기록한다. 정적 RAM
headroom은 SRAM capacity에서 정적 사용량을 뺀 값이며 runtime system heap
여유와 다른 지표다.

2026-09-06의 상세 관찰과 미완료 항목은
[NU-87 Wi-Fi 웹 NES PoC 실기 smoke 결과](../test-results/2026-09-06-nu87-web-nes-smoke/summary.md)에
기록했다. 해당 실행은 가정용 WPA 망에서 수행했으므로 정식 격리망 시험을
대체하지 않는다.

| 항목 | 결과 |
| --- | --- |
| firmware commit | 본 문서와 같은 구현 commit |
| Zephyr revision | `e70694102ad6f910485155a824c5483daf605a9f` |
| Realtek HAL revision | `9a4caf9846d6f0ceb6b38f971a771b1325c1e4b9` |
| Flash 사용 / slot | 509,812 / 2,015,232 B (25.30%) |
| 정적 RAM 사용 / SRAM | 139,522 / 475,136 B (29.36%) |
| 정적 RAM headroom | 335,614 B |
| 현재 clean build `zephyr.bin` SHA-256 | `41930385f474fac4ef270fc85696a8f56278db6a555e75c60c7093c25b54a2f9` — build 확인, 실기 이미지와 구분 |
| no-blob Flash / RAM | 203,108 B / 120,640 B |
| flash dry-run | bootloader 28,928 B @ `0x000000`; application 575,488 B @ `0x014000` |
| 2026-09-07 실기 이미지 | Flash 509,844 B; RAM 139,522 B; SHA-256 `f8387594f317b7f72f7e444ca303ed21ca764f740c0fc5c57a496aee7632db9e` |
| 시험 일시 / 장소 | 2026-09-06 / 가정용 WPA 망(정식 격리망 조건 미충족) |
| association / DHCP | 연결·DHCP 성공, 55.592초 — **FAIL**, 30초 목표 초과 |
| Wi-Fi 후 현재 / 최저 heap | 32,336 / 30,120 B |
| 조이스틱 방향 / SW | X-only/Y-only/대각선/중앙 해제와 웹 4방향 PASS (interactive smoke) / SW 정식 10회 기준 미완료 |
| 지연 mean / p95 / max | 미완료 |
| 시작→10분 input sample/error/deadline | +46,211 / +0 / +0 — clock 수정 전 역사적 결과; 안정성 PASS, rate FAIL |
| 시작→10분 packet/send-error | +22,029 / +0 — clock 수정 전 역사적 결과; 안정성 PASS, rate FAIL |
| clock 수정 후 단기 rate | host 30.04761초, uptime +30,077 ms, input +3,007(99.98 Hz) — clock·sampler PASS; WebSocket +1,421(47.25 Hz) 관찰, 오류 delta 0 |
| 로컬 ROM 실행 | 미완료 |
| reset/assert/stuck | 이번 smoke 관찰 구간 reset/assert 없음; 전체 판정 미완료 |
| 결론 / 후속 issue | 전체 실기 미완료 — 상세 결과 문서 참조 |

이 soak는 clock 수정 전 이미지에서 호스트 시간 600초 이상 동안 수행했지만
NU-87 uptime은 462.108초만
증가했다. AmebaD DTS의 260 MHz 선언과 SoC 초기화의 실제 200 MHz가 다른 것이
원인이었다. clock 수정 이미지의 약 30초 측정에서는 uptime과 host time이
일치하고 100 Hz sampler가 확인됐다. 수정 후 10분 안정성은 별도로 재시험해야
한다.

## NU-87 자체 에뮬레이션 가능성

현 단계 판단은 **조건부 가능성이 있으나 미검증, 별도 후속 Spike 필요**다.

- 긍정 근거: 실제 Cortex-M33 200 MHz와 linker RAM headroom은 compact
  NROM core benchmark를 시도할 수 있는 수준이다.
- resource 관찰: Wi-Fi 연결 뒤 system heap 현재/최저 여유는
  32,336 / 30,120 B였다. 단일 smoke snapshot이므로 최종 resource gate는 아니며,
  emulator frame time, ROM 저장 방식과 audio 비용은 아직 측정하지 않았다.
  현재 system heap은 64 KiB다.
- 전송 제약: 256×240×60 fps는 8 bpp 약 3.7 MB/s, RGB565 약 7.4 MB/s라 raw
  frame 전송은 현실적인 기본안이 아니다.

따라서 이번 browser-first PoC가 안정성/resource 기준을 통과한 다음,
재배포 가능한 NROM으로 CPU/RAM을 benchmark하고 tile delta/압축 방식을 검증한다.

실기 항목과 원본 기록을 모두 채우기 전에는 issue #35를 Done으로 처리하지 않는다.

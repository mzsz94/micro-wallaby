# NU-87 Wi-Fi 웹 NES PoC 실기 smoke 결과

이 문서는 2026-09-06에 수행한 NU-87 Wi-Fi·HTTP·WebSocket smoke test와
2026-09-07에 수행한 system clock·조이스틱 후속 확인 결과다. 시험에는
비밀번호가 설정된 일반 가정용 Wi-Fi를 사용했으므로,
격리된 2.4 GHz AP를 요구하는 정식 시험 조건은 충족하지 않았다. 따라서 아래
`PASS`는 명시된 smoke 범위에만 적용되며 전체 PoC 합격을 뜻하지 않는다.

접속 URL, IP 주소, SSID와 PSK는 의도적으로 기록하지 않는다. 시험에 사용한
자격증명도 이 문서나 저장소 산출물에 포함하지 않는다.

## 시험 이미지와 환경

| 항목 | 기록 |
| --- | --- |
| Flash 사용 / slot | 509,844 / 2,015,232 B (25.30%) |
| 정적 RAM 사용 / SRAM | 139,522 / 475,136 B (29.36%) |
| 정적 RAM headroom | 335,614 B |
| `zephyr.bin` SHA-256 | `f8387594f317b7f72f7e444ca303ed21ca764f740c0fc5c57a496aee7632db9e` |
| Flash 기록 | PASS — 이미지 기록 완료 |
| UART | 1,500,000 baud |
| 시험망 | NON-COMPLIANT — 일반 가정용 WPA 보호망; 격리 AP 아님 |

SHA-256은 이번 결과와 시험 이미지를 연결하기 위한 식별값이다. 민감정보가
포함된 로컬 설정 파일 자체는 보존하거나 첨부하지 않는다.

## 결과 요약

| 확인 항목 | 결과 | 근거 및 남은 조건 |
| --- | --- | --- |
| 빌드와 flash | PASS (smoke) | 위 크기로 링크·이미지 생성을 완료하고 보드 기록에 성공 |
| Wi-Fi 초기화 안전성 | PASS (1회 smoke) | 준비 전 첫 연결 요청이 `-EAGAIN`으로 안전하게 거절됐고 BUS FAULT가 발생하지 않음 |
| Association·DHCP 기능 | PARTIAL | DHCP와 웹 접속은 성공했으나 DHCP 완료가 55.592초로 30초 목표를 초과 |
| 정식 네트워크 조건 | FAIL | 격리 시험망이 아닌 가정용 Wi-Fi를 사용함 |
| `/api/status` | PASS (smoke) | 동적 JSON 응답과 Wi-Fi·WebSocket·입력·heap 상태를 수집함 |
| 정적 resource, TX count 20 | FAIL | HTTP 200 헤더 뒤 정적 gzip 본문이 전송되지 않고 정지 |
| 정적 resource, TX count 32 | PASS (단독 요청) | `/`, `controller.js`, `app.js`, `jsnes.min.js` 전체 길이 수신 및 gzip 원본과 byte-identical 확인 |
| 초기 동시 요청 | FAIL (1회) | 겹친 최초 요청에서 `jsnes.min.js`가 5,460 / 31,469 B 뒤 timeout; WebSocket send error 1회 동반 |
| 반복 `jsnes.min.js` 요청 | PASS (5회) | 다섯 번의 단독 요청이 모두 31,469 B로 완료 |
| 조이스틱 4방향·축 독립성 | PASS (interactive smoke) | 다채널 ADC 수정 뒤 X-only, Y-only, 대각선, 중앙 해제와 브라우저 상·하·좌·우를 사용자 확인 |
| 로컬 ROM 실행 | NOT RECORDED | 브라우저와 조이스틱은 정상 확인했지만 적법한 로컬 ROM 선택·실행 결과는 명시적으로 기록하지 않음 |
| clock·sampler 수정 후 주기 | PASS (단기 측정) | host 30.04761초 동안 uptime 30.077초, input 3,007개(99.98 Hz); WebSocket은 1,421개(47.25 Hz) 관찰, 오류 delta 0 |
| 10분 안정성 | PARTIAL | reset/assert/error/heap 감소는 없었으나 잘못된 system clock 때문에 sampler·WebSocket rate 기준 미달 |

`PASS (1회 smoke)`인 Wi-Fi 초기화 결과는 준비 전 vendor join 진입을 차단하는
보호 경로가 한 번 동작했음을 뜻한다. 반복 cold boot 기준을 충족한 결과는
아니므로 초기화 race의 정식 종료 근거로 사용하지 않는다.

## 정적 resource 전송

TX buffer count가 20인 이미지에서는 동적 `/api/status` 응답은 성공했지만 정적
gzip 본문 전송은 HTTP 200 헤더 뒤에서 멈췄다. TX buffer count를 32로 늘린
이미지에서는 다음 resource를 끝까지 수신했다.

| Resource | gzip 본문 크기 | 결과 |
| --- | ---: | --- |
| `/` | 3,031 B | PASS |
| `/controller.js` | 2,406 B | PASS |
| `/app.js` | 3,280 B | PASS |
| `/jsnes.min.js` | 31,469 B | PASS |

네 파일 모두 수신한 gzip bytes가 firmware에 포함한 원본과 동일했다. 이 A/B
결과는 TX buffer 부족이 정적 본문 정지의 직접 조건이었음을 지지한다.

다만 브라우저의 초기 겹친 요청 중 한 번은 `jsnes.min.js`가 5,460 B까지만
수신된 뒤 timeout됐고, 같은 구간에 WebSocket send error가 한 번 발생했다.
이후 `jsnes.min.js` 단독 요청 다섯 번의 완료 시간은 다음과 같다.

```text
0.680 s, 0.790 s, 0.615 s, 0.623 s, 0.669 s
```

평균은 0.675초, 최소는 0.615초, 최대는 0.790초다. 단독 반복 성공은 정적 파일
내용과 기본 전송 경로를 확인하지만, 최초 페이지의 동시 asset 요청과 WebSocket을
함께 사용한 안정성까지 합격시키지는 않는다.

## 10분 runtime soak — clock 수정 전 역사적 결과

호스트의 monotonic clock을 기준으로 50초 대기 12회, 총 600초 이상 상태를
관찰했다. 시작과 종료 `/api/status` 및 delta는 다음과 같다.

| 지표 | 시작 | 종료 | Delta |
| --- | ---: | ---: | ---: |
| `uptime_ms` | 619,215 | 1,081,323 | +462,108 |
| `packets_sent` | 20,864 | 42,893 | +22,029 |
| `send_errors` | 1 | 1 | **0** |
| `input_samples` | 61,921 | 108,132 | +46,211 |
| `input_errors` | 0 | 0 | **0** |
| `deadline_misses` | 0 | 0 | **0** |
| `heap_free_bytes` | 32,336 | 32,336 | 0 |
| `heap_min_free_bytes` | 30,120 | 30,120 | 0 |

이 측정은 system clock 수정 전 이미지로 수행했으며, 수정 후 rate 합격 근거로
사용하지 않는다. WebSocket은 전 구간 연결 상태였고 reset, assert, 전송 정지,
새 오류와 heap
감소는 관찰되지 않았다. 따라서 이 구간의 **연결·메모리 안정성 smoke는
PASS**다. 시작 전에 발생한 초기 동시 요청의 WebSocket send error 1회는
delta에 포함되지 않는다.

반면 600초가 넘는 실제 시간에 보드 uptime은 462.108초만 증가했다. 별도의
20.106초 계측에서도 uptime 15.415초, input sample 1,541개, WebSocket packet
733개만 증가했다. 실제 rate는 각각 약 76.65 Hz와 36.46 Hz로 목표 100 Hz와
50 Hz에 미달하므로 **주기 rate 기준은 FAIL**이다.

## System clock 불일치

원인은 pinned Zephyr의 AmebaD clock description과 SoC 초기화가 서로 다른
것이다.

- `dts/arm/realtek/amebad/amebad.dtsi`의 `clk_sys`는 260 MHz를 선언한다.
- `soc/realtek/ameba/amebad/soc.c`는 `SystemSetCpuClk(CLK_KM4_200M)`으로 실제
  CPU를 200 MHz로 설정한다.
- Cortex-M SysTick은 선언된 260 MHz로 timeout을 계산하므로 실제 시간은
  `200 / 260 = 10 / 13` 비율로 느리게 진행한다. 측정된 약 76.65%와 일치한다.

`patches/zephyr/0003-dts-arm-realtek-amebad-fix-system-clock-rate.patch`에서
`clk_sys`를 실제 200 MHz로 맞췄다. 수정 이미지를 flash한 뒤 다음 단기
측정으로 clock과 주기 동작을 확인했다.

| 지표 | 결과 |
| --- | ---: |
| host monotonic 경과 | 30.04761초 |
| board `uptime_ms` delta | +30,077 ms |
| `input_samples` delta | +3,007 (99.98 Hz) |
| WebSocket `packets_sent` delta | +1,421 (47.25 Hz) |
| input/send/deadline 오류 delta | 0 |

host와 board 경과 시간이 약 0.10% 이내로 일치하고 100 Hz sampler가 목표
주기로 동작하므로 clock 수정과 sampler rate는 **PASS (단기 측정)**다.
WebSocket은 목표 50 Hz에 근접했고 관찰 구간 오류가 없었다. 다만 clock 수정
후 이미지를 사용한 10분 soak는 아직 수행하지 않았으므로, 위의 수정 전
10분 안정성 결과와 합쳐 장기 rate 합격으로 해석하지 않는다.

## 조이스틱, ROM과 오디오 경계

이전 별도 실기에서는 ADC 원시 범위로 VRX `0..4079`, VRY `0..4081`을
관찰했지만, 두 값을 순차적인 단일-channel ADC read로 얻었다. 이후 한 축을
움직일 때 X와 Y가 함께 변하는 현상이 확인되어 이 범위 기록은 축 독립성
증거로 무효화했다.

후속 구현은 ADC4와 ADC5를 하나의 다채널 sequence로 요청한다. Ameba ADC
driver는 conversion list의 모든 결과를 읽고 channel ID를 검증한 뒤 요청한
channel 순서로 buffer에 배치한다. 이 수정 이미지를 사용해 다음을 사용자와
대화형으로 확인했다.

- 좌우 조작에서는 X만 방향 임계값을 넘고 Y는 중립을 유지한다.
- 상하 조작에서는 Y만 방향 임계값을 넘고 X는 중립을 유지한다.
- 대각선에서는 두 축이 함께 반영된다.
- 중앙으로 놓으면 두 방향 입력이 모두 해제된다.
- 브라우저에서 상·하·좌·우 입력이 정상적으로 표시된다.

따라서 이번 firmware와 브라우저의 4방향 및 축 독립성은 interactive smoke
범위에서 PASS다. 시험용 ROM 파일은 저작권과 재배포 경계 때문에 결과에
제공하거나 첨부하지 않는다. 브라우저의 로컬 ROM 선택과 실제 게임 입력은
명시적인 실행 기록이 없으므로 합격으로 기록하지 않는다. 현재 PoC에는
오디오가 없으며 오디오 시험도 수행하지 않았다.

## 시험 계획 대비 상태

| 단계 | 상태 |
| --- | --- |
| 1. boot·초기화 대기 | PARTIAL — `-EAGAIN` 안전 거절과 무 BUS FAULT를 1회 확인; 반복 cold boot 미수행 |
| 2. Wi-Fi 연결 | FAIL — DHCP 성공, 55.592초로 30초 기준 미달 |
| 3. `/api/status` | PASS (smoke) — 동적 상태 응답 수집 |
| 4. 웹 페이지 정적 자산 | PARTIAL — TX 32에서 개별 자산은 완전 수신, 초기 동시 요청 timeout 1회 |
| 5. 조이스틱 X/Y | PASS (interactive smoke) — X-only/Y-only/대각선/중앙 해제와 웹 4방향 확인 |
| 6. 조이스틱 SW 10회 | NOT RUN |
| 7. 화면 버튼·키보드 조합 | NOT RUN |
| 8. 로컬 iNES 선택·실행 | NOT RECORDED — ROM 미제공, 실행 결과 미기록 |
| 9. 단절 fail-safe·재연결 | NOT RUN |
| 10. 100 ms 종단 지연 | NOT RUN |
| 11. 10분 연속 조작 | PARTIAL — 수정 전 이미지의 연결·메모리 안정성만 PASS; clock 수정 후 sampler 단기 rate PASS, 수정 후 10분 soak 미수행 |

## 후속 확인

1. 격리된 2.4 GHz 시험 AP에서 동일 이미지 SHA-256을 사용해 다시 시험한다.
2. DHCP 30초 초과 원인을 초기화 대기 시간과 association/DHCP 시간으로 나눠
   timestamp와 함께 기록한다.
3. 브라우저의 동시 asset 요청과 WebSocket을 함께 시작하는 cold-load를 반복해
   timeout과 send error가 0인지 확인한다.
4. 조이스틱 SW 반복 시험과 적법한 로컬 ROM 선택·실행을 별도로 기록한다.
5. clock 수정 이미지에서 시작·종료 `/api/status`를 갖춘 10분 soak를 반복한다.

현재 결과만으로 GitHub issue #35 또는 전체 웹 NES PoC를 Done으로 처리하지
않는다.

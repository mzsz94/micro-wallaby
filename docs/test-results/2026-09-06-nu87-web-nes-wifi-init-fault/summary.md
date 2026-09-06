# NU-87 Wi-Fi 초기화 BUS FAULT 분석

## 시험 조건

- 날짜: 2026-09-06
- 재현 firmware commit: `ac0bd43`
- Zephyr revision: `e70694102ad6f910485155a824c5483daf605a9f`
- UART: `/dev/cu.usbserial-10`, 1,500,000 baud, 8-N-1
- 터치 모듈: 미사용
- 조이스틱: 100 Hz sampler 사용

SSID와 비밀번호는 기록하지 않는다. 재현 빌드에는 비밀번호 문자열이 있었지만
보안 선택이 OPEN인 별도 구성 오류도 있었다. OPEN 경로가 vendor 호출에 전달한
PSK는 `NULL`, 길이는 0이므로 이 구성 오류는 BUS FAULT의 직접 원인이 아니다.

## 1차 실기 결과

| 확인 항목 | 결과 | 근거 |
| --- | --- | --- |
| ROM boot와 Zephyr 시작 | PASS | Zephyr banner와 앱 시작 로그 출력 |
| 조이스틱 sampler 시작 | PASS | `Joystick sampler started at 100 Hz` |
| HTTP/WebSocket listener 시작 | PASS | port 8080 listen 로그 출력 |
| Wi-Fi 초기화 | BLOCKED | empty-EFUSE 확인 입력 대기 |
| Wi-Fi connect 안전성 | FAIL | 0.507초에 BUS FAULT, system halt |
| 브라우저/DHCP/soak | 미실행 | Wi-Fi connect 전에 중단 |

핵심 fault 값은 다음과 같다.

```text
PC = 0x0e0308c2
LR = 0x0e030863
r3 = 0x00000000
thread = mw_wifi
fault = imprecise data bus error
```

## 원인

`wifi_init()`은 별도 vendor thread를 만들고 즉시 반환한다. 그 thread는 adapter의
바깥 구조를 먼저 게시한 다음 EFUSE가 비어 있으면 UART 입력을 기다린다. adapter
내부 구조 초기화는 Enter 입력 뒤에 이어진다.

애플리케이션은 고정 500 ms 뒤 연결을 요청했다. `rtw_joinbss_start_api()`는 아직
NULL인 adapter 하위 포인터를 `r3`로 받은 뒤 `[r3 + 0x155]`를 읽었다. PC/LR,
`r3=0`, 0.507초 타이밍과 EFUSE 입력 대기 로그가 모두 이 초기화 race와 일치한다.

```text
wifi_init thread: EFUSE 확인 입력 대기 ──────── Enter ── 내부 초기화 완료
app mw_wifi:               500 ms ── connect ── NULL + 0x155 fault
```

## 수정

- Zephyr Ameba driver의 connect 진입부에서 `wifi_is_running(STA_WLAN_INDEX)`를
  먼저 검사한다.
- 준비 전에는 vendor join 함수를 호출하지 않고 `-EAGAIN`을 반환한다.
- 앱의 기존 retry 경로가 timeout을 취소하고 10초 뒤 다시 시도한다.
- `RTK_WIFI_EVENT_STA_START` flag는 현재 링크에서 event producer가 제거될 수
  있어 사용하지 않는다.
- OPEN인데 비밀번호 문자열이 설정된 경우 앱 시작 시 `-EINVAL`로 거절한다.
- EFUSE는 쓰지 않는다. 프롬프트 뒤 Enter는 RAM의 확인 상태만 바꾸며 재부팅하면
  다시 묻는다.

## 데스크톱 검증

- portable C host tests: PASS
- browser/controller/JSNES tests: 10/10 PASS
- NU-87 실제 blob compile/link: PASS
- Ameba image merge: PASS
- 최종 기계어에서 `wifi_is_running()` 호출이 carrier/state 변경 및
  `wifi_connect_zephyr()`보다 앞에 배치됨을 확인
- flash dry-run: bootloader 28,928 B @ `0x000000`, application 571,360 B @
  `0x014000`

## 남은 실기 게이트

1. 격리 AP와 일치하도록 OPEN/PSK 설정을 한 가지로 확정한다.
2. 수정 이미지를 flash하고 EFUSE 프롬프트 뒤 Enter를 누르지 않은 채 30초 이상
   BUS FAULT 없이 `-EAGAIN` 재시도하는지 확인한다.
3. Enter 뒤 `Band`, Wi-Fi initialization, heap 로그가 이어지는지 확인한다.
4. 다음 재시도에서 association과 DHCP가 완료되는지 확인한다.
5. 인터페이스 MAC이 all-zero/all-FF가 아닌 unicast인지 확인한다.
6. 조이스틱·웹·지연·10분 soak 절차를 수행한다.

이 게이트가 끝날 때까지 GitHub issue #35는 Done으로 처리하지 않는다.

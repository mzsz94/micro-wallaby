# NU-87 터치·조이스틱 I/O 실기 시험 계획

- 상위 작업: [#1 — [M0] microWallaby v0 범위와 시스템 구조 확정](https://github.com/mzsz94/micro-wallaby/issues/1)
- 관련 보드 갭 작업: [#4 — [M0] NU-87 보드 지원 검증 및 갭 분석](https://github.com/mzsz94/micro-wallaby/issues/4)
- 구현 브랜치: `feat/m0-nu87-io-validation`
- 기준 앱: `firmware/nu87_io_bringup`

## 목적

현재 보유한 터치 센서와 조이스틱을 이용해 NU-87의 Zephyr GPIO, ADC,
주기 실행, fault latch와 RGB 상태 표시 경로를 실제 보드에서 반복 검증한다.
이 시험은 최종 센서와 모터를 검증하는 것이 아니라 그 장치가 사용할
소프트웨어 경계를 먼저 검증한다.

## 시험 범위

### Fixture A — 결합 시험

- 터치 `D0` → `PA15`
- 조이스틱 `VRX` → `PB1/ADC4`
- 조이스틱 `VRY` → `PB2/ADC5`
- 조이스틱 `SW` → `PB22`, 내부 pull-up, active-low polling
- 조이스틱의 `5V` 표기 전원 핀 → NU-87 `3.3V` (실제 5V 연결 금지)
- 터치 `A0`는 연결하지 않는다.

`D0`는 터치 센서의 출력이며 조이스틱에는 존재하지 않는다.

### Fixture B — 터치 아날로그 특성 시험

- 보유 터치 모듈 핀: `VCC/GND/D0/A0`
- 조이스틱을 전부 분리한다.
- 터치 `A0` → `PB1/ADC4`
- 터치 `D0` → `PA15`

두 fixture는 overlay와 Kconfig fragment로 분리한다. 실행 중 핀을 재할당하지
않으며 반드시 전원을 끈 뒤 배선을 변경한다.

## 합격 기준

| ID | 시험 | 합격 기준 |
| --- | --- | --- |
| T0 | 전기 사전 점검 | 모듈 출력이 모두 0~3.3 V이고 VCC-GND 단락이 없다. |
| T1 | build·boot·console | 고정 Zephyr SHA에서 pristine build가 되고 우선 20회, 완료 전 100회 reset에서 정상 부팅한다. |
| T2 | RGB self-test | PA13 Red, PA12 Green, PA14 Blue가 순서대로 정확히 점등한다. |
| T3 | 터치 D0 | touch/release 50회가 debounce 후 1:1이며 5분 idle 오탐이 없다. |
| T4 | 조이스틱 ADC | 각 축 span이 full-scale 60% 이상, center는 35~65%, center noise p-p는 5% 이하이다. |
| T5 | 조이스틱 SW | 100회 polling event가 1:1이며 5분 idle 오탐이 없다. 외부 pull-up 전까지 잠정 결과로 표시한다. |
| T6 | fault/clear | 터치 fault가 latch되고 해제만으로 복귀하지 않으며, neutral 상태에서 fault 후 SW 해제→새 누름→2초 유지 순서를 만족할 때만 clear된다. |
| T7 | reset/fail-closed | 터치 active 또는 입력 미준비 상태로 부팅하면 `FAULT`이며 motor command는 항상 존재하지 않는다. |
| T8 | 1시간 soak | unexpected reset, ADC/GPIO error와 worker deadline miss가 모두 0이다. |
| T9 | 터치 A0 | idle/touched 각 5초의 min/median/max를 기록하고 median 차이가 ADC full-scale 10% 이상이다. |

정식 24시간 deadline 시험, GPIO interrupt와 외부 10 kΩ pull-up 시험은 이
작업의 완료 조건에 포함하지 않고 후속 W1 gate로 유지한다.

## 시험 절차

1. 보드 revision, 모듈 실크와 배선 사진을 기록한다.
2. 멀티미터로 3.3 V 공급과 각 출력 전압 범위를 확인한다.
3. Fixture A를 build·flash하고 console 1,500,000 baud에서 시작 로그를 보존한다.
4. `raw_d0`의 idle/touched 전기 레벨을 확인해 overlay의 active level을 확정한
   뒤, 필요하면 overlay를 바꾸고 formal run을 다시 시작한다.
5. formal run 재부팅 후 첫 5초는 조이스틱을 중립에 고정해 `diag_center` 완료
   로그를 얻고, 이어 총 5분간 터치와 SW를 조작하지 않아 idle 오탐 기준을
   먼저 판정한다.
6. idle 판정이 완료된 뒤 T3~T8의 의도적 터치·SW·조이스틱 조작을 수행한다.
7. 전원을 끄고 Fixture B로 변경한다. D0가 inactive인 채 A0 idle 5초 통계가
   완료될 때까지 기다린 뒤 터치를 유지해 touched 5초 통계를 얻는다.
8. 결과 요약에 각 기준의 PASS/FAIL과 근거 파일을 연결한다.

## 결과 보존 위치

```text
docs/test-results/YYYY-MM-DD-nu87-io/
├── summary.md
├── build-info.txt
├── wiring.jpg
├── serial.log
├── adc.csv
├── zephyr.dts
├── .config
├── ram_report.txt
└── rom_report.txt
```

`build-info.txt`에는 microWallaby commit, Zephyr SHA, SDK 버전, board revision,
fixture와 touch D0 polarity를 기록한다. 원시 로그를 수정하지 않고 판정 근거는
`summary.md`에 별도로 작성한다.

## 알려진 Zephyr 갭

- NU-87 board support PR은 console, GPIO와 RGB까지 실기 검증했으며 ADC는
  아직 보드별 검증 대상이다.
- Ameba ADC는 multi-channel bitmask를 지원하지 않으므로 ADC4와 ADC5를 각각
  별도 `adc_read()`로 순차 읽는다.
- 현재 ADC binding의 핀 설명과 RTL872xD EVB pinctrl 정의가 일치하지 않는다.
  이 앱은 EVB pinctrl의 `PB1/ADC4`, `PB2/ADC5`를 사용하고 결과를 갭으로 남긴다.
- Ameba GPIO interrupt 설정은 pull 설정을 해제할 수 있으므로 외부 pull-up이
  없는 `PB22` 시험에서는 interrupt를 활성화하지 않는다.
- ADC 초기화 또는 build가 실패하면 센서 불량으로 단정하지 않고 DTS/pinctrl
  최소 재현과 build log를 보존해 upstream 기여 후보로 분류한다.

## 안전 해석 제한

터치 D0는 `E-stop sense` 소프트웨어 경로를 모의할 뿐이다. 액추에이터 rail을
직접 차단하지 않으므로 물리 E-stop이나 100 ms torque-off 합격 증거로 사용할
수 없다. 조이스틱 값도 intent 계산과 RGB 표시에만 사용하고 모터 출력으로
연결하지 않는다.

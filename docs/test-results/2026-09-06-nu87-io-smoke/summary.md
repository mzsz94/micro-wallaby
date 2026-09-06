# NU-87 fixture A 초기 실기 결과

이 기록은 정식 합격 시험 전의 smoke test 결과다. 사용자가 이번 실행에서는
터치 모듈을 시험하지 않기로 했으므로 터치 관련 항목은 실패가 아니라
`NOT RUN`으로 분류한다.

## 결과 요약

| 항목 | 결과 | 근거 및 남은 조건 |
| --- | --- | --- |
| 통합 이미지 빌드 | PASS | Zephyr 링크와 Realtek AmebaD 이미지 병합 완료 |
| USB 직접 기록 | PASS | Mac mini의 `/dev/cu.usbserial-10`에서 115,200 baud로 기록하고 `All images are sent successfully!` 확인 |
| Zephyr 부팅·console | PASS (smoke) | `*** Booting Zephyr OS build e70694102ad6 ***`와 앱 시작 로그 확인. 정식 T1의 20/100회 reset은 미수행 |
| 조이스틱 VRX/VRY | INVALIDATED | VRX `0..4079`, VRY `0..4081` 범위는 관찰했지만 후속 시험에서 두 축이 함께 변함을 확인; 순차 단일-channel read라 축 독립성 증거로 사용할 수 없음 |
| 조이스틱 SW | PASS (smoke) | `raw_sw=1, sw=0`에서 `raw_sw=0, sw=1`로 변하고 해제 후 복귀. 정식 T5의 100회/5분 idle은 미수행 |
| 터치 D0/A0 | NOT RUN | 사용자 선택으로 이번 실행에서 조작하지 않음; polarity도 미확정 |
| runtime 오류 | PASS (관찰 구간) | 관찰한 로그에서 ADC/GPIO/deadline 오류가 모두 0 |

## 관찰 근거

부팅 후 앱은 `SELF_TEST`에서 `SAFE_IDLE`로 전이했다. 첫 중앙 보정 구간의
VRX 범위는 `1906..1940`, VRY 범위는 `1908..2049`였고 두 축 모두 ADC
full-scale의 5% 이내 peak-to-peak였다. 조이스틱을 움직였을 때 양 축에서
거의 전체 ADC 범위를 확인했고 `MANUAL_EMU` 전이와 중립 복귀를 확인했다.
그러나 이 실행은 VRX와 VRY를 각각 별도의 단일-channel ADC sequence로 읽었다.
후속 시험에서 어느 방향으로 움직여도 두 값이 같이 변하는 현상이 확인되었으므로,
이 범위만으로 물리적인 두 축이 독립적으로 읽혔다고 판정한 것은 잘못이었다.

조이스틱 스위치를 누르는 동안 다음 상태가 관찰됐다.

```text
raw_sw=0 sw=1
```

관찰 구간의 상태 로그에는 다음 오류 카운터가 유지됐다.

```text
errors=adc:0 gpio:0 deadline:0
```

## 정식 시험 대비 상태

| 시험 계획 ID | 상태 |
| --- | --- |
| T0 전기 사전 점검 | NOT RUN — 멀티미터 측정 기록 없음 |
| T1 build·boot·console | PARTIAL — build/flash/boot 성공, reset 반복 횟수 미충족 |
| T2 RGB self-test | NOT RECORDED |
| T3 터치 D0 | NOT RUN |
| T4 조이스틱 ADC | INVALIDATED — 범위는 관찰했으나 축 독립성을 입증하지 못함 |
| T5 조이스틱 SW | PARTIAL — 동작 확인, 반복/idle 기준 미충족 |
| T6 fault/clear | NOT RUN |
| T7 reset/fail-closed | NOT RUN |
| T8 1시간 soak | NOT RUN |
| T9 터치 A0 | NOT RUN |

이번 interactive console 출력은 원시 `serial.log` 파일로 저장하지 않았다.
정식 시험에서는 시험 계획에 따라 원시 로그, 배선 사진과 측정값을 별도로
보존해야 한다.

## 후속 정정 및 재확인

2026-09-07 후속 구현에서는 ADC4와 ADC5를 하나의 다채널 ADC sequence로
읽고, driver가 FIFO 결과의 channel ID와 중복·누락을 확인한 뒤 channel 순서로
출력하도록 수정했다. 수정 이미지를 flash한 뒤 사용자가 다음 동작을 대화형으로
확인했다.

- X-only 조작에서 X만 변하고 Y는 중립 유지
- Y-only 조작에서 Y만 변하고 X는 중립 유지
- 대각선 조작에서 두 축이 함께 변함
- 중앙 복귀 시 X/Y 방향이 모두 해제됨

따라서 축 독립성은 후속 다채널 ADC 이미지에서 **PASS (interactive smoke)**로
재확인했다. 이 후속 결과는 위 초기 측정의 숫자를 소급해 유효하게 만드는 것이
아니며, 초기 `errors=adc:0` 카운터 역시 축 매핑의 정확성을 보장하지 않는다.

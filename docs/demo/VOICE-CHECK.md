# 실제 로컬 음성 처리 확인 — 2026-10-11

별도의 [음성 처리 GIF](media/06-local-voice-verified.gif)와
[MP4](media/military-wallaby-local-voice.mp4)는 실행 중인 앱에 합성 한국어 WAV를 넣어
녹화했습니다. 자막으로 발화를 대신한 기존 5개 상황 영상과 구분합니다.

```text
브라우저 WAV 입력 → HostPC faster-whisper CPU int8
  → llama.cpp / Qwen2.5 1.5B Q4_K_M / JSON Schema
  → 원문 대조 → SQLite 저장
  → Wi-Fi HTTP → NU87 → BLE → NU40 OLED 드라이버 ACK
```

## 확인 결과

Windows의 로컬 Microsoft Heami 음성으로 만든 16 kHz 모노 PCM16 WAV 3개를 사용했습니다.
각각 처리 모드를 먼저 선택했습니다. 모델과 패키지는 준비된 로컬 파일을 사용했고,
Python 모델 라이브러리에는 offline 환경 변수를 지정했습니다. 브라우저의 외부 요청과
JavaScript 오류는 없었습니다. 물리적으로 인터넷을 끊은 시험은 하지 않았습니다.

| 입력 | 음성 길이 | 브라우저 처리 시간 | 실제 처리 | OLED |
| --- | --- | --- | --- | --- |
| 보고 항목 질의 | 3.90초 | 18.156초 | `local-llm-evidence` | matching ACK |
| 상황 보고 | 13.57초 | 10.197초 | `local-llm-analysis` | matching ACK |
| 지시 정리 | 9.90초 | 8.316초 | `local-llm-analysis` | matching ACK |

처리 시간은 파일 업로드부터 음성 인식·LLM·무선 표시 응답까지 포함한 이번 1회 관측값입니다.
사람의 발화, 야외 잡음, 다른 PC나 보드에 대한 성능 기준으로 사용하지 않습니다.
녹화 파일의 시간은 별도 반복 실행이므로 표의 값과 다를 수 있습니다.

인식 오류도 그대로 기록했습니다. 예를 들어 “상황 보고”가 “상호한 보고”로, “창고에서”가
“참고해서”로 인식됐습니다. 모델이 이를 정답으로 고쳤다고 주장하지 않습니다. 결과의
**원문 수정**으로 입력창에 복사한 뒤, 위치·시각 등을 확인하고 다시 처리할 수 있습니다.
복사만으로 실행하거나 저장하지 않으며, 재처리하더라도 원래 기록을 보존합니다.

## 작은 모델에 적용한 처리 보완

JSON object만 요청했을 때에는 모델이 전체 발화를 한 문장으로 복사하거나 3개보다 많은
요약 구절을 출력해 기존 원문 검사를 통과하지 못했습니다. llama.cpp에서 지원하는
선택형 `--llm-structured` 경로를 추가했습니다. 원문 문장 선택과 항목 수를 JSON Schema로
제약하되, 출처·원문 일치와 추정·부정 표현 보존 검사는 독립적으로 계속 적용합니다.

쉼표로 나열한 보고 절차는 문장 전체를 선택지로 유지합니다. 보고 요약에서는 이미
추출한 관측·조치·요청을 모델이 생략하더라도 유지합니다. 검증 실패나 모델 장애가 나면
규칙·원문 기반 대체 결과를 표시합니다. 정지와 경고는 LLM을 기다리지 않습니다.

## 재생성

[Linux 실행 안내](../../voice_tactical/LINUX.md)로 모델·앱·게이트웨이를 실행한 뒤,
Playwright와 FFmpeg가 있는 환경에서 다음 명령을 사용합니다.

```bash
WALLABY_PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs \
  node docs/demo/record-live-voice.mjs /path/to/synthetic-korean-report.wav
```

녹화기는 실제 실행 중인 앱에 **상황 보고 WAV 요청 1회**를 보내므로 앱의 SQLite에
기록 1건을 추가하고 선택된 OLED에 상태를 전송합니다. 원본 기록을 삭제하지 않습니다.
촬영용 CSS만 적용하며 응답을 교체하거나 인식 오류를 숨기지 않습니다. GIF·MP4는
`docs/demo/media/`, 원본 WebM·화면·상세 응답은 Git 제외 `build/live-voice-recording/`에 둡니다.
두 공유 영상에는 소리가 없습니다.

[공유용 검증 데이터](voice-check.json)에 모델 revision, 실제 인식 원문과 처리 결과를
남겼습니다. NU40의 연속 마이크 수집, 실제 OLED 픽셀, 사람의 발화 정확도, TTS 청취와
OTA는 이 녹화의 검증 범위에 포함하지 않습니다. 보드 확인 범위는
[실기 기록](HARDWARE-CHECK.md)을 확인하세요.

[녹화 검증 데이터](live-voice-verification.json)에는 별도 촬영 요청의 처리 시간과
GIF·MP4 길이, 프레임 수, SHA-256을 기록했습니다.

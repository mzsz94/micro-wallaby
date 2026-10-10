# militaryWallaby 상황별 데모

보고서의 교범 질의·상황 보고·지시사항 정리를 네 가지 훈련 시나리오로 구성했습니다.
1120×630, 8fps. 개별 장면은 12초, 통합본은 48초입니다. GIF에는 소리가 없습니다.

**[통합 GIF](media/military-wallaby-overview.gif) · [MP4 영상](media/military-wallaby-demo.mp4)**

![militaryWallaby 상황별 데모](media/military-wallaby-overview.gif)

| 장면 | 영상 | 확인하는 흐름 |
| --- | --- | --- |
| 문서를 펼칠 수 없을 때 | [교범 항목 조회](media/01-manual-query.gif) | 질문 → 로컬 훈련 자료 검색 → 출처와 원문 확인 |
| 지나가는 보고를 기록으로 | [상황 보고 구조화](media/02-situation-report.gif) | 보고 원문 → 시각·위치·규모·관측·조치·요청 추출 → SQLite 저장 |
| 전달받은 지시 정리 | [담당·기한 확인](media/03-organize-orders.gif) | 원문 → 담당·기한 추출 → 빠진 기한은 미확인으로 유지 |
| 경고 시 왈라비 정지 | [훈련 경고와 정지](media/04-warning-stop.gif) | 짧은 직접 이동 명령 → 모사 경고 → 이동 차단 → 해제 후 대기 |

## 영상에서 실제로 실행한 것

응답은 현재 `voice_tactical` 서버의 `/api/turn`에서 생성했습니다. 보고 저장은
`/api/records`로 다시 확인했고, 로봇은 기존 Motion Lab의 Canvas와 상태 전이를
재사용했습니다. 경고 전 이동, 경고 후 속도 0, 추가 이동 거절을 녹화 중 검사합니다.

음성 입력은 보고서의 사용 상황을 보여 주는 **자막 연출**입니다. 이 녹화는 실제
STT·LLM·TTS 추론, 이어셋, NU87·NU40 하드웨어를 사용하지 않았습니다. 응답은 현재의
로컬 규칙·근거 검색 경로이며, 자료는 저장소의 개발용 훈련 예시입니다. 공식 교범
질의나 현장 성능을 검증한 영상으로 해석하지 마세요. 이 범위는 영상 하단에도 표시합니다.

보고서에 제안한 whisper.cpp·llama.cpp·Silero VAD·임베딩 RAG의 구현이나 성능을 이
영상만으로 주장하지 않습니다. 현재 앱의 선택형 STT 엔진은 faster-whisper입니다.

## 재생성

Node.js, Python 3, FFmpeg, Chromium과 Playwright가 필요합니다. 예를 들어 저장소
루트에서 녹화 도구를 Git 제외 경로에 설치하고 실행할 수 있습니다.

```sh
npm install --prefix build/demo-recorder playwright@1.64.0
build/demo-recorder/node_modules/.bin/playwright install chromium
WALLABY_PLAYWRIGHT_MODULE="$PWD/build/demo-recorder/node_modules/playwright/index.mjs" \
  node docs/demo/record.mjs
```

`--preview`를 붙이면 GIF를 인코딩하지 않고 각 장면 마지막 화면과 검증 기록을 만듭니다.
`WALLABY_PYTHON`과 `WALLABY_DEMO_PORT`로 Python 실행 파일과 녹화용 포트를 바꿀 수 있습니다.

녹화기는 별도 로컬 서버와 SQLite를 `build/scenario-gifs/`에 만들며, 현재 실행 중인
앱이나 보드 설정은 변경하지 않습니다. 외부 브라우저 요청은 차단합니다. 프레임·로그·
전체 API 응답은 `build/`에, 공유용 GIF·MP4는 `docs/demo/media/`에 저장합니다.

[검증 요약](verification.json)에 파일별 길이·프레임 수와 확인 범위를 기록했습니다.

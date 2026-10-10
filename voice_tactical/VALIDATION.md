# Military Wallaby 검증 기록

검증일: 2026-10-11 (KST). 브랜치: `test/military-wallaby`.

## 기준과 역할

- 시작점: 작업 시작 시 원격 브랜치 중 최신 커밋인 `origin/bugfix/zephyr-ble`의 `59ac448`.
- 시뮬레이터: `origin/feat/motion-simulator`의 `48a04f1`에서 가져와 음성 제어를 연결.
- NU40 OLED 참고: `mzsz94/nu40-ble-display`의 `230a1300d121b3bf25d3125a0cf247679148a40a`.
- NU40DK: 컨트롤 타워, PDM 마이크·BTN4·OLED·MAX98357A 스피커.
- NU87 Tiny: 왈라비, 명령·경고 상태 및 BLE/Wi-Fi 중계. 실물 모터 구동은 포함하지 않음.
- HostPC: 로컬 Whisper·Qwen LLM 서버, 보고·교범 검색·기록과 동작 시각화.

## 자동 검사

| 검사 | 결과 |
| --- | --- |
| `python3 -m unittest discover -s voice_tactical/tests -q` | 94개 통과 |
| `node --test simulation/*.test.mjs voice_tactical/web/*.test.mjs` | 31개 통과 |
| NU87 firmware Python 테스트 | 46개 통과 |
| NU40 초기 설치 패키지 테스트 (`intelhex==2.3.0`) | 2개 통과 |
| 공통 오디오·버튼 C 테스트를 실행하는 Python 테스트 | 1개 통과 |
| `firmware/nu87_lcd_gateway/tests/run_kit_tests.sh` | 프로토콜 13개 및 KIT 콜백·오디오·SMP·경고·로봇 C 검사 통과, ASan/UBSan 적용 |

음성 서버 검사는 세션·프레임 누락, 마이크 시작/정지 경합, 늦은 LLM 응답 취소,
LLM 처리 중 BTN4 입력, 경고 해제 ACK 실패 시 차단 유지, 모델의 근거 불일치·추정·부정
표현 누락, 허용 목록 밖 동작, HTTP 입력 검증, OTA의 이미지/응답/전송 실패를 포함한다.
GitHub Actions에도 호스트 검사를 추가했다. 위 결과는 로컬 실행 결과이며 원격 CI 결과와 구분한다.

## 실제 모델과 브라우저

- Apple Silicon / Python 3.12, faster-whisper 1.2.1, PyAV 16.0.1.
- Whisper 모델: `Systran/faster-whisper-small`, revision
  `536b0662742c02347bc0e980a01041f333bce120`. 로컬 파일만 사용하는 초기화를 확인했다.
- 합성 한국어 WAV(16 kHz mono PCM16)의 “왈라비 앞으로 가”를 실제 Whisper로 인식했다.
  브라우저 WAV 입력부터 시뮬레이터의 전진과 약 0.9초 후 정지까지 확인했다.
  이는 NU40 실물 PDM 마이크 시험은 아니다.
- 기존 로컬 Ollama의 `qwen2.5:7b`를 `127.0.0.1:11435`에서 실행했다.
  `OLLAMA_NO_CLOUD=1`로 클라우드 기능을 끄고 실제 한국어 보고 분석 결과와
  `local-llm-analysis` 응답을 확인했다. 외부 LLM API는 사용하지 않았다.
- `http://127.0.0.1:8767/`에서 역할 표시, 기본 NU40 마이크 입력, 보고·기록,
  소프트웨어 감지 이벤트의 경고 표시·이동 차단·명시적 해제를 확인했다.
  NU87 설정이 없는 상태는 UI에 그대로 표시된다.

## 실제 펌웨어 빌드

NU40는 Zephyr sysbuild로 MCUboot와 앱을 함께 빌드했고 컴파일 경고 없이 완료했다.
생성한 P-256 키로 업데이트 이미지에 서명했으며 `imgtool verify`가 성공했다.
OTA CLI의 `inspect`는 구조와 SHA-256만 검증한다. 공개키 서명 확인은 별도의
`imgtool verify`와 장치의 MCUboot가 담당한다.

| 항목 | 결과 |
| --- | --- |
| NU40 앱 | FLASH 232,784 B / RAM 80,176 B |
| MCUboot | FLASH 27,548 B / RAM 15,936 B |
| NU40 업데이트 이미지 | 232,936 B, version `0.1.0+0` |
| NU40 초기 설치 UF2 | 521,728 B; MBR·기존 UF2 영역 보존 테스트 통과 |
| NU87 공통 페어링 코드 적용 빌드 | FLASH 592,348 B / RAM 193,846 B |
| NU87 병합 이미지 | 658,016 B |

NU40 이미지 해시(MCUboot):
`86dbeb9601eb33bcef5aa45953f5d39c5b80f8f4af0e4f6fbcc6842d892a2717`.
NU40 전송 파일 SHA-256:
`f551963a8d7fec3d39ee48c6897beee491ca96348595f40df4222dff0e17d487`.
NU87 파일 SHA-256:
`3fb6e29f79921bb5c433e812f84923b31b0c92d8b75b472e2a77bfea5d2463eb`.

두 보드 빌드에 동일한 비공개 페어링 파일을 적용했다. 서명 키·페어링 코드·빌드 파일은
Git에서 제외했다. NU87 검증 빌드의 Wi-Fi/API 토큰은 비어 있다. 실물 시험용 빌드는
기존 안내대로 `local.conf`에 개발망 설정을 제공해야 한다.

## 남은 실물 시험

이번 작업에서 보드에 기록하거나 OTA를 전송하지 않았다. 다음은 벤치 검증이 필요하다.

1. NU40 MCUboot 최초 설치와 NU87 개발망 설정 후 BLE 인증 연결.
2. 키트 배선, PDM 음질·잡음, BLE 오디오 처리량과 프레임 손실.
3. BTN4를 눌렀을 때 실제 스피커 경고음, NU87 경고 상태와 화면의 정지·해제.
4. 서명 이미지 OTA 업로드 → 시험 부팅 → 기능 점검 → 명시적 확정.
5. 전원 차단·무선 단절·미확정 리셋에서 복구 및 MCUboot 롤백.

드라이버 ACK는 가청 출력 측정이 아니다. NU87의 기존 테스트 난수 생성기 등 개발 포트의
제한도 유지된다. 이 기록은 실물 음질·무선 안정성·제품 수준 보안을 검증했다고 뜻하지 않는다.

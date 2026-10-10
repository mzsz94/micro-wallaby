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
| NU40 초기 설치 패키지 테스트 (`intelhex==2.3.0`) | 3개 통과 |
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
| NU40 초기 설치 UF2 | 1,826,816 B; 이전 OTA 상태 초기화·MBR·기존 UF2 영역 보존 테스트 통과 |
| NU87 개발망·공통 페어링 코드 적용 최종 빌드 | FLASH 614,192 B / RAM 206,542 B |
| NU87 병합 이미지 | 679,872 B |

NU40 이미지 해시(MCUboot):
`86dbeb9601eb33bcef5aa45953f5d39c5b80f8f4af0e4f6fbcc6842d892a2717`.
NU40 전송 파일 SHA-256:
`f551963a8d7fec3d39ee48c6897beee491ca96348595f40df4222dff0e17d487`.
NU40 최종 초기 설치 UF2 SHA-256:
`071f1bda6ac45805b4222b101fb3a1a9f4bf9f3b424478f8dbc060e987f9ab0b`.
NU87 파일 SHA-256:
`89ab5ab70f06b6f6ed45f5e931c0d6d3e95c53917f8e6edbcb24e0ecdba2b361`.

두 보드 빌드에 동일한 비공개 페어링 파일을 적용했다. 서명 키·페어링 코드·빌드 파일은
Git에서 제외했다. NU87 최종 빌드는 사용자가 비공개 `local.conf`에 입력한 개발망 설정과
API 토큰을 포함한다. 컴파일된 설정이 해당 파일과 일치하고, KIT·WPA2·BLE SMP 및
공통 페어링 코드가 적용됐음을 값을 출력하지 않고 확인했다.

## NU87 실물 다운로드와 부팅

사용자가 맥미니에 연결한 CP2102N 장치를 USB 식별값으로 확인하고 다운로드 모드로
전환한 뒤 기록했다. 고정 해시로 검증한 Realtek 업로더를 사용했다.

- 부트로더: 28,928 B, 주소 `0x0`, SHA-256
  `74d86e9c144fe5018835de4dfb308c1350057d614fda016576099ea379a60d6e`.
- 앱: 위 최종 NU87 이미지, 주소 `0x14000`. 보정 저장 영역은 기록 범위 밖이다.
- 업로더 종료 코드 0과 `All images are sent successfully!`를 확인했다.
  전체 플래시를 다시 읽어 바이트 단위로 비교한 결과는 아니다.
- 실제 Zephyr `e70694102ad6` 부팅과 새 게이트웨이 시작을 확인했다.
  기존 `Efuse empty!` 시작 프롬프트를 관찰한 뒤 개행 한 번으로 진행했다.
  eFuse 데이터를 기록하지 않았다.
- 첫 Wi-Fi 접속은 `-4108`로 실패했고, 두 번째 접속은 vendor 결과 0과
  join status 9로 성공했다. 부팅 후 약 46.1초에 DHCP 주소가 출력됐다.
- 당시 맥미니와 NU87은 서로 다른 IPv4 네트워크에 있었고, 맥미니에서 인증 HTTP
  상태 요청은 시간 초과였다. 같은 개발망 연결 후 HTTP·BLE 경로를 추가 확인해야 한다.

비공개 로컬 기록은 `build/nu87-voice/flash.log`, `boot.log`에 저장했다.
이 NU87 검증 단계에서는 NU40의 USB 설치·OTA를 수행하지 않았다.

## NU40 실물 초기 설치와 부팅

이후 사용자가 연결한 NU40DK를 USB 식별값으로 확인했다. 당시 앱은 참고 저장소의
OLED 전용 버전과 달리 `firmware=3`, `version=0.1.4`, `transport=smp_ble`를 보고했다.
USB 부트로더로 전환해 `Board-ID: BARAM-NU40`, `SoftDevice: not found`,
부트로더 `60c185b`를 확인하고 기존 `CURRENT.UF2`와 `INFO_UF2.TXT`를 비공개로 백업했다.

기존 이미지에는 새 레이아웃의 primary trailer 페이지에 4,046바이트의 비어 있지 않은
데이터가 남아 있었다. Sparse UF2는 해당 페이지의 다른 바이트를 보존하므로, 초기 설치
생성기를 보완해 `0x1000–0xdffff` 전체를 명시적으로 기록하도록 했다. 검증된 MCUboot와
confirmed 앱 이외에는 이전 OTA 상태·secondary slot·NVS·BLE 페어링을 초기화한다.
MBR `0x0–0xfff`와 USB 복구 부트로더 `0xe0000+`는 기록 대상에서 제외한다.
이 초기화는 factory UF2에만 적용하며 이후 signed SMP OTA에는 적용하지 않는다.

- 3,568개 UF2 블록의 주소·번호·family ID·magic과 원본 HEX 일치를 독립적으로 검사했다.
  primary image_ok=1 및 MCUboot magic 보존, secondary/NVS의 erased 값도 확인했다.
- 최종 UF2 1,826,816바이트 전송을 완료하고 새 USB 제품명
  `military-wallaby NU40 voice`로 재등장한 것을 확인했다.
- 실제 부팅 로그: `role=military-wallaby firmware=2`, `oled_init=0`,
  PDM `ready=1 rate=16000 frame=176`, BTN4 `P1.05 init=0`, BLE 초기화·광고 결과 0.
- BLE 연결과 ATT MTU 247 협상도 관찰했으나, 그 연결의 중앙 장치가 NU87인지는
  이 USB 로그만으로 확인하지 않았다. 전체 플래시 readback 비교는 수행하지 않았다.

백업은 Git 제외 경로 `build/nu40-backups/`, 새 부팅 로그는
`build/nu40-device-validation/boot.log`에 저장했다. 실제 음질·스피커 가청 출력 및
NU87 경유 OTA 업로드·시험 부팅·확정·롤백은 아직 검증하지 않았다.

## BTN2/BTN3 후속 변경 (NU40 전용)

물리 버튼 기능은 NU40 앱에만 추가했다. NU87 펌웨어, BLE 오디오 프레임 형식 및
HostPC 실행 코드는 변경하지 않았다. 제조사 예제에서 BTN2=P1.07, BTN3=P1.06을
확인했다. 웹 마이크 시작은 수신 세션 준비이며, BTN2가 실제 PDM 녹음을 시작하고
BTN3가 녹음 종료와 전송을 요청한다. 최대 10초/500프레임/88KB를 RAM에 보관한다.
한도 도달만으로는 전송하지 않는다. 녹음 전송 전의 1초 간격 패킷은 순수 무음이다.

- NU40 테스트 5개와 공통 오디오·버튼 테스트 1개가 통과했다. 새 C 검사는 ASan/UBSan을
  사용하며 500프레임 한도, BTN3 전 출력 금지, 버퍼 삭제와 세션 예산을 확인한다.
  실제 `voice.c` 콜백·전송 작업을 포함한 검사에서 STOP/새 세션 뒤 대기 패킷 차단,
  알림·큐·작업 제출 실패와 경고·OTA·구독·연결 해제 처리를 확인했다.
- 기존 NU87 수신 콜백을 사용하는 ASan/UBSan 호환 검사에서 80초의 무음 연결 유지와
  500개 녹음 프레임, 40개 종료 무음 프레임을 수신했다. 총 620개, 누락 0개였다.
  64프레임 버퍼 초과와 STOP 뒤 추가 패킷 차단도 확인했다. 실물 무선 시험은 아니다.
- 실제 C ADPCM 인코더 출력 104프레임을 기존 Python 디코더·VAD로 처리했다.
  초기 무음 5프레임에서 발화가 생성되지 않았고, 녹음 전송 뒤 한 발화가 생성됐다.
- 이 ADPCM 경로의 합성 명령을 로컬 Whisper에 넣자 `왈랍이 앞으로 가`로 전사돼
  정확 문구 기반 이동 명령은 선택되지 않았다. 패킷 호환 확인을 실제 PDM 음질이나
  명령 인식 성공으로 해석하지 않는다. 실물 마이크 입력으로 인식률을 확인해야 한다.
- 기존 Host VAD는 녹음 내부에 700ms 침묵이 있으면 첫 구간을 처리하고 STOP을 보낸다.
  500프레임 시험에서는 135번째에 발화가 확정됐다. 따라서 이번 호환 모드는 짧은
  명령 한 문장용이며, 긴 녹음 전체의 일괄 처리를 보장하지 않는다.
- 연결·구독 상실, 경고, OTA, STOP 또는 새 세션은 보관 녹음을 폐기한다.
  Host 수신 화면의 5초 lease가 유지돼야 하며, 음성 응답이 끝난 뒤 다음 녹음을 시작한다.

NU40 `0.1.1+0` 앱과 MCUboot를 실제 sysbuild로 다시 빌드했으며 컴파일 경고 없이
서명 공개키 검증을 통과했다. 앱 FLASH 235,364 B, RAM 168,336 B, 서명 업데이트
파일 235,516 B다. OTA CLI도 이미지 구조와 해시를 별도로 확인했다.

- 업데이트 파일 SHA-256:
  `9ac1769d80d99de0d173f68daee76ebd4a99661c7f1b43df377f933a2e285c17`
- MCUboot 이미지 해시:
  `467a9f2ffa246d5eb6a3378932c89bcf53c8436beaf18572cced31b41cff4426`

앞선 NU40 실물 설치 결과는 이전 `0.1.0`에 해당한다. BTN2/BTN3 후속 변경은
NU87 경유 OTA 연결과 실제 버튼·마이크·스피커 시험이 완료되기 전까지 보드 검증으로
기록하지 않는다. 이전 서명 이미지와 초기 설치 파일은 Git 제외 경로
`build/nu40-voice-0.1.0/`에 보관했다.

## 남은 실물 시험

NU87 다운로드·Wi-Fi 부팅과 NU40 초기 설치·주변장치 초기화는 위와 같이 확인했다.
다음은 벤치 검증이 필요하다.

1. HostPC를 NU87과 같은 개발망에 연결해 인증 HTTP 및 NU87↔NU40 BLE 인증 연결 확인.
2. 키트 배선, PDM 음질·잡음, BLE 오디오 처리량과 프레임 손실.
3. BTN4를 눌렀을 때 실제 스피커 경고음, NU87 경고 상태와 화면의 정지·해제.
4. 서명 이미지 OTA 업로드 → 시험 부팅 → 기능 점검 → 명시적 확정.
5. 전원 차단·무선 단절·미확정 리셋에서 복구 및 MCUboot 롤백.

드라이버 ACK는 가청 출력 측정이 아니다. NU87의 기존 테스트 난수 생성기 등 개발 포트의
제한도 유지된다. 이 기록은 실물 음질·무선 안정성·제품 수준 보안을 검증했다고 뜻하지 않는다.

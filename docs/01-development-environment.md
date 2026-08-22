# 1. 개발환경과 설치 위치

## 사용 환경

- 컴퓨터: Apple Silicon M1 Mac
- 운영체제: macOS 14.6.1
- 프로젝트 루트: `/Users/usuzin/Documents/ChatGPT/mzsz-nucode`
- 보드 타깃: `nucode_nu40/nrf52840`
- 개발 방식: IDE 없이 터미널

## 시스템에 설치된 도구

Homebrew 프로그램은 `/opt/homebrew` 아래에 설치된다.

| 도구 | 용도 |
|---|---|
| CMake | Zephyr 빌드 파일 생성 |
| Ninja | 실제 컴파일 실행 |
| devicetree compiler | 보드 핀·장치 설정 처리 |
| ccache | 반복 컴파일 가속 |
| Python 3.12 | west와 Zephyr 스크립트 실행 |
| Node.js/npm | 로컬 웹앱 빌드와 실행 |

## 프로젝트 내부에 설치된 도구

전역 환경을 오염시키지 않도록 대부분을 프로젝트 안에 설치했다.

| 경로 | 내용 |
|---|---|
| `.venv/` | west 등 Python 가상환경 |
| `.zephyr-sdk/` | ARM Zephyr SDK와 GCC 툴체인 |
| `zephyr/` | 고정된 Zephyr 소스 |
| `modules/` | CMSIS와 Nordic HAL 등 최소 모듈 |
| `web/node_modules/` | 웹앱 패키지 |
| `build/` | Zephyr 빌드 결과 |
| `web/dist/` | 웹앱 최종 빌드 결과 |
| `backups/` | 플래시 직전 보드 펌웨어 백업 |

Zephyr 소스는 `.zephyr-revision`에 기록된 커밋으로 고정된다. 버전을 고정하면
나중에 같은 소스와 같은 설정으로 다시 빌드하기 쉽다.

## 주요 폴더

```text
mzsz-nucode/
├── src/main.c                         # 보드 애플리케이션
├── boards/*.overlay                   # 터치 ADC와 플래시 배치
├── prj.conf                           # Zephyr 기능 설정
├── scripts/                           # 설치·빌드·검증·플래시·모니터
├── web/                               # 가상 주택 웹앱
├── docs/                              # 이 학습 문서
├── build/zephyr/zephyr.uf2            # 보드에 넣는 펌웨어
└── backups/                           # 기존 펌웨어 백업
```

## 왜 일반 NU40-DK 빌드와 플래시 주소가 다른가

연결된 보드는 USB에서 다음과 같이 확인됐다.

- 볼륨: `NRF52BOOT`
- Board-ID: `nRF52840-pca10056-v1`
- UF2 Bootloader: 0.9.1
- SoftDevice: S140 6.1.1

이것은 보드 물리 모델이 PCA10056이라는 뜻이 아니라, 레거시 부트로더의 USB
호환 ID다. 이 부트로더는 애플리케이션을 `0x26000`부터 실행한다. 따라서
`boards/nucode_nu40_nrf52840.overlay`에서 Zephyr 코드 파티션을
`0x26000..0xED000`으로 제한했다. `scripts/verify-uf2.py`가 매번 결과 UF2가
이 범위를 벗어나지 않는지 검사한다.

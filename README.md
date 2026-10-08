# microWallaby

NU-87·Zephyr를 기반으로 만드는 작은 반려로봇입니다. 현재 얼굴 케이스와 I/O
bring-up, 하드웨어 제작 전 주행·도킹 시뮬레이션을 함께 관리합니다.

## 먼저 소프트웨어로 실행하기

```sh
python3 simulation/serve.py
```

Python 3 표준 라이브러리와 브라우저만 필요합니다. `http://127.0.0.1:8765/`에서
5축 모델의 수동 주행, 표현 관절, 자동 도킹, 센서 오류를 조작할 수 있습니다.
자세한 실행·MacBook 전달·테스트 방법과 모델 가정은
[시뮬레이터 안내](simulation/README.md)를 참고하세요.

## 저장소 구조

| 경로 | 역할 |
| --- | --- |
| `simulation/` | 독립 실행 가능한 운동학 모델·브라우저 UI·테스트·배포 ZIP 도구 |
| `firmware/nu87_io_bringup/` | NU-87 입력 장치·안전 상태 로직, portable C 테스트 |
| `firmware/nu87_web_nes_poc/` | 기존 Wi-Fi·웹·조이스틱 PoC |
| `tools/nu87_flash/` | macOS 플래시 도구 |
| `patches/` | 고정 Zephyr 소스에 적용하는 패치 |
| `docs/` | 설계, 시험 계획과 검증 기록 |

얼굴 케이스의 `hardware/wallaby-face/` 설계 작업은 현재 로컬에 별도로 보관되어
있으며 이번 시뮬레이터 커밋에는 포함하지 않습니다.

시뮬레이션은 **좌우 바퀴 + 머리 yaw/pitch + 허리 yaw**의 이동 확장안을 시험합니다.
2축 고정형 시장 검증 PoC와 구분하며, 기존 제품 범위를 자동으로 변경하지 않습니다.
5축 구동·충전 하드웨어가 완성되었거나 NU-87 펌웨어로 동작한다고 의미하지 않습니다.

## 개발과 검증

```sh
node --test simulation/model.test.mjs
make -C firmware/nu87_io_bringup/tests/host test
```

펌웨어 빌드·핀 설정은 [I/O bring-up README](firmware/nu87_io_bringup/README.md)를
따릅니다. `west.yml`에 고정된 Zephyr 환경과 시뮬레이터 실행 환경은 독립적입니다.
시뮬레이터에는 npm 의존성이 없고, GitHub Actions에서 모델 테스트와 전달용 ZIP 생성을 실행합니다.

기능 작업 브랜치는 `feat/`, 버그 수정은 `bugfix/`를 사용합니다.
소스·테스트·문서는 Git에서 관리하고 `simulation/dist/` 배포 파일과 Python 캐시는 제외합니다.

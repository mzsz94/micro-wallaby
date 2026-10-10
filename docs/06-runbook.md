# 6. 빌드·플래시·실행 절차

모든 명령은 프로젝트 루트에서 실행한다.

```bash
cd /Users/usuzin/Documents/ChatGPT/mzsz-nucode
```

## 최초 설치

```bash
make deps
make setup
make web-install
```

## 펌웨어 빌드

```bash
make build
```

결과는 `build/zephyr/zephyr.uf2`다. 빌드 스크립트가 nRF52840 family ID와
안전한 플래시 주소 범위를 자동 검증한다.

## 펌웨어 플래시

1. NU40-DK의 RESET을 빠르게 두 번 누른다.
2. Finder에 `NRF52BOOT`가 나타나는지 확인한다.
3. 다음 명령을 실행한다.

```bash
make flash
```

플래시 전에 `CURRENT.UF2`가 `backups/`에 복사된다. 기록이 끝나면 보드가
자동 재부팅하므로 macOS에서 디스크가 제대로 추출되지 않았다는 알림이나 `cp`
I/O 오류가 나올 수 있다. 이후 Zephyr USB 시리얼이 나타나면 정상이다.

## USB 센서 로그 확인

```bash
make monitor
```

종료는 `Ctrl+C`다. 이 명령은 BLE와 동시에 사용할 수 있다.

## 웹앱 실행

```bash
make web-dev
```

Chrome 또는 Edge에서 다음 주소를 연다.

```text
http://localhost:3000
```

1. 부모 조건을 입력하고 게임 초안을 만든 뒤 `승인하고 시작`을 누른다.
2. 마우스와 키보드로 선택 이동과 제출을 먼저 확인한다.
3. `Bluetooth 연결`을 누른다.
4. 장치 선택 창에서 `MZSZ-TOY`를 고른다.
5. 보드와 조이스틱을 약 1.2초 동안 움직이지 않아 중심 보정을 마친다.
6. IMU 좌우 기울기와 조이스틱 좌우로 선택 카드가 이동하는지 확인한다.
7. 터치 센서와 조이스틱 버튼으로 답을 제출한다.
8. 정답마다 보드 LED가 하나씩 켜지는지 확인한다.

웹 서버 종료는 서버를 실행한 터미널에서 `Ctrl+C`다.

## 웹 최종 빌드 검사

```bash
make web-build
```

코드 수정 후 펌웨어의 `make build`와 웹의 `make web-build`가 모두 성공해야
하나의 기능을 완료한 것으로 본다.

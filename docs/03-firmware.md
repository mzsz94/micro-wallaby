# 3. Zephyr 펌웨어 구조

## 설정 파일

`prj.conf`에서 다음 기능을 활성화한다.

```text
GPIO   터치 D0와 보드 LED
ADC    터치 A0 전압
I2C    LSM6DS3TR 통신
USB    터미널 디버깅
BLE    Web Bluetooth 연결
UF2    부트로더에 복사할 결과 생성
```

BLE 장치명은 `NU40 Home Remote`이고 한 번에 한 중앙 장치만 연결한다. 현재
로컬 실험 단계에서는 BLE 페어링과 암호화(`CONFIG_BT_SMP`)를 끈 상태다.

## 시작 순서

`src/main.c`의 `main()`은 다음 순서로 실행된다.

1. USB CDC 출력 준비를 위해 1초 대기
2. 터치 GPIO와 ADC 초기화
3. 보드 LED 네 개를 출력으로 초기화
4. I²C 버스 스캔과 IMU 종류 확인
5. LSM6DS3TR을 104 Hz, ±2g, ±245°/s로 설정
6. Zephyr Bluetooth 스택 초기화
7. `NU40 Home Remote` 이름과 커스텀 서비스 UUID 광고
8. 100ms마다 센서를 읽고 USB 출력 및 BLE 알림 전송

## 센서 단위 변환

LSM6DS3TR의 원시 16비트 값에 데이터시트 감도를 적용한다.

```text
가속도 ±2g:     raw × 0.061 mg/LSB
자이로 ±245dps: raw × 8.75 mdps/LSB
```

웹 BLE 패킷에서는 자이로를 `0.01°/s` 단위의 int16으로 줄여 18바이트 안에
넣는다. USB 터미널은 기존처럼 `mdps` 단위로 출력한다.

## 메인 반복문

샘플 주기는 100ms, 즉 10 Hz다. 매 샘플에서 다음을 수행한다.

1. P0.02 터치 디지털 상태 읽기
2. P0.03/AIN1 터치 아날로그 전압 읽기
3. IMU 가속도와 자이로 6축 읽기
4. BLE용 18바이트 패킷 생성
5. 웹이 알림을 구독 중이면 `bt_gatt_notify()` 호출
6. 다섯 샘플마다 USB 터미널에 한 줄 출력

USB 출력은 2 Hz로 줄여 로그가 너무 빠르게 흐르지 않게 했지만 BLE 데이터는
10 Hz 그대로 유지한다.

## 웹 명령 수신

웹은 방 상태가 바뀔 때 3바이트 명령을 보낸다.

```text
[0x01, room_index, state]
```

펌웨어의 `write_house_command()`가 길이와 범위를 검사한 뒤 `room_index`에
대응하는 LED를 켜거나 끈다. 잘못된 명령은 GATT 오류로 거부한다.

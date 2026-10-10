# NU87 Zephyr IMU

NU40 IMU 앱의 센서 드라이버·교정·명령·로그 형식을 NU87로 옮긴 별도 앱입니다.
NU40 앱과 루트 Zephyr 환경을 유지하며, NU87 의존성은 `build/nu87-workspace`에 둡니다.

## 연결

| IMU | NU87 |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SDA | PA26 |
| SCL | PA25 |

전원을 끈 상태에서 연결합니다. PA25/PA26의 native USB와 I2C를 동시에 사용하지 않습니다.
콘솔은 CP2102N USB 직렬 포트이며 **1,500,000 baud**입니다. I2C는 100 kHz이고
0x6a/0x6b 주소를 검사합니다. 센서는 WHO_AM_I 0x69/0x6a 호환 프로파일을 지원하며,
기존 NU40에서 확인한 모듈은 주소 0x6a, ID 0x6a입니다.

## 빌드

Python 3.12 이상, CMake, Ninja, Zephyr SDK 1.0.1 ARM 도구체인(`.zephyr-sdk`)이 필요합니다.

```sh
bash firmware/nu87_imu_bringup/setup.sh
bash firmware/nu87_imu_bringup/build.sh
make -C firmware/nu87_imu_bringup/tests test
```

이 앱은 [micro-wallaby f9e463f70d8a](https://github.com/mzsz94/micro-wallaby/tree/f9e463f70d8aa822a00ca637d459e9569002d580)의
NU87 지원과 패치를 사용합니다. Zephyr `e70694102ad6`, HAL `9a4caf9846d6` 및
CMSIS 커밋은 `west.yml`에 고정했습니다. 원본의 200 MHz clock, bounded I2C polling,
repeated START 패치와 Realtek 바이너리를 적용합니다. 별도 앱의 `ameba_log_compat.c`도
같은 원본에서 가져왔습니다.

추가 로컬 패치는 AmebaD의 RESTART 명령을 **직전 메시지의 마지막 바이트**에 붙입니다.
HAL `I2C_MasterRepeatRead()`와 같은 동작이며, WHO_AM_I 읽기는 명령 `0x40f`,
`0x300` 순서입니다. 원본 패치의 다음 READ 첫 바이트에 RESTART를 붙이는 오류를
수정하며 `setup.sh`와 `build.sh` 모두 이 패치를 적용합니다.
`python3 firmware/nu87_imu_bringup/tests/test_i2c_polling.py`는 실제 드라이버의
polling 함수를 host의 mock 레지스터로 실행해 명령 순서와 오류 처리를 검사합니다.

출력은 `build/nu87-imu/images/bootloader_all.bin`과 `km0_km4_app.bin`입니다.
부트로더는 flash 0x0, 결합 앱은 0x14000에 기록합니다. ROM 매핑 때문에
Zephyr의 링크 offset은 0입니다. NVS는 0x250000–0x255fff를 사용합니다.

## 플래싱과 확인

아래 포트는 실제 연결된 NU87 포트로 지정합니다. 첫 명령은 직렬 포트를 열지 않습니다.

```sh
python3 firmware/nu87_imu_bringup/flash.py --build-dir build/nu87-imu --port /dev/cu.usbserial-10
```

BOOT를 누른 상태로 RESET을 누르고 놓고, BOOT를 놓아 다운로드 모드로 진입합니다.

```sh
python3 firmware/nu87_imu_bringup/flash.py --build-dir build/nu87-imu --port /dev/cu.usbserial-10 --write
```

업로더의 성공 메시지와 종료 코드 확인 후 BOOT를 놓은 상태로 RESET합니다.
`NU87 IMU v1.2`, `probe ... init=0`, `IMU ready`와 `sample=...` 로그를 확인합니다.
실제 성공 여부는 연결된 하드웨어 로그로 판단해야 합니다.

`imu status`, `imu retry`, `imu stream on/off`, `imu calibrate`, `imu save`, `imu clear`를
구현했습니다. 다만 현재 v1.2의 실제 NU87에서는 KM0 monitor와 UART 입력이 충돌해
이 셸 명령이 실행되지 않는 것을 확인했습니다. 실제 보정은 맥북 브리지에서 적용하며
`web/IMU-DASHBOARD.md`와 `HARDWARE-RESULT.md`에 방법과 검증값을 기록했습니다.
취득 목표는 100 Hz, 센서 ODR 104 Hz,
샘플 로그는 10 Hz, health 로그는 10초 간격입니다. NU40와 동일한
`sample=`, `accel_g=`, `gyro_dps=`, `corrected_dps=`, `health:` 형식을 유지합니다.
저지연 로그 설정은 trigger threshold 1, process sleep 20 ms입니다.

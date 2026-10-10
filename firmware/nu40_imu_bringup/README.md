# NU40-DK IMU bring-up

NU40-DK의 두 UF2 부트로더 배치를 지원하는 독립 앱입니다.
기존 루트의 BLE 놀이 앱과 NU-87 펌웨어를 수정하지 않습니다.
`INFO_UF2.TXT`에 맞는 이미지를 선택해야 합니다. 두 이미지는 서로 바꿔 쓸 수 없습니다.

| 빌드 옵션 | 부트로더 | 앱 시작 | 이미지 디렉터리 |
| --- | --- | --- | --- |
| `baram` (기본) | BARAM-NU40 / SoftDevice 없음 | `0x1000` | `build/nu40-imu/` |
| `legacy` | NRF52BOOT / PCA10056 / S140 6.1.1 | `0x26000` | `build/nu40-imu-legacy/` |

## 연결

전원을 끈 상태에서 NU-IMU의 VCC→3.3V, GND→GND, SDA→P0.26,
SCL→P0.27을 연결합니다. I2C는 100 kHz입니다.
0x6a와 0x6b 주소를 확인하고 실제 WHO_AM_I가 0x69 또는 0x6a일 때만
LSM6DS3 / LSM6DS3TR-C 호환 레지스터 설정을 적용합니다. ID만으로 센서의
정확한 상품명까지 확정하지 않으며, 다른 ID에는 설정을 기록하지 않습니다.

104 Hz 센서 설정, 100 Hz 읽기 목표, 가속도 ±2g, 자이로 FS0를 사용합니다.
가속도 단위는 g, 자이로는 degree/s이며 축은 센서의 원래 축입니다.
통신 실패와 실제 ID를 구분해 기록하며 초기화 실패는 2초마다 재시도합니다.
설정 후 레지스터를 다시 읽어 확인하고 새로운 6축 데이터만 집계합니다.

## 빌드와 플래싱

저장소 루트에서 실행합니다.

```sh
bash firmware/nu40_imu_bringup/build.sh
make -C firmware/nu40_imu_bringup/tests test
```

RESET을 빠르게 두 번 눌러 `BARAM-NU40` 볼륨을 연 뒤:

```sh
python3 firmware/nu40_imu_bringup/flash.py --image build/nu40-imu/zephyr/zephyr.uf2
python3 firmware/nu40_imu_bringup/flash.py --image build/nu40-imu/zephyr/zephyr.uf2 --write
```

첫 명령은 사전 검사입니다. 기록 시 기존 `CURRENT.UF2`와 부트로더 정보를
`backups/nu40-imu-날짜시각/`에 먼저 보관합니다.
앱은 `0x1000..0xDC000`, 교정 NVS는 `0xDC000..0xE0000`이며 UF2 검증기가
앱 범위 밖 기록을 거부합니다. 부트로더는 `0xE0000` 이상에 보존됩니다.

`NRF52BOOT` / S140 6.1.1 보드는 아래 명령을 사용합니다.

```sh
bash firmware/nu40_imu_bringup/build.sh legacy
python3 firmware/nu40_imu_bringup/flash.py --layout legacy --image build/nu40-imu-legacy/zephyr/zephyr.uf2
python3 firmware/nu40_imu_bringup/flash.py --layout legacy --image build/nu40-imu-legacy/zephyr/zephyr.uf2 --write
```

레거시 앱은 `0x26000..0xDC000`에 한정하며 MBR/SoftDevice와 기존 NVS 영역을
보존합니다. 플래셔가 보드 ID·SoftDevice 버전·벡터·모든 UF2 블록 주소를 검사합니다.
교체 보드의 `CURRENT.UF2`에서 앱 벡터 `0x26000`을 확인했으며,
`0xDC000..0xE0000`은 비어 있었습니다.
레거시 빌드는 내부 RC 저속 클록을 사용하여 외부 32.768 kHz 크리스털이
없는 보드에서도 USB 초기화 전에 클록 안정화를 기다리며 멈추지 않도록 합니다.

## 로그와 명령

```sh
.venv/bin/python -m serial.tools.miniterm /dev/cu.usbmodem101 115200 --raw
```

포트 번호는 달라질 수 있습니다. 종료는 Ctrl+]입니다.

- `imu status`: ID, 주소, 실패 단계, SDA/SCL 입력 상태, 신선한 샘플·오류 수와 교정 상태
- `imu stream off` / `imu stream on`: 10 Hz 샘플 로그 중지·재개
- `imu retry`: 센서 검색과 초기화를 즉시 다시 실행
- `imu calibrate`: 정지 상태 1,000개 샘플로 자이로 바이어스 교정
- `imu save`: 성공한 바이어스를 NVS에 저장
- `imu clear`: 저장값과 RAM 바이어스 삭제

교정은 모듈을 탁자에 가만히 놓고 실행하며, 가속도 중력값은 제거하지 않습니다.
샘플 누락·통신 오류·움직임·15초 시간 초과는 교정을 취소합니다.
센서 변경 시 재교정해야 합니다. 모듈 고유번호나 온도 기반 보정은 없습니다.
같은 ID·주소로 통신을 복구하면 저장 전 RAM 교정도 유지합니다.
재부팅 시에는 저장된 교정값을 불러옵니다. 같은 ID·주소를 쓰는 다른 센서로
바꾼 경우 자동으로 구분할 수 없으므로 직접 재교정해야 합니다.

## 출처

교정 및 NVS 코드의 기반은 Apache-2.0인
[micro-wallaby NU87 IMU](https://github.com/mzsz94/micro-wallaby/tree/f9e463f70d8aa822a00ca637d459e9569002d580/firmware/nu87_imu_bringup)입니다.
이 앱은 nRF52840의 Zephyr I2C 드라이버를 사용하며 AmebaD 패치를 사용하지 않습니다.
센서 레지스터·감도는 ST의 [LSM6DS3](https://github.com/STMicroelectronics/lsm6ds3-pid)와
[LSM6DS3TR-C](https://github.com/STMicroelectronics/lsm6ds3tr-c-pid) 드라이버를 참조했습니다.

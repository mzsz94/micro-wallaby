# 2026-10-10 NU-87 기록 결과

**최종 상태: Zephyr IMU v1.2 기록·부팅·센서 실측·대시보드 수신 확인 완료.**
주소 0x6a, WHO_AM_I 0x6a, LSM6DS3TR-C 프로파일이 정상 초기화되었습니다.
30.011초간 fresh 카운터가 3,278에서 6,078로 증가했으며, 오류와 late 카운터는
모두 0이었습니다. 실제 취득은 약 93.3 Hz, 로그 수신 간격 중앙값은 100 ms였습니다.
이 구간의 가장 긴 로그 간격은 209 ms였습니다. 실제 움직임에 따른 가속도·각속도
변화와 웹의 자세·차트 갱신도 확인했습니다. 이후 사용자 요청으로 맥북 측 보정을
추가했습니다. 보드 NVS에 교정값을 저장하지는 않았습니다.

## 수정 전 v1.1 확인

- 대상 USB: CP2102N `10c4:ea60`, `/dev/cu.usbserial-10`
- 다운로드 전 115200 baud에서 ROM NAK `0x15` 연속 수신
- Zephyr 빌드: `nucode_nu87/rtl872xd`, UART 1500000 baud
- I2C: SDA PA26, SCL PA25, 100 kHz
- boot: 28,928 bytes, flash 0x0
- app: 166,464 bytes, flash 0x14000–0x3ca3f
- app SHA-256: `dfb6d6c914598baef7e2d8bb4dce17d778a42fb35c5b0f01fdc5ad368d55b919`
- 4 KiB 단위 기록용 삭제 끝 0x3d000; 앱 파티션 끝 0x200000, NVS 시작 0x250000
- Realtek 공식 uploader가 `All images are sent successfully!` 출력 후 종료 코드 0
- 사용자 재기록 요청으로 같은 이미지 재전송: 성공 메시지 및 종료 코드 0 재확인

사용자가 BOOT를 놓고 RESET을 한 번 누른 뒤 새 앱의 부팅을 확인했습니다.
UART에서 `NU87 IMU v1.1 (Oct 10 2026 14:11:31)` 배너와 주기적인
health 로그를 받았습니다. 브리지의 USB 연결 상태는 `connected`입니다.

수정 전 센서는 연결되지 않았습니다. 주소 0x6a의 WHO_AM_I 읽기는 -116(timeout),
0x6b는 -5(I/O error)로 실패하며 `ready=0`, `fresh=0`이었습니다.
GPIO 입력 레지스터는 SDA=0/SCL=0이지만, 주변장치 핀의 입력 경로 설정도
영향을 줄 수 있으므로 이 값만으로 물리적 단락을 단정하지 않습니다.
이 상태에서 I2C 명령 순서를 조사했습니다.

Realtek HAL의 repeated START가 현재 바이트 뒤에 적용되는 규약과 Zephyr
polling 패치의 명령 배치가 다름을 발견했습니다.

## I2C 수정본 v1.2

- RESTART를 이전 메시지의 마지막 바이트에 부착하도록 수정했습니다.
- 실제 polling 함수를 host 레지스터 모델로 실행하는 회귀검사 11개,
  기존 IMU 검사 9개와 플래셔 검사 10개가 통과했습니다.
- 이전 잘못된 명령 순서가 새 회귀검사에서 실패함도 확인했습니다.
- 새 앱: 166,432 bytes, SHA-256
  `8c5d049b9fa6838de5f4c83d2fba71b1d80a60554bab5adf5adf62c1b205f6df`.
- 부트로더는 변경되지 않았습니다. 삭제 끝 0x3d000, NVS 시작 0x250000.
- BOOT + RESET 뒤 ROM NAK를 확인하고 새 이미지를 기록했습니다.
- 공식 uploader 성공 메시지와 종료 코드 0을 확인했습니다.
- RESET 후 `NU87 IMU v1.2 (Oct 10 2026 14:26:18)` 배너를 확인했습니다.
- `probe addr=0x6a WHO_AM_I=0x6a profile=LSM6DS3TR-C init=0 stage=ready` 수신.
- `ready=1`, `last_error=0`, 실측 데이터 연속 수신을 확인했습니다.
- 저장된 교정값은 없으며 `bias_valid=0`입니다. 교정·저장을 수행하지 않았습니다.
- 정상 I2C 동작 중에도 raw GPIO 입력이 0/0으로 읽히므로 NU87의 웹 핀 상태는
  물리적인 LOW/HIGH 대신 `확인 불가`로 표시합니다. 원본 로그는 보존합니다.
- 브리지 중지·재시작 후 웹이 새로고침 없이 자동으로 수신을 재개했습니다.

NU87용 USB 브리지와 웹 대시보드는 재부팅 후 수신을 위해 실행했습니다.
주소: http://localhost:5178/imu

빌드/전송 로그: `build/nu87-build.log`, `build/nu87-imu/BUILD.json`,
`build/nu87-imu/flash-20261010.log`, `build/nu87-imu/hardware-20261010.log`.
재기록 전체 출력: `build/nu87-imu/reflash-20261010.log`.
RESET 후 상태: `build/nu87-imu/reflash-status.json`.
v1.2 기록 로그: `build/nu87-imu/flash-v1.2-20261010.log`.
v1.2 빌드 정보: `build/nu87-imu/BUILD.json`.
수정 전 빌드 정보: `build/nu87-imu/BUILD-before-i2c-fix.json`.
30초 연속 실측 검증: `build/nu87-imu/verified-v1.2-status.json`.

## USB-C 위 기준 재보정

사용자가 이전 측정 중 움직였다고 알려온 뒤의 새 데이터만 사용했습니다.
20초의 198개 출력 샘플을 10구간으로 나누고, 각 구간의 자이로 평균들의 중앙값을
고정 바이어스로 정했습니다. 별도 20초의 198개 실측으로 검증했습니다.

- 자이로 오프셋 X/Y/Z: `[0.9839184211, -4.3115925, -1.5356475]` °/s.
- 독립 검증의 보정 후 평균: `[-0.014545, 0.128144, -0.016265]` °/s.
- 독립 검증의 표준편차: `[0.188632, 1.803509, 0.218738]` °/s.
  순간 진동을 제거하거나 측정값을 0으로 덮어쓰지 않습니다.
- 기준 중력: `[-0.7229257576, -0.0906287879, 0.7499919192]` g.
- 처음에는 이 자세를 상대 기울기 0°로 사용했습니다. 이후 아래 바닥 기준을 추가해
  실제 보드 자세로 수정했습니다. 절대 방위각, 가속도 6면 교정, 중력 제거는 수행하지
  않았습니다.
- 보정 파일: `build/nu87-imu/calibration-usb-up.json`.
- 원본/검증 기록: `host-calibration-fresh-samples.json`,
  `host-calibration-verification-samples.json` (모두 `build/nu87-imu` 아래).
- 실행 중 브리지의 `corrected = raw gyro - saved bias`를 확인했고 원본 값은 유지합니다.
  `calibrated-dashboard-status.json`에 실행 결과를 보관했습니다.

현재 KM0 monitor가 UART 입력을 소비하는 정황으로 Zephyr 셸의 보정·저장 명령이
실행되지 않았습니다. CR/LF 및 저속 문자 전송 모두 응답을 얻지 못했습니다.
따라서 보정은 **동일 USB 식별번호의 NU87에 한해 맥북에서 적용**합니다.
보드의 `bias_valid` 상태는 변경하지 않았으며 웹에는 `맥북 보정 적용`을 표시합니다.
지원되지 않는 NU87 웹 명령은 비활성화했습니다.

## 바닥 자세 동기화 수정

사용자가 보드를 바닥에 눕힌 뒤 새로 받은 5초의 50개 샘플로 수평 기준을 측정했습니다.
평균 중력은 `[0.10801, 0.05232, 1.041732]` g, 축별 표준편차는
`[0.002055, 0.002390, 0.002747]` g였습니다. 기록은
`build/nu87-imu/flat-pose-observation.json`에 있습니다.

이 중력을 보드의 법선으로 사용하고, 이전 USB-C 위 측정의 바닥면 투영을 USB 방향으로
사용합니다. 두 측정의 실제 각도는 약 50.43°이며 90°로 강제하지 않습니다. 보드 회전과
카메라 시점을 분리해 수평 보드가 바닥 격자와 평행하게 표시되도록 수정했습니다.
`calibration-usb-up.json`에 `flatGravity`를 추가했으며 기존 자이로 오프셋은 유지했습니다.
이전 설정은 `calibration-before-flat-pose.json`에 보관했습니다.

적용 후 실측은 `connected`, `ready=true`, 약 93 Hz, 통신 오류 0회였으며 기존
자이로 오프셋이 한 번만 적용됨을 확인했습니다. 확인 시점의 스냅샷은
`flat-pose-dashboard-status.json`에 있습니다. 브라우저에서 바닥 기준 표시와
누운 보드 모형, 새로운 수신값에 따른 기울기 갱신을 확인했습니다. Python 26개와
렌더링 14개 테스트, IMU 화면 ESLint 및 TypeScript 검사를 통과했습니다.

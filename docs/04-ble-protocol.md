# 4. BLE GATT 프로토콜

## 장치와 UUID

- 광고 이름: `NU40 Home Remote`
- 서비스: `7b1e0001-21a8-4f8c-bb2d-4c7d2156f001`
- 센서 특성: `7b1e0002-21a8-4f8c-bb2d-4c7d2156f001`
- 명령 특성: `7b1e0003-21a8-4f8c-bb2d-4c7d2156f001`

센서 특성은 `Read + Notify`, 명령 특성은 `Write + Write Without Response`다.

## 센서 패킷

BLE 기본 ATT 데이터 20바이트 안에 들어가도록 18바이트, little-endian으로
설계했다.

| 오프셋 | 크기 | 형식 | 의미 |
|---:|---:|---|---|
| 0 | 1 | uint8 | 프로토콜 버전, 현재 1 |
| 1 | 1 | bit flags | bit0 = 터치 D0 |
| 2 | 2 | uint16 LE | 패킷 순번 |
| 4 | 2 | uint16 LE | 터치 전압 mV |
| 6 | 2 | int16 LE | AX, mg |
| 8 | 2 | int16 LE | AY, mg |
| 10 | 2 | int16 LE | AZ, mg |
| 12 | 2 | int16 LE | GX, 0.01°/s |
| 14 | 2 | int16 LE | GY, 0.01°/s |
| 16 | 2 | int16 LE | GZ, 0.01°/s |

웹에서는 `DataView.getUint16(offset, true)`와
`DataView.getInt16(offset, true)`로 해석한다. 마지막 `true`는 little-endian을
뜻한다.

## 명령 패킷

| 오프셋 | 값 | 의미 |
|---:|---|---|
| 0 | `0x01` | 방 장치 상태 설정 명령 |
| 1 | 0~3 | 거실, 주방, 침실, 현관 |
| 2 | 0 또는 1 | 끄기 또는 켜기 |

## 연결 흐름

```text
웹에서 연결 버튼 클릭
  → navigator.bluetooth.requestDevice()
  → NU40 Home Remote 선택
  → GATT 서비스와 두 특성 검색
  → 센서 특성 startNotifications()
  → 10 Hz 패킷 수신
  → 마우스/터치 상태 변경 시 명령 특성 write
```

브라우저 보안 정책 때문에 장치 선택 창은 반드시 사용자의 클릭 이벤트 안에서
열어야 한다. 새로고침하거나 보드가 재부팅되면 다시 연결해야 한다.

## 현재 보안 수준

이 프로토콜은 빠른 로컬 시제품을 위해 인증과 암호화를 사용하지 않는다.
누구든 가까운 거리에서 서비스 UUID를 알면 연결을 시도할 수 있으므로 실제 집
제어에는 그대로 사용하면 안 된다.

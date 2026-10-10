# NU87 Wi-Fi 게이트웨이 실기기 확인

## Published evidence and upstream scope

The operator confirmed the improvement on 2026-10-10. The English report is published in
[micro-wallaby issue #8, hardware-validation comment](https://github.com/mzsz94/micro-wallaby/issues/8#issuecomment-6096231052).
It includes pinned revisions, staged scan/connection results, image and source hashes,
sanitized diagnostics, test coverage, and remaining limitations. This branch,
`bugfix/zephyr-ble`, contains the application-local compatibility code and tests.

[Zephyr PR #121773](https://github.com/zephyrproject-rtos/zephyr/pull/121773) addresses the
separate timer-wrapper defect. Its exact upstream port passed 10 QEMU test cases across
two configurations but has not been flashed onto this board. The gateway used an
application-local timer workaround. The HAL coexistence and HCI IRQ changes are not in
that PR, and no `hal_realtek` coexistence PR has been submitted.

The results below are from one board pair and one session, not a controlled RF benchmark
or repeated A/B study. The newest result is a successful NU87-to-NU40 text-command ACK;
older sections retain the preceding failures as historical observations. Device and
network identifiers are redacted. NU40 firmware was unchanged and its KIT app has no OTA.

## 최신 결과: NU87↔NU40 연결 및 텍스트 ACK 성공

2026-10-10, 일반 검색에 이어 연결 시도(initiating)의 공존 보고를 보완한 앱을 기록했습니다.
앱 640,864바이트 / `0x14000`, SHA-256
`c6dbdffc0dea5cc64fea4c1e3e8ee28064c934f9416c04a59b2d9c73450cf7f9`.
부트로더는 28,928바이트 / `0x0`, 기존과 같은 SHA-256
`74d86e9c144fe5018835de4dfb308c1350057d614fda016576099ea379a60d6e`입니다.
업로더 종료 코드 0과 성공 메시지, 새 진단 필드 실행을 확인했습니다. 전체 플래시 읽기 비교는 미실행입니다.

- 부팅 관찰 중 BLE 초기화 전에 한 차례 재시작됐습니다. 원인은 이 로그로 확정할 수 없습니다.
  다음 부팅에서는 Wi-Fi 두 번째 접속 시도가 성공하고 약 47.3초에 DHCP가 완료됐습니다.
- NU87 주소 `<NU87-DHCP-IP>`. 인증 HTTP 3회 200(105 / 80 / 39ms), 비인증 401.
  main 스택 여유 6,328바이트 / 12KiB, Bluetooth 초기화 전/후 힙 34,552 / 26,992바이트.
- 실제 광고로 식별한 `NU40-KIT-DISPLAY`를 선택했고, 첫 연결 시도가 성공했습니다.
  `create=[1,0]`, `connection=[1,0]`, 취소 0회. 기존 연결 생성 제한 3초는 유지했습니다.
- GATT 서비스 검색과 ACK 알림 구독 완료, `connected=true`, `lcdReady=true`, 오류 0.
- 맥북 로컬 웹 API → NU87 Wi-Fi → NU40 BLE 경로로 아래 문구를 한 번 전송했습니다.
  `/api/display` HTTP 200, 729ms, `ready=true`를 받았습니다. 이 경로는 일치하는
  텍스트 명령의 성공 ACK를 받은 뒤에만 성공을 반환합니다. 육안 픽셀 검사는 별개입니다.

```text
NU87 -> NU40
Wi-Fi + BLE
Link test passed
Hello, NU40!
```

- 연결 후 initiating/setup 상태 모두 false로 종료됐으며, link/native_link_seen은 true입니다.
  UART 광고 완료 프레임·HCI 광고 수·앱 광고 수는 모두 6,834개. UART·프레이밍·읽기 오류 0.
- 시리얼 모니터를 정상 종료한 뒤에도 인증 HTTP 200과 Wi-Fi·BLE·화면 제어 준비 상태가 유지됐습니다.
  웹 화면에서 `화면 제어 준비됨`과 `화면 반영 응답을 받았습니다.` 표시를 확인했습니다.
- 연결 후 `No command sent for cmd complete 0xfc8f` 경고 1회가 있었습니다.
  vendor 공존 mailbox가 Wi-Fi 펌웨어 경로로 전송되어 Zephyr 명령 추적을 거치지 않는
  경로와 부합합니다. 해당 처리 자체는 연결을 해제하지 않으며 실제 ACK와 후속 연결 상태는
  정상입니다. 반복 경고와 명령 시간 초과/링크 해제가 동반되는지는 향후 관찰 대상입니다.
- 빌드, Python 45개 검사, 프로토콜 13개 검사군, BLE 어댑터·UART·공존 경합의
  ASan/UBSan 검사를 통과했습니다.

로그와 JSON은 Git 제외 경로 `build/nu87-lcd-gateway/`의 `flash-kit-final.log`,
`boot-kit-final.log`, `http-kit-verification.json`, `kit-live-verification.json`,
`scan-mode-observations.jsonl`에 있습니다. 제어 서버는 현재 주소를 사용하며
`127.0.0.1:8766`에만 바인딩합니다. NU40 펌웨어는 변경하지 않았고 현재 KIT 앱의 OTA는 미지원입니다.
타이머 수정의 upstream 제출 상태와 HAL 공존 보완의 범위는 문서 상단에 구분했습니다.

## 과거 기록: 일반 검색 공존 보고 보완 후 광고 수신 개선

검색 방식 비교 이미지의 후속 HTTP 조회에서 광고 이벤트 2개를 확인했습니다.
둘 다 NU40 식별 조건에 맞지 않았습니다. 따라서 수신이 완전히 차단됐다고
단정할 수 없으며, 기존 HCI 계수는 vendor 이벤트 버퍼를 통과한 뒤의 수치입니다.

제조사 구현과 비교해 누락된 호스트 검색 상태 전달을 앱 내부에서 보완했고,
버퍼 이전 UART 광고 프레임·라인 오류 계수를 추가했습니다. 상세 동작과 제한은
[KIT 모드 문서](KIT-DISPLAY.md)를 참고하세요.

- 기록한 앱: 639,808바이트, SHA-256
  `a001caa8be47d601f123a2d274d182e400c4d5412974d8f130901479e7bc87b5`.
- 빌드 성공, Python 39개 검사, 프로토콜 13개 검사군, 실제 BLE 어댑터와 UART
  진단의 ASan/UBSan 검사 통과. 공존 보고와 native 연결 이벤트의 경쟁 조건도 검사했습니다.
- 최종 ELF에서 HCI TX/RX, UART 읽기, UART 라인 상태 관측 경로의 연결을 확인했습니다.
- 업로더 종료 코드 0과 성공 메시지, 새 진단 API 실행을 확인했습니다.
  전체 플래시 읽기 비교는 수행하지 않았습니다.

실기기 결과:

- Wi-Fi 접속은 -4108, -4113 이후 세 번째 시도에서 성공했습니다. DHCP
  `<NU87-DHCP-IP>`, 부팅 후 약 83.6초에 준비됐습니다.
- 인증 HTTP 3회 200(131 / 173 / 39ms), 비인증 401, 시리얼 종료 후에도 200.
  main 스택 여유 6,328바이트 / 12KiB, Bluetooth 초기화 전/후 힙 34,548 / 26,988바이트.
- 광고 이벤트가 첫 조회 4,094개에서 103.01초 뒤 16,523개로 증가했습니다.
  이 구간은 12,429개, 평균 약 120.66개/초이며 NU40 외 주변 광고도 포함합니다.
  두 조회 모두 UART 완료 광고 프레임, HCI 전달 수, 앱 수신 수가 정확히 일치했습니다.
  UART 오류·잘못된 프레임·읽기 오류·검색 오류는 0이었습니다.
- 성공한 HCI 상태 반영 4회, 공존 보고 배치 4회 / vendor 보고 9회, 거부 0.
- NU87에서 `NU40-KIT-DISPLAY` 이름과 서비스, 연결 가능한 random 주소를 확인했습니다.
  RSSI 약 -50~-52dBm. Mac의 별도 20초 검색에서도 NU40 광고 29개를 확인했습니다.
- 웹에서 실제 후보를 선택해 연결을 요청했으나, 3초 연결 생성 제한에 걸려 실패했습니다.
  NU87↔NU40 연결 및 OLED ACK는 아직 확인되지 않았습니다. 제조사 구현의
  별도 연결 시도(initiating) 상태 전달을 다음 확인 대상으로 삼았습니다.

원본 이미지와 자료는 Git 제외 경로 `build/nu87-lcd-gateway/coex-scan-first/`에 보관했습니다.
로컬 제어 서버도 새 주소를 사용하도록 갱신했고 `127.0.0.1:8766`에만 바인딩합니다.

## 과거 기록: 공존 보고 보완 전 검색 방식 비교

2026-10-10, 앱 636,096바이트를 `0x14000`에 기록했습니다.
SHA-256: `f2fef689660980e3c6b5e8e74a19e607992de8a8b1c03b0b915570671e21c59d`.
부트로더는 아래와 같은 28,928바이트 이미지입니다. 업로더 성공 메시지와 이후
새 진단 기능 실행을 확인했으며, 전체 플래시 읽기 비교는 수행하지 않았습니다.

- Wi-Fi 첫 접속은 AP 검색 실패(-4108), 다음 접속은 성공(0 / join status 9).
- DHCP `<NU87-DHCP-IP>`, 약 54.2초에 준비. Bluetooth 초기화 성공 후 검색 시작.
- 인증 HTTP 3회 200(78 / 39 / 39ms), 비인증 401, 시리얼 종료 후에도 200.
- main 스택 여유 6,296바이트 / 총 12KiB. Bluetooth 초기화 전/후 힙 여유 34,548 / 26,992바이트.
- 실제 HCI 이벤트 마스크의 LE Meta와 Advertising Report 비트 모두 1.
  검색 간격/창은 96/48(각 60/30ms), 필터 정책 0, 중복 제거 0, 검색 활성 1.
  마스크·검색 파라미터·검색 활성 명령의 컨트롤러 완료 상태는 모두 0.
- 능동→수동→능동 전환이 실제 적용됐으며 generation 1→2→3, 검색 오류 0.
  수동 검색을 41초 이상, 복원한 능동 검색을 35초 이상 관찰했지만 LE/광고 이벤트는 모두 0.
  전체 HCI 응답은 71→75→81개로 증가했습니다.
- RF 보정 종류 1(BT_LOK), 결과 0. 공존 enabled=1, rfe/ant_div/bt_pos/ant_type=0,
  TDMA en/slotN/wlslot=0/0/0. 이 값만으로 안테나나 공존 설정의 오류를 판정하지 않습니다.
- 같은 수동 검색 구간에 Mac은 `NU40-KIT-DISPLAY`를 RSSI -56dBm으로 발견했습니다.
  NU87의 서비스·이름 필터 이전에 광고 이벤트가 전달되지 않는 상태입니다.

**이 단계에서는 NU87↔NU40 연결과 텍스트 ACK가 확인되지 않았습니다. OTA도 미검증이었습니다.**
컨트롤러/드라이버/RF 공존 경로의 추가 원인 확인이 필요합니다. 제조사 공개 자료에서는
내부 공유 안테나 구조만 확인됐으며 BTS0/BTS1 매핑은 확인되지 않아 설정을 변경하지 않았습니다.
기본 능동 검색으로 복원했고, 웹 화면은 현재 주소를 사용해 `Wi-Fi 연결됨 · NU40 대기`를 표시합니다.

사용자의 광고 미표시 제보 후 Mac에서 중복 수신을 켠 20초 직접 검색을 다시 수행했습니다.
캐시의 장치 이름 대신 실제 광고의 이름·서비스를 확인했고, `NU40-KIT-DISPLAY` 광고
32건(연결 가능)을 받았습니다. 이 검사는 연결이나 화면 쓰기를 수행하지 않았습니다.
웹 검색은 `/api/peers` → NU87 `/v1/peers` 경로이므로 Mac 직접 검색과 별개입니다.
혼동을 줄이도록 버튼을 `NU87에서 보드 검색`으로 바꾸고 검색 주체를 표시했습니다.
직접 검색 기록은 `nu40-fresh-scan.log`입니다.

부팅 로그의 주소 문자열에 vendor 로그가 끼어 자동 주소 추출이 실패해, 완성한 주소로
인증 HTTP를 직접 검증한 뒤 연결 상태 파일을 갱신했습니다. 업로더는 정상 종료했으며
이후 시리얼 관찰 프로세스만 명시적으로 종료했습니다. 실측 자료는 Git에서 제외된
`flash-kit-final.log`, `boot-kit-final.log`, `http-kit-mode-verification.json`,
`scan-mode-observations.jsonl`에 있습니다.

빌드 및 프로토콜 13개 검사군, 실제 BLE 어댑터 ASan/UBSan 검사, Python 34개 검사를 통과했습니다.

## 과거 기록: KIT 호환 초기 작업과 수신 진단

NU40 저장소의 프로토콜을 반영한 KIT 모드를 구현했습니다. 첫 실기기 기록에서
Realtek HCI UART IRQ 우선순위 오류로 부팅이 멈췄고, 해당 등록만 Zephyr 범위로
변환한 뒤 Bluetooth 컨트롤러 초기화 결과 0과 광고 검색 시작을 확인했습니다.
그러나 이 이미지에서는 Wi-Fi 접속이 연속 10회 실패했습니다. BLE 검색이 Wi-Fi
접속보다 먼저 시작된 점을 확인해 Wi-Fi IPv4 준비 후 검색하도록 수정했지만,
검색을 시작하지 않은 상태에서도 접속이 7회 실패했습니다. 이후 Bluetooth 초기화
자체를 Wi-Fi IPv4 준비 이후로 옮기고 실제 힙 사용량 및 vendor 오류 진단을 추가했습니다.
이 수정본에서는 Wi-Fi와 Bluetooth 초기화가 모두 성공했습니다.

기록한 앱: 631,680바이트, SHA-256
`24ee00eca164613fc73115c54422d73cc48f8dbdcf9cc2cbb04ffff145b9f631`.

- 첫 접속은 vendor `-4108`(대상 AP 검색 실패), 두 번째 접속은 결과 0 / join status 9.
- DHCP 주소 `<NU87-DHCP-IP>`, 부팅 후 약 47.2초에 준비.
- 이후 Bluetooth 초기화 결과 0, BLE 검색 시작.
- Bluetooth 초기화 전/후 실제 힙 여유 34,548 / 26,988바이트.
- 인증 HTTP 3회 200 (90 / 83 / 37ms), 비인증 HTTP 401.
- 시리얼 포트를 닫은 뒤에도 HTTP 200. 첫 HTTP 처리 후 main 스택 여유 7,776바이트.
- Mac에서는 NU40-KIT-DISPLAY 광고가 RSSI -52dBm으로 보이지만 NU87 후보는 비어 있습니다.
  두 보드를 20~30cm 가까이 둔 뒤에도 동일했습니다. BLE 수신 경로 진단을 추가 중입니다.

이후 원시 HCI 수신 진단 이미지(633,760바이트,
`642246cee5ae100ab8d06c8d1dd7faa29a124d6373350e486b3a8e8c3db211b2`)에서도
Wi-Fi/HTTP는 정상이며, 컨트롤러 응답 71개 중 LE 이벤트와 광고 이벤트는 모두 0개였습니다.
따라서 앱의 서비스·이름 필터 이전 단계에서 광고가 전달되지 않습니다.
이 이미지의 RF 보정 종류 출력은 1바이트 enum을 4바이트로 읽은 진단 오류가 있어
다음 이미지에서 수정했습니다. 원래 보정 함수의 호출 인수와 실행 결과는 바뀌지 않았습니다.

이 단계에서는 NU87↔NU40 연결과 OLED ACK를 아직 확인하지 못했습니다.

Mac에서 NU40으로 직접 연결한 분리 검사에서는 텍스트 63바이트를 5개 조각으로
전송하고, 시퀀스 `0x4242`의 ACK 상태 0을 받았습니다. 모든 ATT 쓰기 응답과
연결 해제도 확인했습니다. 이는 NU40의 수신·OLED 처리 경로 검사이며 NU87 경유 성공은 아닙니다.

## 과거 기록: Wi-Fi 단독 확인

2026-10-10, 사용자가 교체한 NU87에 게이트웨이를 기록하고 맥북과 같은 테스트 Wi-Fi에서 HTTP 응답을 확인했습니다.

### 당시 확인한 연결

- 대상: CP2102N `10c4:ea60`, USB 일련번호 `<NU87-USB-SERIAL>`
- 기록 포트: `<NU87-SERIAL-PORT>`, 업로더 115200 baud / 앱 콘솔 1500000 baud
- 테스트 AP: `<TEST-WIFI-SSID>` (WPA2, 사용자가 개발·테스트망으로 확인)
- 맥북 IPv4: `<MAC-LAN-IP>`
- NU87 DHCP IPv4: `<NU87-DHCP-IP>` (재부팅 시 바뀔 수 있음)
- `/v1/status`: 인증 요청 3회 HTTP 200, 각각 111 / 60 / 87ms
- 인증 없는 상태 요청: HTTP 401
- 시리얼 모니터를 종료한 뒤에도 추가 인증 요청 HTTP 200
- 직전 HTTP 수정 이미지의 main 스택 여유: 8,040바이트 / 총 12KiB (최종 이미지에서는 재측정하지 않음)
- 로컬 제어 화면: `Wi-Fi 연결됨 · NU40 대기`, NU87 IP 표시 확인

Wi-Fi 연결은 시리얼의 주소 출력만으로 판단하지 않고 맥북에서 실제 HTTP 응답을 받아 검증했습니다.
로컬 제어 서버는 `127.0.0.1:8766`에만 바인딩합니다. LAN 펌웨어 제공은 현재 실행에서 꺼져 있습니다.

### 당시 기록한 이미지

- 앱: `build/nu87-lcd-gateway/images/km0_km4_app.bin`, 623,040바이트, 주소 `0x14000`
- 앱 SHA-256: `4f4fd57f69d6c524368588d74bf0ade14296087b6f641e95733b0f4c49173249`
- 부트로더: 28,928바이트, 주소 `0x0`
- 부트로더 SHA-256: `74d86e9c144fe5018835de4dfb308c1350057d614fda016576099ea379a60d6e`

검증된 Realtek 업로더로 부트로더와 앱을 함께 기록했습니다. 업로더 종료 코드 0과
`All images are sent successfully!`, 이후 Zephyr 앱 부팅 및 위 HTTP 통신을 확인했습니다.
전체 플래시를 다시 읽어 비교한 결과는 아닙니다. DHCP 직후의 첫 HTTP 확인은 실패했으며,
이후 시리얼 포트를 닫은 상태에서 위 3회 응답과 인증 거부를 확인했습니다.

로컬 기록: `flash-wifi-final.log`, `boot-wifi-final.log`, `http-final-verified.json` (`build/nu87-lcd-gateway/` 안에 있으며 Git 제외).
Wi-Fi 비밀번호/API 토큰은 `local.conf`와 생성된 이미지에 포함되므로 이 파일들은 배포하지 않습니다.

### 당시 원인과 수정

1. STA 인터페이스 활성화 및 초기화 후 실제 무선 MAC 주소 반영.
2. 인터럽트에서 mutex를 사용하던 vendor Wi-Fi 인증 타이머를 전용 스레드로 실행.
3. HTTP 요청 처리 중 발생한 main 스택 초과를 12KiB 확보로 수정하고 여유량 실측.
4. 연결/DHCP 예산을 60초로 늘리고, 비동기 연결 해제 완료를 실제 무선 상태로 확인한 뒤
   재시도하는 보완본을 기록했습니다. 정상 연결·HTTP 응답은 실기기로 확인했고, 취소·재시도
   경계 조건은 실제 함수로 실행한 ASan/UBSan 호스트 테스트로 확인했습니다. 실기기의
   강제 DHCP 시간 초과나 AP 전원 중단 복구는 추가 검증하지 않았습니다.

### Wi-Fi 단독 확인 당시의 제한

- 맥북에서 `NU40-KIT-DISPLAY` 광고를 발견하고 BLE 연결·GATT 검색·알림 구독까지 확인했습니다.
  서비스는 `90E2C000-3C0D-4EF3-9D78-574CD1A64087`, `…C001`은 알림,
  `…C002`는 응답 있는 쓰기 특성입니다. 10초 구독 동안 알림은 없었습니다.
  제공받은 NU40 저장소에서 명령 형식을 확인했고 [KIT 모드](KIT-DISPLAY.md)에 반영했습니다.
  이후 Mac 직접 연결의 화면 텍스트 전송·ACK 검사는 위에 기록했습니다. NU40 펌웨어는 변경하지 않았으며 BLE 연결은 해제했습니다.
- 이 Wi-Fi 단독 검사 당시 NU87에 NU40 주소는 설정되지 않았습니다. **NU87↔NU40 BLE**, OLED 표시,
  OTA 및 전원 중단 복구는 미검증입니다. macOS의 CoreBluetooth 기기 UUID는 BLE MAC 주소가 아닙니다.
- NU87 자체 OTA나 IMU 동시 실행을 구현한 결과가 아닙니다.
- 이 보드는 vendor 초기화 중 `Efuse empty! ... Press any key ...`에서 입력을 기다립니다.
  관측한 경고에만 개행 한 번을 보내 시작했습니다. eFuse는 기록하지 않았습니다.
  전원을 다시 켜면 이 입력이 필요할 수 있으며 USB 없이 자동 시작하는 동작은 확인하지 못했습니다.
- 시리얼 포트를 열 때 USB 제어 신호로 보드가 재시작될 수 있으므로, 이미 연결된 보드는
  HTTP로 먼저 확인합니다.
- 테스트망용 구현이며 보안 및 복구 제한은 [구성 문서](../../docs/WIRELESS-LCD.md)를 참고하세요.

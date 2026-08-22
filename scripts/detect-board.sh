#!/usr/bin/env bash
set -euo pipefail

BOOT_VOLUME=""
for candidate in /Volumes/NRF52BOOT /Volumes/BARAM-NU40; do
  if [[ -f "${candidate}/INFO_UF2.TXT" ]]; then
    BOOT_VOLUME="${candidate}"
    break
  fi
done

if [[ -n "${BOOT_VOLUME}" ]]; then
  INFO_FILE="${BOOT_VOLUME}/INFO_UF2.TXT"
  echo "UF2 부트로더 감지: ${BOOT_VOLUME}"
  sed -n '1,20p' "${INFO_FILE}"

  if grep -q '^Board-ID: nRF52840-pca10056-v1' "${INFO_FILE}" && \
     grep -q '^SoftDevice: S140 6\.1\.1' "${INFO_FILE}"; then
    echo "호환성 확인: NU40-DK 레거시 PCA10056 + S140 6.1.1"
    echo "애플리케이션 안전 범위: 0x00026000..0x000ed000"
    exit 0
  fi

  if grep -q '^Board-ID: BARAM-NU40' "${INFO_FILE}"; then
    echo "최신 BARAM-NU40 부트로더가 감지되었습니다." >&2
    echo "현재 프로젝트는 연결된 레거시 S140 6.1.1 보드용입니다." >&2
    exit 2
  fi

  echo "경고: 연결된 UF2 보드가 이 프로젝트의 안전 조건과 다릅니다." >&2
  exit 2
fi

if system_profiler SPUSBDataType 2>/dev/null | grep -q 'PCA10056:'; then
  echo "NU40-DK USB 장치는 보이지만 UF2 볼륨은 없습니다."
  echo "RESET을 빠르게 두 번 눌러 UF2 부트로더로 진입하세요."
  exit 3
fi

echo "NU40-DK를 찾지 못했습니다. 데이터 전송 가능한 USB 케이블과 전원을 확인하세요." >&2
exit 1

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOT_VOLUME=""
UF2_FILE="${1:-${ROOT_DIR}/build/zephyr/zephyr.uf2}"

for candidate in /Volumes/NRF52BOOT /Volumes/BARAM-NU40; do
  if [[ -f "${candidate}/INFO_UF2.TXT" ]]; then
    BOOT_VOLUME="${candidate}"
    break
  fi
done

if [[ ! -f "${UF2_FILE}" ]]; then
  echo "플래시할 UF2가 없습니다: ${UF2_FILE}" >&2
  echo "먼저 'make build'를 실행하세요." >&2
  exit 1
fi

if [[ -z "${BOOT_VOLUME}" ]]; then
  echo "UF2 볼륨이 없습니다. RESET을 빠르게 두 번 누른 뒤 다시 시도하세요." >&2
  exit 1
fi

INFO_FILE="${BOOT_VOLUME}/INFO_UF2.TXT"
if ! grep -q '^Board-ID: nRF52840-pca10056-v1' "${INFO_FILE}" || \
   ! grep -q '^SoftDevice: S140 6\.1\.1' "${INFO_FILE}"; then
  echo "안전 중단: 연결된 보드가 NU40-DK 레거시 PCA10056 + S140 6.1.1이 아닙니다." >&2
  exit 2
fi

"${ROOT_DIR}/scripts/verify-uf2.py" "${UF2_FILE}"

if [[ "${DRY_RUN:-0}" == "1" ]]; then
  echo "플래시 사전 점검 완료: 보드에는 기록하지 않았습니다."
  exit 0
fi

SERIAL="$(system_profiler SPUSBDataType 2>/dev/null | awk '/PCA10056:/{seen=1} seen && /Serial Number:/{print $3; exit}')"
SERIAL="${SERIAL:-unknown}"
BACKUP_DIR="${ROOT_DIR}/backups"
BACKUP_FILE="${BACKUP_DIR}/CURRENT-${SERIAL}-$(date +%Y%m%d-%H%M%S).UF2"

mkdir -p "${BACKUP_DIR}"
if [[ -f "${BOOT_VOLUME}/CURRENT.UF2" ]]; then
  cp -X "${BOOT_VOLUME}/CURRENT.UF2" "${BACKUP_FILE}"
  echo "현재 펌웨어 백업: ${BACKUP_FILE}"
fi

echo "플래싱: ${UF2_FILE} -> ${BOOT_VOLUME}/FIRMWARE.UF2"
cp -X "${UF2_FILE}" "${BOOT_VOLUME}/FIRMWARE.UF2"
sync
echo "전송 완료. 보드가 자동 재부팅되며 UF2 볼륨이 사라지는 것이 정상입니다."

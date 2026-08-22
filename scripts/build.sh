#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEST="${ROOT_DIR}/scripts/west.sh"
BUILD_DIR="${ROOT_DIR}/build"
BOARD="nucode_nu40/nrf52840"
UF2_FILE="${BUILD_DIR}/zephyr/zephyr.uf2"

if [[ ! -x "${ROOT_DIR}/.venv/bin/west" ]]; then
  echo "Zephyr 환경이 없습니다. 먼저 'make setup'을 실행하세요." >&2
  exit 1
fi

"${WEST}" build \
  --pristine=always \
  --board "${BOARD}" \
  --build-dir "${BUILD_DIR}" \
  "${ROOT_DIR}"

if [[ ! -f "${UF2_FILE}" ]]; then
  echo "빌드는 끝났지만 UF2 산출물이 없습니다: ${UF2_FILE}" >&2
  exit 1
fi

"${ROOT_DIR}/scripts/verify-uf2.py" "${UF2_FILE}"
echo
echo "UF2 생성 완료: ${UF2_FILE}"

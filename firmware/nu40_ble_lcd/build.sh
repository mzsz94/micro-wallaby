#!/usr/bin/env bash
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${APP_DIR}/../.." && pwd)"
PROFILE="${1:-unconfigured}"
BUILD_DIR="${ROOT_DIR}/build/nu40-ble-lcd"
EXTRA_ARGS=("-DEXTRA_CONF_FILE=")
case "${PROFILE}" in
  unconfigured) ;;
  pcf8574-example)
    BUILD_DIR="${ROOT_DIR}/build/nu40-ble-lcd-example"
    EXTRA_ARGS=("-DEXTRA_CONF_FILE=${APP_DIR}/pcf8574-example.conf")
    ;;
  ssd1306-example)
    BUILD_DIR="${ROOT_DIR}/build/nu40-ble-lcd-ssd1306-example"
    EXTRA_ARGS=("-DEXTRA_CONF_FILE=${APP_DIR}/ssd1306-example.conf")
    ;;
  *) echo "Usage: $0 [unconfigured|pcf8574-example|ssd1306-example]" >&2; exit 2 ;;
esac
"${ROOT_DIR}/scripts/west.sh" build --pristine=always \
  --board nucode_nu40/nrf52840 --build-dir "${BUILD_DIR}" "${APP_DIR}" -- \
  "-DDTC_OVERLAY_FILE=${APP_DIR}/app.overlay" "${EXTRA_ARGS[@]}"
python3 "${ROOT_DIR}/firmware/nu40_imu_bringup/flash.py" \
  --layout legacy --image "${BUILD_DIR}/zephyr/zephyr.uf2" --verify-only

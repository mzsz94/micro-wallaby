#!/usr/bin/env bash
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${APP_DIR}/../.." && pwd)"
LAYOUT="${1:-baram}"
case "${LAYOUT}" in
  baram)
    BUILD_DIR="${ROOT_DIR}/build/nu40-imu"
    OVERLAYS="${APP_DIR}/app.overlay"
    ;;
  legacy)
    BUILD_DIR="${ROOT_DIR}/build/nu40-imu-legacy"
    OVERLAYS="${APP_DIR}/app.overlay;${APP_DIR}/legacy.overlay"
    ;;
  *) echo "Usage: $0 [baram|legacy]" >&2; exit 2 ;;
esac
"${ROOT_DIR}/scripts/west.sh" build --board nucode_nu40/nrf52840 \
  --build-dir "${BUILD_DIR}" "${APP_DIR}" -- "-DDTC_OVERLAY_FILE=${OVERLAYS}"
python3 "${APP_DIR}/flash.py" --image "${BUILD_DIR}/zephyr/zephyr.uf2" --layout "${LAYOUT}" --verify-only

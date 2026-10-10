#!/usr/bin/env bash
# Build only. This script never uploads, flashes, or opens a serial port.
set -euo pipefail
# local.conf credentials are compiled into generated headers and firmware.
umask 077
GATEWAY_APP="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATEWAY_ROOT="$(cd "${GATEWAY_APP}/../.." && pwd)"
GATEWAY_WORKSPACE="${GATEWAY_ROOT}/build/nu87-workspace"
GATEWAY_SDK="${GATEWAY_ROOT}/.zephyr-sdk"
GATEWAY_DEPS="${GATEWAY_ROOT}/build/nu87-gateway-deps"
GATEWAY_BUILD="${GATEWAY_BUILD_DIR:-${GATEWAY_ROOT}/build/nu87-lcd-gateway}"
for module in mbedtls-098e120afb51877ed814bdc04443890ae12e0642 TF-PSA-Crypto-fa92349590a54959ee2efc21b668e5afdb144726; do
  if [[ ! -d "${GATEWAY_DEPS}/${module}" ]]; then
    echo "Crypto dependencies missing; run bash firmware/nu87_lcd_gateway/setup.sh" >&2
    exit 1
  fi
done
GATEWAY_EXTRA_CONF="program.conf"
if [[ -f "${GATEWAY_APP}/local.conf" ]]; then GATEWAY_EXTRA_CONF="${GATEWAY_EXTRA_CONF};local.conf"; fi
export PATH="${GATEWAY_WORKSPACE}/.venv/bin:${PATH}"
export ZEPHYR_BASE="${GATEWAY_WORKSPACE}/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${GATEWAY_WORKSPACE}/sdk-compat"
export CCACHE_DIR="${GATEWAY_ROOT}/build/nu87-gateway-cache/ccache"
cd "${GATEWAY_WORKSPACE}"
west build -b nucode_nu87 "${GATEWAY_APP}" -d "${GATEWAY_BUILD}" -- \
  -DWEST_TOPDIR="${GATEWAY_WORKSPACE}" \
  -DUSER_CACHE_DIR="${GATEWAY_ROOT}/build/nu87-gateway-cache" \
  -DZEPHYR_SDK_INSTALL_DIR="${GATEWAY_SDK}" \
  -DCMAKE_GDB="${GATEWAY_SDK}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb" \
  -DZEPHYR_EXTRA_MODULES="${GATEWAY_DEPS}/mbedtls-098e120afb51877ed814bdc04443890ae12e0642;${GATEWAY_DEPS}/TF-PSA-Crypto-fa92349590a54959ee2efc21b668e5afdb144726" \
  -DEXTRA_CONF_FILE="${GATEWAY_EXTRA_CONF}" "$@"

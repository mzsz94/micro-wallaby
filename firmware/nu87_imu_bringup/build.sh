#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${APP_DIR}/../.." && pwd)"
NU87_WORKSPACE="${ROOT_DIR}/build/nu87-workspace"
NU87_BUILD_DIR="${ROOT_DIR}/build/nu87-imu"
NU87_SDK="${ROOT_DIR}/.zephyr-sdk"
NU87_COMPAT="${NU87_WORKSPACE}/sdk-compat"

if [[ ! -x "${NU87_WORKSPACE}/.venv/bin/west" || ! -d "${NU87_WORKSPACE}/zephyr" ]]; then
  echo "NU87 workspace is missing; run bash firmware/nu87_imu_bringup/setup.sh first." >&2
  exit 1
fi
bash "${APP_DIR}/apply_i2c_patch.sh"
mkdir -p "${NU87_COMPAT}"
if [[ ! -e "${NU87_COMPAT}/arm-zephyr-eabi" ]]; then
  ln -s "${NU87_SDK}/gnu/arm-zephyr-eabi" "${NU87_COMPAT}/arm-zephyr-eabi"
fi
export PATH="${NU87_WORKSPACE}/.venv/bin:${PATH}"
export ZEPHYR_BASE="${NU87_WORKSPACE}/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="${NU87_COMPAT}"
export CCACHE_DIR="${NU87_WORKSPACE}/.ccache"
cd "${NU87_WORKSPACE}"
west build -b nucode_nu87 "${APP_DIR}" -d "${NU87_BUILD_DIR}" -- \
  -DWEST_TOPDIR="${NU87_WORKSPACE}" \
  -DZEPHYR_SDK_INSTALL_DIR="${NU87_SDK}" \
  -DCMAKE_GDB="${NU87_SDK}/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb" \
  -DEXTRA_CONF_FILE=program.conf
python "${APP_DIR}/flash.py" --build-dir "${NU87_BUILD_DIR}" --port /dev/cu.nu87-build-validation

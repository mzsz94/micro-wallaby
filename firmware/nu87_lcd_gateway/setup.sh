#!/usr/bin/env bash
# Fetch only public, pinned crypto dependencies; does not flash anything.
set -euo pipefail
GATEWAY_APP="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATEWAY_ROOT="$(cd "${GATEWAY_APP}/../.." && pwd)"
GATEWAY_DEPS="${GATEWAY_ROOT}/build/nu87-gateway-deps"
mkdir -p "${GATEWAY_DEPS}"
fetch_dependency() {
  local repository="$1" revision="$2" sha256="$3" extracted="$4"
  local archive="${GATEWAY_DEPS}/${extracted}.tar.gz"
  if [[ -d "${GATEWAY_DEPS}/${extracted}" ]]; then return; fi
  curl --fail --location --retry 2 "https://codeload.github.com/zephyrproject-rtos/${repository}/tar.gz/${revision}" -o "${archive}"
  printf '%s  %s\n' "${sha256}" "${archive}" | shasum -a 256 -c -
  tar -xzf "${archive}" -C "${GATEWAY_DEPS}"
}
fetch_dependency mbedtls 098e120afb51877ed814bdc04443890ae12e0642 \
  7f3a576443622698cd4307bbbbc3024c0d4223085916316e77e4aacdd6ce23e7 \
  mbedtls-098e120afb51877ed814bdc04443890ae12e0642
fetch_dependency TF-PSA-Crypto fa92349590a54959ee2efc21b668e5afdb144726 \
  09d8b8a3d1e7fb05e44637e0a5f968f4a3b8ed244e29cea14e510806760ff093 \
  TF-PSA-Crypto-fa92349590a54959ee2efc21b668e5afdb144726
if [[ ! -x "${GATEWAY_ROOT}/build/nu87-workspace/.venv/bin/west" ]]; then
  echo "Base NU87 SDK missing. First run: bash firmware/nu87_imu_bringup/setup.sh" >&2
  exit 1
fi

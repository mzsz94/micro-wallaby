#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${APP_DIR}/../.." && pwd)"
NU87_WORKSPACE="${ROOT_DIR}/build/nu87-workspace"
NU87_SOURCE_REV=f9e463f70d8aa822a00ca637d459e9569002d580
NU87_SOURCE="${NU87_WORKSPACE}/micro-wallaby"
NU87_PYTHON="${NU87_PYTHON:-${ROOT_DIR}/.venv/bin/python}"

mkdir -p "${NU87_WORKSPACE}/manifest" "${NU87_WORKSPACE}/.west"
cp "${APP_DIR}/west.yml" "${NU87_WORKSPACE}/manifest/west.yml"
# west init refuses nested workspaces, so give this isolated directory its own config.
cat > "${NU87_WORKSPACE}/.west/config" <<'CONFIG'
[manifest]
path = manifest
file = west.yml
[zephyr]
base = zephyr
CONFIG

if [[ ! -d "${NU87_SOURCE}/.git" ]]; then
  git init "${NU87_SOURCE}"
  git -C "${NU87_SOURCE}" remote add origin https://github.com/mzsz94/micro-wallaby.git
  git -C "${NU87_SOURCE}" fetch --depth 1 origin "${NU87_SOURCE_REV}"
  git -C "${NU87_SOURCE}" checkout --detach "${NU87_SOURCE_REV}"
fi
if [[ "$(git -C "${NU87_SOURCE}" rev-parse HEAD)" != "${NU87_SOURCE_REV}" ]]; then
  echo "Unexpected micro-wallaby revision; use a fresh isolated workspace." >&2
  exit 1
fi
if [[ ! -x "${NU87_WORKSPACE}/.venv/bin/python" ]]; then
  "${NU87_PYTHON}" -m venv "${NU87_WORKSPACE}/.venv"
fi
export PATH="${NU87_WORKSPACE}/.venv/bin:${PATH}"
python -m pip install west
cd "${NU87_WORKSPACE}"
west update --narrow -o=--depth=1
python -m pip install -r zephyr/scripts/requirements-base.txt \
  -r modules/hal/realtek/ameba/scripts/requirements.txt 'python-mbedtls==2.10.1'
# Remove our follow-up patch before checking the original patch stack.
bash "${APP_DIR}/apply_i2c_patch.sh" --reverse
sh "${NU87_SOURCE}/tools/apply_west_patches.sh"
bash "${APP_DIR}/apply_i2c_patch.sh"
# Upstream's CMake validates every blob declared by hal_realtek.
west blobs fetch hal_realtek

#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${APP_DIR}/../.." && pwd)"
NU87_ZEPHYR="${ROOT_DIR}/build/nu87-workspace/zephyr"
NU87_PATCH="${APP_DIR}/patches/0001-amebad-restart-after-byte.patch"

if git -C "${NU87_ZEPHYR}" apply --reverse --check "${NU87_PATCH}" 2>/dev/null; then
  if [[ "${1:-}" == --reverse ]]; then
    git -C "${NU87_ZEPHYR}" apply --reverse "${NU87_PATCH}"
  fi
elif [[ "${1:-}" != --reverse ]]; then
  git -C "${NU87_ZEPHYR}" apply --check "${NU87_PATCH}"
  git -C "${NU87_ZEPHYR}" apply "${NU87_PATCH}"
fi

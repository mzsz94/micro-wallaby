#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEST="${ROOT_DIR}/.venv/bin/west"

if [[ ! -x "${WEST}" ]]; then
	echo "west가 없습니다. 먼저 'make setup'을 실행하세요." >&2
	exit 1
fi

export ZEPHYR_BASE="${ROOT_DIR}/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="${ROOT_DIR}/.zephyr-sdk"
export CCACHE_DIR="${ROOT_DIR}/.ccache"

cd "${ROOT_DIR}"
exec "${WEST}" "$@"

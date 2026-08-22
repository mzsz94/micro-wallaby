#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON_BIN="${PYTHON_BIN:-/opt/homebrew/bin/python3.12}"
REVISION="$(tr -d '[:space:]' < "${ROOT_DIR}/.zephyr-revision")"
VENV_DIR="${ROOT_DIR}/.venv"
WEST="${VENV_DIR}/bin/west"

for tool in cmake ninja dtc ccache; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "필수 도구가 없습니다: ${tool}" >&2
    echo "먼저 'make deps'를 실행하세요." >&2
    exit 1
  fi
done

if [[ ! -x "${PYTHON_BIN}" ]]; then
  echo "Python 3.12가 없습니다: ${PYTHON_BIN}" >&2
  echo "먼저 'make deps'를 실행하세요." >&2
  exit 1
fi

echo "[1/6] Python 가상환경 생성"
if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
  "${PYTHON_BIN}" -m venv "${VENV_DIR}"
fi
"${VENV_DIR}/bin/python" -m pip install --upgrade pip west

echo "[2/6] Zephyr 워크스페이스 초기화 (${REVISION})"
if [[ ! -f "${ROOT_DIR}/.west/config" ]]; then
  "${WEST}" init \
    -m https://github.com/zephyrproject-rtos/zephyr \
    --mr "${REVISION}" \
    "${ROOT_DIR}"
else
  if [[ ! -d "${ROOT_DIR}/zephyr/.git" ]]; then
    echo ".west 설정은 있지만 Zephyr 저장소가 없습니다." >&2
    exit 1
  fi
  if ! git -C "${ROOT_DIR}/zephyr" cat-file -e "${REVISION}^{commit}" 2>/dev/null; then
    git -C "${ROOT_DIR}/zephyr" fetch origin "${REVISION}"
  fi
  git -C "${ROOT_DIR}/zephyr" checkout --detach "${REVISION}"
fi

echo "[3/6] NU40-DK 필수 Zephyr 모듈 동기화"
cd "${ROOT_DIR}"
"${WEST}" update cmsis cmsis_6 hal_nordic

echo "[4/6] Zephyr Python 패키지 설치"
"${VENV_DIR}/bin/python" -m pip install \
  -r "${ROOT_DIR}/zephyr/scripts/requirements-base.txt"

echo "[5/6] ARM Zephyr SDK 설치"
export ZEPHYR_BASE="${ROOT_DIR}/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="${ROOT_DIR}/.zephyr-sdk"
"${WEST}" sdk install \
  --install-dir "${ZEPHYR_SDK_INSTALL_DIR}" \
  --gnu-toolchains arm-zephyr-eabi \
  --no-hosttools

echo "[6/6] 설치 확인"
"${WEST}" --version
cmake --version | head -n 1
"${VENV_DIR}/bin/python" --version
echo "Zephyr: $(git -C "${ROOT_DIR}/zephyr" rev-parse --short HEAD)"
echo
echo "설치 완료: make detect && make build"

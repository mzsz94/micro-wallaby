#!/usr/bin/env bash
# NU40: bash web/tools/start-imu-dashboard.sh
# NU87: bash web/tools/start-imu-dashboard.sh --board nu87 --serial-number <USB serial>
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHON="${ROOT_DIR}/.venv/bin/python"
owned_pids=()

cleanup() {
  if ((${#owned_pids[@]} == 0)); then return; fi
  for child_pid in "${owned_pids[@]}"; do
    kill "${child_pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  exec "${PYTHON}" "${ROOT_DIR}/web/tools/imu_bridge.py" "$@"
fi

if lsof -nP -iTCP:8765 -sTCP:LISTEN >/dev/null 2>&1; then
  if (($#)); then
    echo "An IMU bridge is already running on port 8765; board options were not applied." >&2
    echo "Stop that bridge, then rerun with --board and --serial-number." >&2
    exit 1
  fi
  echo "Using the existing IMU bridge. The dashboard shows its selected board."
else
  "${PYTHON}" "${ROOT_DIR}/web/tools/imu_bridge.py" "$@" &
  owned_pids+=("$!")
fi
if ! lsof -nP -iTCP:5178 -sTCP:LISTEN >/dev/null 2>&1; then
  (cd "${ROOT_DIR}/web" && exec npm run dev -- --host 127.0.0.1 --port 5178) &
  owned_pids+=("$!")
fi
echo "IMU dashboard: http://localhost:5178/imu"
echo "Keep this terminal open. Ctrl+C stops services started by this command."
if ((${#owned_pids[@]})); then
  wait
fi

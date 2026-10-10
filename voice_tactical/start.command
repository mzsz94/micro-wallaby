#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
voice_python="voice_tactical/.venv/bin/python"
if [ ! -x "$voice_python" ]; then voice_python=python3; fi
exec "$voice_python" -m voice_tactical.server "$@"

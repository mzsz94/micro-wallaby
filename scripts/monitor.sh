#!/usr/bin/env bash
set -euo pipefail

ports=(/dev/cu.usbmodem*)
if [[ ! -e "${ports[0]}" ]]; then
	echo "Zephyr USB serial port를 찾지 못했습니다."
	echo "보드가 실행 중이고 USB 케이블이 연결됐는지 확인하세요."
	exit 1
fi

port="${ports[0]}"
echo "센서 모니터: $port"
echo "종료: Ctrl+C"
stty -f "$port" 115200 raw -echo
exec cat "$port"

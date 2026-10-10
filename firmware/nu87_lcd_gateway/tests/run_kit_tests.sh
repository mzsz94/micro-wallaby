#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
kit_tests_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kit_gateway_dir=$(CDPATH= cd -- "$kit_tests_dir/.." && pwd)
kit_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/nu87-kit-tests.XXXXXX")
trap 'rm -rf "$kit_build_dir"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$kit_gateway_dir/include" "$kit_tests_dir/test_kit_protocol.c" \
  -o "$kit_build_dir/protocol"
"$kit_build_dir/protocol"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
  -fsanitize=address,undefined -I "$kit_tests_dir/kit_mock" \
  -I "$kit_gateway_dir/include" -I "$kit_gateway_dir/../wireless_lcd_common/include" \
  "$kit_tests_dir/test_kit_adapter.c" -o "$kit_build_dir/adapter"
"$kit_build_dir/adapter"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$kit_gateway_dir/include" "$kit_tests_dir/test_audio_protocol.c" \
  -o "$kit_build_dir/audio"
"$kit_build_dir/audio"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
  -fsanitize=address,undefined -I "$kit_tests_dir/kit_mock" \
  -I "$kit_gateway_dir/include" "$kit_tests_dir/test_smp_relay.c" \
  -o "$kit_build_dir/smp"
"$kit_build_dir/smp"
for relay in alarm_relay robot_state; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable \
    -fsanitize=address,undefined -I "$kit_tests_dir/kit_mock" -I "$kit_gateway_dir/include" \
    "$kit_tests_dir/test_${relay}.c" -o "$kit_build_dir/$relay"
  "$kit_build_dir/$relay"
done

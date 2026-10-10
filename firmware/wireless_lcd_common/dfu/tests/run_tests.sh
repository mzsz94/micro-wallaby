#!/bin/sh
set -eu
dfu_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dfu_test_bin=$(mktemp "${TMPDIR:-/tmp}/legacy-dfu-tests.XXXXXX")
trap 'rm -f "$dfu_test_bin"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$dfu_dir" "$dfu_dir/legacy_dfu_client.c" \
    "$dfu_dir/tests/test_legacy_dfu_client.c" -o "$dfu_test_bin"
"$dfu_test_bin"

#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
transport_tests_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
transport_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/nu87-hci-tests.XXXXXX")
trap 'rm -rf "$transport_build_dir"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -I "$transport_tests_dir/transport_mock" \
  "$transport_tests_dir/test_hci_transport_diagnostics.c" -o "$transport_build_dir/transport"
"$transport_build_dir/transport"

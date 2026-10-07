#!/bin/sh
# Linux host regression tests. Uses fake evdev nodes and never opens /dev/input.
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/anland-input-grab-test.XXXXXX")
trap 'rm -rf -- "$test_build_dir"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
    -I"$test_dir/include" -I"$test_dir/../../../../../common" \
    "$test_dir/input_grab_test.c" \
    "$test_dir/../../../../../common/socket_utils.c" \
    -o "$test_build_dir/input_grab_test"
"$test_build_dir/input_grab_test"

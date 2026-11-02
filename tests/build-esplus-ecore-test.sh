#!/bin/sh
# Build + run tests/esplus_ecore_test.c on the host (no TV, no toolchain).
# esplusplayer_tv50.h asserts the 32-bit ARM struct layout; a 64-bit host cannot meet
# it, and the test does not depend on it, so those asserts are compiled out here.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); R="$HERE/.."
OUT=${OUT:-${TMPDIR:-/tmp}/esplus_ecore_test}
for D in "" "-DMLT_TRACE"; do
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" \
    -o "$OUT" "$HERE/esplus_ecore_test.c" "$R/mlclient/esplus_player.c" -lpthread -ldl
  "$OUT" 2>/dev/null
done

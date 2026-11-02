#!/bin/sh
# Milestone 1, step 1: compile the portable Moonlight core (moonlight-common-c
# + enet + reedsolomon) for Tizen 5.0 armv7l with the Tizen GCC 6.2 toolchain.
# This is the FEASIBILITY build - it stops at .o files. Crypto (PlatformCrypto.c,
# needs mbedTLS) and libgamestream (needs OpenSSL 1.0 + libcurl) are NOT built
# here.
#
# Result on first run (2026-11-01): 27/27 core files compile, 0 warnings.
#
# Env:
#   MLT   = path to a moonlight-tizen checkout (has moonlight-common-c/)
#   DEPS  = tizen-deps dir (fetch-tizen-sources.sh output). Must contain NO
#           spaces in the path (symlink it if yours does).
set -e
MLT=${MLT:?set MLT to a moonlight-tizen checkout}
DEPS=${DEPS:?set DEPS to the tizen-deps dir (no spaces in path)}
C=$MLT/moonlight-common-c
OUT=${OUT:-./core-obj}
SHIM=$OUT/shim
mkdir -p "$OUT" "$SHIM"

# Two vestigial Emscripten references in the fork's Platform.h. The real native
# port removes them (route Limelog to ListenerCallbacks.logMessage); this shim
# only lets the feasibility build proceed. No core .c file calls any emscripten API.
cat > "$SHIM/emscripten.h" <<'EOF'
#include <stdio.h>
#define EM_LOG_CONSOLE 1
#define emscripten_log(flags, ...) fprintf(stderr, __VA_ARGS__)
EOF

GCC=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-gcc
FLAGS="--sysroot=$DEPS/sysroot -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp -O2 -std=gnu11 -fPIC -DHAS_SOCKLEN_T -DNDEBUG"
INC="-I$C/src -I$C/enet/include -I$C/reedsolomon -I$SHIM"

ok=0; fail=0
for f in "$C"/src/*.c "$C"/enet/*.c "$C"/reedsolomon/rs.c; do
  b=$(basename "$f" .c)
  # win32: Windows only. PlatformCrypto: needs mbedTLS (not built here).
  # FakeCallbacks/ConnectionTester: test helpers, not part of the library.
  case "$b" in win32|PlatformCrypto|FakeCallbacks|ConnectionTester) continue;; esac
  if "$GCC" $FLAGS $INC -Wall -Wextra -Wno-unused-parameter -c "$f" -o "$OUT/$b.o"; then
    ok=$((ok+1)); else fail=$((fail+1)); fi
done
echo "core compiled OK=$ok FAIL=$fail -> $OUT"
[ "$fail" = 0 ]

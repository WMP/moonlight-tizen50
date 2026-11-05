#!/bin/sh
# Build + run tests/esplus_ecore_test.c and tests/video_esplus_test.c on the host (no TV, no toolchain).
# esplusplayer_tv50.h asserts the 32-bit ARM struct layout; a 64-bit host cannot meet
# it, and the test does not depend on it, so those asserts are compiled out here.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); R="$HERE/.."
OUT=${OUT:-${TMPDIR:-/tmp}/esplus_ecore_test}
LC="$R/third_party/moonlight-tizen/moonlight-common-c/src"
[ -f "$LC/Limelight.h" ] || { echo "Limelight.h missing: run 'git submodule update --init'" >&2; exit 1; }
for D in "" "-DMLT_TRACE"; do
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" \
    -o "$OUT" "$HERE/esplus_ecore_test.c" "$R/mlclient/esplus_player.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D '-D_Static_assert(a,b)=' -I"$R" -I"$R/mlclient" -I"$LC" \
    -o "$OUT-video" "$HERE/video_esplus_test.c" "$R/mlclient/video_esplus.c" "$R/mlclient/esplus_player.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-video" 2>/dev/null
  gcc -std=gnu11 -Wall -Wextra -Werror $D -I"$R/mlclient" -I"$LC" \
    -o "$OUT-pad" "$HERE/engine_pad_test.c" "$R/mlclient/engine_pad.c" "$R/mlclient/mldiag.c" -lpthread -ldl
  "$OUT-pad" 2>/dev/null
done
# mldiag self-test
gcc -std=gnu11 -Wall -Wextra -Werror -I"$R/mlclient" -o "$OUT-diag" "$HERE/mldiag_test.c" "$R/mlclient/mldiag.c" -lpthread
"$OUT-diag"
# control-stream encrypt check (needs OpenSSL headers)
if printf '#include <openssl/evp.h>\n' | gcc -E - >/dev/null 2>&1; then
  gcc -std=gnu11 -Wall -Wextra -Werror -I"$R/mlclient" -o "$OUT-ctl" "$HERE/ctlcrypto_test.c" "$R/mlclient/ctlcrypto.c" "$R/mlclient/mldiag.c" -lpthread -lcrypto
  "$OUT-ctl"
else
  echo "ctlcrypto_test SKIPPED: no OpenSSL headers"
fi

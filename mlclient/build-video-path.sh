#!/bin/sh
# Milestone 1 step 3 (partial): compile the esplusplayer video sink
# (video_esplus.c, a moonlight-common-c DECODER_RENDERER_CALLBACKS) and
# link-test the WHOLE native video path for Tizen 5.0:
#   core + PlatformCrypto(mbedTLS) + libgamestream(OpenSSL/curl) + video sink
#   + esplusplayer + ecore, against the TV's real libraries.
#
# Result (2026-11-01): compiles 0 warnings, links to an ARM ELF with NEEDED
# libesplusplayer/libecore*/libssl/libcrypto/libcurl, no glibc symbol > 2.24.
# --allow-shlib-undefined is needed for esplusplayer's transitive TV-only deps
# (same as build-esppplay.sh); they resolve at runtime on the TV.
#
# NOT yet done: the GameStream HTTP orchestration (serverinfo GET / pairing /
# app launch / RTSP-url parse) that the WASM fork does in JS - port from
# moonlight-embedded client.c - and an end-to-end stream test (needs a Sunshine
# host on the LAN and the TV powered on).
#
# Env: MLT, DEPS (no spaces), BPD (from fetch-backport-deps.sh),
#      DEPSBUILD (from build-deps-lib.sh, has obj/ gs/ libmbedcrypto_tv.a).
set -e
MLT=${MLT:?}; DEPS=${DEPS:?}; BPD=${BPD:?}; DEPSBUILD=${DEPSBUILD:?}
HERE=$(cd "$(dirname "$0")" && pwd)
GCC=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-gcc
OBJD=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-objdump
C=$MLT/moonlight-common-c; TVL=$DEPS/tv-libs/usr/lib
OUT=${OUT:-./video-build}; mkdir -p "$OUT"
FLAGS="--sysroot=$DEPS/sysroot -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp -O2"

"$GCC" $FLAGS -std=gnu11 -Wall -Wextra -I"$C/src" -I"$HERE/.." -c "$HERE/video_esplus.c" -o "$OUT/video_esplus.o"
"$GCC" $FLAGS -std=gnu11 -Wall -Wextra -I"$C/src" -I"$HERE/.." -c "$HERE/esplus_player.c" -o "$OUT/esplus_player.o"
"$GCC" $FLAGS -std=gnu11 -Wall -Wextra -c "$HERE/mldiag.c" -o "$OUT/mldiag.o"

cat > "$OUT/vtest.c" <<'EOF'
#include <openssl/x509.h>
#include "Limelight.h"
X509 *g_Cert; EVP_PKEY *g_PrivateKey; char *g_UniqueId; char *g_CertHex;
extern void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS);
extern int gs_pair(int,const char*,unsigned short,const char*,char**);
int main(void){ DECODER_RENDERER_CALLBACKS dr; VidEsplus_FillCallbacks(&dr);
  return (dr.submitDecodeUnit!=0)&&((void*)gs_pair!=0); }
EOF
"$GCC" $FLAGS -I"$C/src" -I"$BPD/ossl-inc" -c "$OUT/vtest.c" -o "$OUT/vtest.o"
"$GCC" $FLAGS -o "$OUT/vtest" "$OUT/vtest.o" "$OUT/video_esplus.o" "$OUT/esplus_player.o" "$OUT/mldiag.o" \
  "$DEPSBUILD"/obj/*.o "$DEPSBUILD"/gs/*.o "$DEPSBUILD/libmbedcrypto_tv.a" \
  -L"$TVL" -Wl,-rpath-link,"$TVL" -Wl,--allow-shlib-undefined \
  -l:libesplusplayer.so -l:libecore_wl2.so.1 -l:libecore.so.1 \
  -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -l:libcurl.so.4 -lpthread
echo "video path linked:"; file "$OUT/vtest" | cut -d, -f1-3
bad=$("$OBJD" -T "$OUT/vtest" | grep -oE 'GLIBC_2\.(2[5-9]|[3-9][0-9])' | sort -u || true)
[ -z "$bad" ] && echo "glibc symbols: all <= 2.24 OK" || { echo "TOO-NEW glibc: $bad"; exit 1; }

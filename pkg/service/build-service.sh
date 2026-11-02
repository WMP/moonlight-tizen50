#!/bin/sh
# Build the native streaming service ELF (moonlight-engine) for the hybrid package:
# mlservice.c (service_app + message-port bridge) + the esplusplayer engine, linked
# against the TV's appfw/message-port/bundle/esplusplayer/openssl/curl libs.
#
# Env: MLT, DEPS (no spaces), BPD (fetch-backport-deps.sh), DEPSBUILD (build-deps-lib.sh).
set -e
MLT=${MLT:?}; DEPS=${DEPS:?}; BPD=${BPD:?}; DEPSBUILD=${DEPSBUILD:?}
HERE=$(cd "$(dirname "$0")" && pwd)
MLC=$HERE/../../mlclient            # video_esplus.c, audio_opus.c, esplus_player.c, gs_client.c
NAT=$HERE/../..                     # esplusplayer_tv50.h
GCC=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-gcc
OBJD=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-objdump
C=$MLT/moonlight-common-c; TVL=$DEPS/tv-libs/usr/lib
OUT=${OUT:-./service-build}; mkdir -p "$OUT"
cp "$NAT/esplusplayer_tv50.h" "$OUT/"
FLAGS="--sysroot=$DEPS/sysroot -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp -O2 -std=gnu11 -Wall -Wextra"
INC="-I$C/src -I$OUT -I$MLT/libgamestream -I$BPD/ossl-inc -I$BPD/curl759/include -I$DEPS/sysroot/usr/include/appcore-agent -I$DEPS/sysroot/usr/include/appfw"

"$GCC" $FLAGS $INC -c "$HERE/mlservice.c" -o "$OUT/mlservice.o"
for s in video_esplus audio_opus esplus_player gs_client; do
  "$GCC" $FLAGS -I$C/src -I$OUT -I$MLT/libgamestream -I$BPD/ossl-inc -I$BPD/curl759/include -c "$MLC/$s.c" -o "$OUT/$s.o"
done

"$GCC" --sysroot=$DEPS/sysroot -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp -O2 \
  -o "$OUT/moonlight-engine" \
  "$OUT"/mlservice.o "$OUT"/video_esplus.o "$OUT"/audio_opus.o "$OUT"/esplus_player.o "$OUT"/gs_client.o \
  "$DEPSBUILD"/obj/*.o "$DEPSBUILD"/gs/*.o "$DEPSBUILD/libmbedcrypto_tv.a" \
  -L"$TVL" -Wl,-rpath-link,"$TVL" -Wl,--allow-shlib-undefined \
  -l:libesplusplayer.so -l:libecore_wl2.so.1 -l:libecore.so.1 \
  -l:libappcore-agent.so.1 -l:libcapi-message-port.so.1 -l:libbundle.so.0 \
  -l:libssl.so.1.0.0 -l:libcrypto.so.1.0.0 -l:libcurl.so.4 -lpthread
echo "built: $(file "$OUT/moonlight-engine" | cut -d, -f1-3)"
bad=$("$OBJD" -T "$OUT/moonlight-engine" | grep -oE 'GLIBC_2\.(2[5-9]|[3-9][0-9])' | sort -u || true)
[ -z "$bad" ] && echo "glibc symbols: all <= 2.24 OK" || { echo "TOO-NEW glibc: $bad"; exit 1; }

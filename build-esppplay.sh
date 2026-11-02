#!/bin/sh
# Build esppplay for the TV. Needs fetch-tizen-sources.sh output and
# TV libraries copied to $DEPS/tv-libs (ssh root@TV 'tar cf - lib usr/lib' | tar xf - -C $DEPS/tv-libs).
set -e
cd "$(dirname "$0")"
DEPS=${DEPS:-../tizen-deps}
CC=$DEPS/toolchain/data/tools/arm-linux-gnueabi-gcc-6.2/bin/arm-linux-gnueabi-gcc
TVL=$DEPS/tv-libs/usr/lib
$CC --sysroot=$DEPS/sysroot -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=softfp \
  -O2 -Wall -Wextra -std=gnu11 -o esppplay esppplay.c \
  -L$TVL -Wl,-rpath-link,$TVL -Wl,--allow-shlib-undefined \
  -lesplusplayer -l:libecore_wl2.so.1 -l:libecore.so.1 -lpthread
# glibc symbol version check: nothing above GLIBC_2.24 may be referenced
! ${CC%gcc}objdump -T esppplay | grep -E 'GLIBC_2\.(2[5-9]|[3-9][0-9])'
echo "built esppplay"

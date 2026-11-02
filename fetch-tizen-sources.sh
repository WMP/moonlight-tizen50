#!/usr/bin/env bash
# Download Tizen sources, headers and toolchain for a native Moonlight client
# on a Samsung TV with Tizen 5.0 (armv7l, glibc 2.24, GCC 6.2, softfp).
#
# Usage: fetch-tizen-sources.sh [output-dir]   (default: ./tizen-deps)
# Needs: curl, unzip, and bsdtar (libarchive-tools) or rpm2cpio + cpio.
#
# Result:
#   <out>/src/      reference sources (esplusplayer, trackrenderer, capi-media-*)
#   <out>/sysroot/  Tizen 5.0 armv7l headers + libs to compile against
#   <out>/toolchain Tizen cross-arm-gcc-6.2 (same GCC major as the TV firmware)
#   <out>/rpm/      downloaded packages (kept for re-extraction)
#
# Note: libesplusplayer on the TV has an OLDER API (esplusplayer_add_*_stream_info)
# than any public source. The sources here are a reference only; struct layouts
# must still be checked against the TV binary.
set -euo pipefail

OUT=$(realpath -m "${1:-./tizen-deps}")
SNAP=https://download.tizen.org/snapshots/TIZEN
T50_BASE=$SNAP/Tizen-5.0/Tizen-5.0-Base/latest/repos/standard
T50_UNI=$SNAP/Tizen-5.0/Tizen-5.0-Unified/latest/repos/standard
T65_UNI=$SNAP/Tizen-6.5/Tizen-6.5-Unified/latest/repos/standard
SDK=https://download.tizen.org/sdk/tizenstudio/official/binary
TOOLCHAIN_ZIP=cross-arm-gcc-6.2_0.2.2_ubuntu-64.zip

mkdir -p "$OUT"/{rpm,src,sysroot}
declare -A LISTING

# Newest file in a directory listing whose name matches "<name>-<digit>...<suffix>".
pick() { # dir name suffix
  local dir=$1 name=$2 suffix=$3
  [[ -n ${LISTING[$dir]:-} ]] || LISTING[$dir]=$(curl -fsS --retry 3 "$dir/" | grep -oE 'href="[^"]+"' | cut -d'"' -f2 | sed 's/%2B/+/g')
  grep -E "^$(sed 's/[+.]/\\&/g' <<<"$name")-[0-9].*$suffix\$" <<<"${LISTING[$dir]}" | sort -V | tail -1
}

fetch() { # dir name suffix -> prints local path, or nothing if not found
  local f; f=$(pick "$1" "$2" "$3")
  if [[ -z $f ]]; then echo "WARN: $2 not found in $1" >&2; return 0; fi
  [[ -s $OUT/rpm/$f ]] || curl -fsS --retry 3 -o "$OUT/rpm/$f" "$1/$(sed 's/+/%2B/g' <<<"$f")"
  echo "$OUT/rpm/$f"
}

unrpm() { # rpm destdir
  mkdir -p "$2"
  if command -v bsdtar >/dev/null; then bsdtar -xf "$1" -C "$2"
  else (cd "$2" && rpm2cpio "$1" | cpio -idm --quiet); fi
}

echo "== Reference sources"
for spec in "$T65_UNI/source esplusplayer" "$T65_UNI/source libtrackrenderer" \
            "$T50_UNI/source capi-media-player" "$T50_UNI/source capi-media-tool"; do
  read -r dir name <<<"$spec"
  r=$(fetch "$dir" "$name" '\.src\.rpm'); [[ -n $r ]] || continue
  d=$OUT/src/$(basename "$r" .src.rpm); rm -rf "$d"; unrpm "$r" "$d"
  for t in "$d"/*.tar.*; do [[ -e $t ]] && tar -xf "$t" -C "$d" && rm "$t"; done
  echo "   $d"
done

echo "== Tizen 5.0 armv7l sysroot"
BASE_PKGS="glibc glibc-devel libgcc libstdc++ libstdc++-devel"
UNI_PKGS="capi-base-common-devel glib2-devel gstreamer-devel capi-media-tool-devel capi-media-player-devel
  capi-media-audio-io-devel ecore-wl2-devel ecore-wayland-devel ecore-devel eina-devel
  efl-devel libtbm-devel wayland-devel libwayland-client
  capi-appfw-application-devel capi-appfw-app-common-devel capi-appfw-app-control-devel capi-appfw-service-application-devel message-port-devel bundle-devel"
for p in $BASE_PKGS; do r=$(fetch "$T50_BASE/packages/armv7l" "$p" '\.armv7l\.rpm'); [[ -n $r ]] && unrpm "$r" "$OUT/sysroot"; done
r=$(fetch "$T50_BASE/packages/noarch" linux-glibc-devel '\.noarch\.rpm'); [[ -n $r ]] && unrpm "$r" "$OUT/sysroot"
# The RPM %post normally links asm -> asm-<arch>; extraction skips scripts.
[[ -e $OUT/sysroot/usr/include/asm ]] || ln -s asm-arm "$OUT/sysroot/usr/include/asm"
for p in $UNI_PKGS; do r=$(fetch "$T50_UNI/packages/armv7l" "$p" '\.armv7l\.rpm'); [[ -n $r ]] && unrpm "$r" "$OUT/sysroot"; done

echo "== Cross toolchain (GCC 6.2)"
if [[ ! -d $OUT/toolchain ]]; then
  curl -fsS --retry 3 -o "$OUT/rpm/$TOOLCHAIN_ZIP" "$SDK/$TOOLCHAIN_ZIP"
  unzip -q "$OUT/rpm/$TOOLCHAIN_ZIP" -d "$OUT/toolchain"
fi
gcc_bin=$(find "$OUT/toolchain" -name '*-gcc' -type f -perm -u+x | head -1)

# Self-check: the pieces the native client needs must be present.
missing=0
for f in usr/include/stdio.h usr/include/media/media_packet.h usr/include/gstreamer-1.0/gst/gst.h \
         usr/include/glib-2.0/glib.h; do
  [[ -e $OUT/sysroot/$f ]] || { echo "MISSING: sysroot/$f" >&2; missing=1; }
done
ls "$OUT"/src/esplusplayer-*/esplusplayer-*/include/esplusplayer_capi/esplusplayer_capi.h >/dev/null 2>&1 \
  || { echo "MISSING: esplusplayer reference headers" >&2; missing=1; }
[[ -n $gcc_bin ]] || { echo "MISSING: cross gcc in toolchain/" >&2; missing=1; }

echo
echo "Sources:   $OUT/src"
echo "Sysroot:   $OUT/sysroot"
echo "Compiler:  ${gcc_bin:-<not found>}"
echo "Next: copy /lib and /usr/lib from the TV to $OUT/tv-libs (link against those, not sysroot .so files)."
exit $missing

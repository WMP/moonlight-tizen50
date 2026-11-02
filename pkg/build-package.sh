#!/bin/sh
# Build the hybrid Moonlight Tizen 5.0 package: native engine service (tpk) +
# moonlight-tizen web UI (wgt) with the transport swapped to the message port.
# Run inside the Docker image (pkg/Dockerfile) for a reproducible toolchain,
# or on a host that has the same tools. Heavy/secret inputs come via env:
#
#   REPO       repo root (this checkout)
#   MLT        a moonlight-tizen checkout (web UI + moonlight-common-c)
#   DEPS       tizen-deps dir (fetch-tizen-sources.sh output; path without spaces)
#   BPD        backport-deps dir (fetch-backport-deps.sh output)
#   DEPSBUILD  build-deps-lib.sh output (core/gs objs + libmbedcrypto_tv.a)
#   TIZEN_CLI      Tizen Studio CLI dir (host ~/tizen-tmp/tools)      [phase 4]
#   TIZEN_PROFILE  signing profiles.xml path                          [phase 4]
#   SIGN_PROFILE   profile name in profiles.xml (default: Moonlight)  [phase 4]
set -e
: "${REPO:?}"; : "${MLT:?}"; : "${DEPS:?}"; : "${BPD:?}"; : "${DEPSBUILD:?}"
HERE="$REPO/pkg"
OUT=${OUT:-$HERE/out}; rm -rf "$OUT"; mkdir -p "$OUT/service/bin" "$OUT/ui"

echo "== phase 1: native engine service =="
OUT="$OUT/_sb" MLT="$MLT" DEPS="$DEPS" BPD="$BPD" DEPSBUILD="$DEPSBUILD" "$HERE/service/build-service.sh"
cp "$OUT/_sb/moonlight-engine" "$OUT/service/bin/moonlight-engine"
cp "$HERE/service/tizen-manifest.xml" "$OUT/service/tizen-manifest.xml"

echo "== phase 2: web UI (moonlight-tizen front-end + message-port transport) =="
# Ship the web front-end, NOT the WASM module.
cp "$MLT/wasm/index.html" "$OUT/ui/index.html"
cp -r "$MLT/wasm/platform" "$OUT/ui/platform"
cp -r "$MLT/wasm/static" "$OUT/ui/static" 2>/dev/null || true
[ -d "$MLT/wasm/res" ] && cp -r "$MLT/wasm/res" "$OUT/ui/res" || true
cp "$HERE/ui/mlbridge.js" "$OUT/ui/platform/mlbridge.js"
cp "$HERE/ui/config.xml" "$OUT/ui/config.xml"
cp "$HERE/ui/icon.png" "$OUT/ui/icon.png" 2>/dev/null || true
# Drop the Emscripten module loader and inject the message-port shim after messages.js.
sed -i '/src="platform\.js"/d' "$OUT/ui/index.html"
sed -i 's#<script type="text/javascript" src="platform/messages.js"></script>#&\n  <script type="text/javascript" src="platform/mlbridge.js"></script>#' "$OUT/ui/index.html"
echo "ui assembled -> $OUT/ui"

echo "== phase 3: tpk service project ready, wgt ui project ready =="
echo "service: $OUT/service   ui: $OUT/ui"

echo "== phase 4: package + sign =="
# tizen CLI is on PATH inside the image; allow override for host runs.
TZ=$(command -v tizen || echo "${TIZEN_CLI:+$TIZEN_CLI/tizen}")
if [ -n "$TZ" ] && [ -x "$TZ" ]; then
  P=${SIGN_PROFILE:-Moonlight}
  PWD_=${CERT_PASS:-tizenpkpass}
  # Use a mounted profile if given; otherwise generate a self-signed dev author
  # cert + security profile in-container (distributor = bundled Tizen dev cert).
  if [ -n "$TIZEN_PROFILE" ]; then
    "$TZ" cli-config "default.profiles.path=$TIZEN_PROFILE/profile/profiles.xml" >/dev/null 2>&1 || true
  elif ! "$TZ" security-profiles list 2>/dev/null | grep -q "^$P\b"; then
    echo "generating dev signing profile '$P' in-container"
    mkdir -p "$OUT/certs"
    "$TZ" certificate -a "$P" -p "$PWD_" -c PL -n Moonlight -- "$OUT/certs"
    "$TZ" security-profiles add -n "$P" -a "$OUT/certs/author.p12" -p "$PWD_"
  fi
  # web wgt (UI)
  "$TZ" build-web -- "$OUT/ui" || true
  "$TZ" package -t wgt -s "$P" -- "$OUT/ui"
  # native tpk (engine)
  "$TZ" package -t tpk -s "$P" -- "$OUT/service"
  # hybrid: combine the signed wgt into the tpk package (-r = HybridApp reference)
  "$TZ" package -t tpk -r "$(ls "$OUT"/ui/*.wgt 2>/dev/null | head -1)" -s "$P" -- "$OUT/service"
  echo "package(s):"; ls "$OUT/service"/*.tpk "$OUT/ui"/*.wgt 2>/dev/null
else
  echo "tizen CLI not found - run inside the Docker image (pkg/Dockerfile)."
fi

#!/bin/sh
# Build the Moonlight Tizen 5.0 web UI package (.wgt): the moonlight-tizen
# front-end with its transport swapped to the Tizen message port (mlbridge.js).
# The native streaming engine ships separately as the .NET host .tpk (build-tpk.sh).
# Run inside the Docker image (pkg/Dockerfile) for a reproducible toolchain.
#
#   REPO           repo root (this checkout)
#   MLT            a moonlight-tizen checkout (web UI source)
#   TIZEN_PROFILE  signing profiles.xml path (optional; else a dev cert is generated)
#   SIGN_PROFILE   profile name in profiles.xml (default: Moonlight)
set -e
: "${REPO:?}"; : "${MLT:?}"
HERE="$REPO/pkg"
OUT=${OUT:-$HERE/out}; rm -rf "$OUT"; mkdir -p "$OUT/ui"

echo "== phase 1: web UI (moonlight-tizen front-end + message-port transport) =="
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

echo "== phase 2: package + sign =="
# tizen CLI is on PATH inside the image; allow override for host runs.
TZ=$(command -v tizen || echo "${TIZEN_CLI:+$TIZEN_CLI/tizen}")
if [ -n "$TZ" ] && [ -x "$TZ" ]; then
  P=${SIGN_PROFILE:-Moonlight}
  PWD_=${CERT_PASS:-tizenpkpass}
  if [ -n "$TIZEN_PROFILE" ]; then
    "$TZ" cli-config "default.profiles.path=$TIZEN_PROFILE/profile/profiles.xml" >/dev/null 2>&1 || true
  elif ! "$TZ" security-profiles list 2>/dev/null | grep -q "^$P\b"; then
    echo "generating dev signing profile '$P' in-container"
    mkdir -p "$OUT/certs"
    "$TZ" certificate -a "$P" -p "$PWD_" -c PL -n Moonlight -- "$OUT/certs"
    "$TZ" security-profiles add -n "$P" -a "$OUT/certs/author.p12" -p "$PWD_"
  fi
  "$TZ" build-web -- "$OUT/ui" || true
  "$TZ" package -t wgt -s "$P" -- "$OUT/ui"
  echo "package(s):"; ls "$OUT/ui"/*.wgt 2>/dev/null
else
  echo "tizen CLI not found - run inside the Docker image (pkg/Dockerfile)."
fi

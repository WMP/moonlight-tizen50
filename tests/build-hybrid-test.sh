#!/bin/sh
# Self-test for pkg/build-hybrid.sh (host only, needs zip/unzip): a fake host .tpk
# with the SDK layout + a minimal UI dir -> check the hybrid .wgt layout.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT INT TERM
mkdir -p "$W/tpk/bin/res/lib" "$W/tpk/shared/res" "$W/ui/platform/.buildResult"
cp "$R/pkg/dotnet-host/tizen-manifest.xml" "$W/tpk/"
echo dll > "$W/tpk/bin/MoonlightHost.dll"; echo so > "$W/tpk/bin/res/lib/libmlengine.so"
echo sig > "$W/tpk/author-signature.xml"; echo sig > "$W/tpk/signature1.xml"
(cd "$W/tpk" && zip -qr ../host.tpk .)
sed 's/ version="0.0.1"/ version="1.2.3"/' "$R/pkg/ui/config.xml" > "$W/ui/config.xml"
echo '<html></html>' > "$W/ui/index.html"; echo js > "$W/ui/platform/mlbridge.js"
echo junk > "$W/ui/platform/.buildResult/x"; echo sig > "$W/ui/author-signature.xml"
sh "$R/pkg/build-hybrid.sh" "$W/host.tpk" "$W/ui" "$W/out.wgt" >/dev/null
L=$(unzip -Z1 "$W/out.wgt")
fail() { echo "FAIL: $*"; exit 1; }
[ "$(echo "$L" | head -2 | tr '\n' ' ')" = "config.xml tizen-manifest.xml " ] || fail "manifests not first"
for f in bin/MoonlightHost.dll bin/res/lib/libmlengine.so res/wgt/index.html res/wgt/config.xml res/wgt/platform/mlbridge.js; do
  echo "$L" | grep -qx "$f" || fail "missing $f"
done
echo "$L" | grep -q 'signature\|\.buildResult' && fail "signatures or build leftovers packed"
M=$(unzip -p "$W/out.wgt" tizen-manifest.xml)
echo "$M" | grep -q '<manifest[^>]* package="Moonlight5"' || fail "manifest package"
echo "$M" | grep -q '<manifest[^>]* version="1.2.3"' || fail "manifest version"
echo "$M" | grep -q 'appid="org.moonlight.host.app"' || fail "host app id changed"
unzip -p "$W/out.wgt" config.xml | grep -q 'privilege/network.get' || fail "host privilege not in root config.xml"
echo "build-hybrid ok"

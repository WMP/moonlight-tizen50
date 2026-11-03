#!/bin/sh
# Build the hybrid package: ONE .wgt with the web UI and the .NET host service.
#
# Why: since Tizen 2.4 a service app can be started only by an app of the SAME
# package. With the UI (.wgt) and the host (.tpk) in separate packages the UI cannot
# start the host. In one package it can (tizen.application.launch / launchAppControl).
#
# Layout of the result (a hybrid web package):
#   config.xml, tizen-manifest.xml   root; the host's manifest gets the web package id
#   bin/  lib/  res/  shared/ ...    the host service, as in its .tpk
#   res/wgt/                         the web UI, with its own config.xml
# The host keeps its app id (org.moonlight.host.app); only its package changes.
# The result is NOT signed: the installer signs it (Apps2Samsung does this itself;
# Tizen Studio: tizen package -t wgt -s <profile> is not used on this layout).
#
# Usage: build-hybrid.sh <host.tpk> <assembled ui dir> <out.wgt>
#   host.tpk  pkg/build-tpk.sh output; ui dir = pkg/build-package.sh's $OUT/ui
set -e
TPK=${1:?usage: build-hybrid.sh <host.tpk> <ui dir> <out.wgt>}
UI=${2:?usage: build-hybrid.sh <host.tpk> <ui dir> <out.wgt>}
OUTWGT=${3:?usage: build-hybrid.sh <host.tpk> <ui dir> <out.wgt>}
[ -f "$TPK" ] || { echo "no tpk: $TPK" >&2; exit 1; }
[ -f "$UI/config.xml" ] || { echo "no config.xml in $UI" >&2; exit 1; }
command -v zip >/dev/null || { echo "zip not found" >&2; exit 1; }

case "$OUTWGT" in /*) ;; *) OUTWGT="$(pwd)/$OUTWGT" ;; esac
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT INT TERM

# 1. the host service, as in its .tpk, without the SDK's signatures (the whole
#    .wgt is signed at install time)
unzip -q "$TPK" -d "$W/pkg"
rm -f "$W/pkg/author-signature.xml" "$W"/pkg/signature*.xml

# 2. the web UI under res/wgt/, without build/sign leftovers
mkdir -p "$W/pkg/res/wgt"
( cd "$UI" && find . -type f \
    ! -path '*/.buildResult/*' ! -name '*.wgt' ! -name 'author-signature.xml' \
    ! -name 'signature*.xml' ! -name '.manifest.tmp' ! -name '.project' ! -name '.tproject' \
    -exec sh -c 'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$W/pkg/res/wgt" {} \; )

# 3. the host joins the web package: package id (and version) from the UI's config.xml
PKG=$(sed -n 's/.*<tizen:application[^>]*package="\([^"]*\)".*/\1/p' "$UI/config.xml" | head -1)
VER=$(sed -n '/<widget/,/>/ s/.* version="\([^"]*\)".*/\1/p' "$UI/config.xml" | head -1)
[ -n "$PKG" ] && [ -n "$VER" ] || { echo "package/version not found in $UI/config.xml" >&2; exit 1; }
MAN="$W/pkg/tizen-manifest.xml"
[ -f "$MAN" ] || { echo "no tizen-manifest.xml in $TPK" >&2; exit 1; }
sed -i -e "s/\(<manifest[^>]* package=\"\)[^\"]*\"/\1$PKG\"/" \
       -e "s/\(<manifest[^>]* version=\"\)[^\"]*\"/\1$VER\"/" "$MAN"
grep -q "<manifest[^>]* package=\"$PKG\"" "$MAN" || { echo "manifest package not set" >&2; exit 1; }

# 4. root config.xml = the UI's, plus the host's privileges (the installer takes the
#    package's privileges from the root config.xml)
EXTRA=""
for p in $(sed -n 's/.*<privilege>\([^<]*\)<\/privilege>.*/\1/p' "$MAN"); do
  grep -q "name=\"$p\"" "$UI/config.xml" || EXTRA="$EXTRA  <tizen:privilege name=\"$p\"/>\\n"
done
sed "s#</widget>#$EXTRA</widget>#" "$UI/config.xml" > "$W/pkg/config.xml"

# 5. zip: the two manifests first, files only (no directory entries)
rm -f "$OUTWGT"
( cd "$W/pkg" && { echo config.xml; echo tizen-manifest.xml
    find . -type f ! -name config.xml ! -name tizen-manifest.xml | sed 's#^\./##' | sort
    find res/wgt -type f -name config.xml; } | zip -q -X -@ "$OUTWGT" )
echo "hybrid: $OUTWGT (package $PKG, version $VER, host app id kept)"
unzip -l "$OUTWGT" | grep -E ' (config\.xml|tizen-manifest\.xml|bin/MoonlightHost\.dll|res/wgt/index\.html)$|libmlengine\.so$' || true

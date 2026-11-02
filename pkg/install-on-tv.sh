#!/bin/sh
# Install a Moonlight package (.wgt or .tpk) onto a Samsung TV over the network, the
# same way the Jellyfin community installer does: run inside
# vitalets/docker-tizen-webos-sdk (headless Tizen CLI + sdb + a sample dev cert), then
# `sdb connect` + `tizen install`. Also works on any host with sdb+tizen on PATH.
#
# Usage: install-on-tv.sh <TV_IP> <package.wgt|.tpk> [cert_password]
#   TV must be in Developer Mode with this host's IP allowed (see DEV-CERT-PATH.md).
#   With [cert_password]: re-sign using /certificates/author.p12 +
#   /certificates/distributor.p12 (mount them). Needed on recent TVs that reject the
#   default Tizen cert with "Invalid certificate chain" [118,-12] — there the
#   distributor cert must be a Samsung cert bound to THIS TV's DUID.
set -e
TV_IP=${1:?usage: install-on-tv.sh <TV_IP> <package.wgt|.tpk> [cert_password]}
PKG=${2:?need a .wgt or .tpk path}
PASS=$3
HERE=$(cd "$(dirname "$0")" && pwd)
[ -f "$PKG" ] || { echo "no such package: $PKG"; exit 1; }
TYPE=${PKG##*.}
case "$TYPE" in wgt|tpk) ;; *) echo "package must be .wgt or .tpk (got .$TYPE)"; exit 1;; esac

# Optional per-TV custom certificate (Samsung, DUID-bound).
if [ -n "$PASS" ]; then
  [ -f /certificates/author.p12 ] && [ -f /certificates/distributor.p12 ] \
    || { echo "cert_password given but /certificates/{author,distributor}.p12 missing"; exit 1; }
  PROF="${HOME:-/home/developer}/tizen-studio-data/profile/profiles.xml"
  sed "s|_CERTIFICATEPASSWORD_|$PASS|g" "$HERE/profile.xml" > /tmp/prof-frag.xml
  # insert the <profile name="custom"> block before </profiles> (portable)
  awk -v f="$(cat /tmp/prof-frag.xml)" '/<\/profiles>/{print f} {print}' "$PROF" > "$PROF.new" \
    && mv "$PROF.new" "$PROF"
  echo "[install] re-signing $PKG with custom (Samsung) certificate"
  tizen package -t "$TYPE" -s custom -- "$PKG"
fi

echo "[install] sdb connect $TV_IP"
sdb connect "$TV_IP" >/dev/null
# discover the connected target id (e.g. 192.168.x.x:26101)
TARGET=$(sdb devices | awk '/device[[:space:]]*$/{print $1; exit} /\tdevice\t/{print $1; exit}')
[ -n "$TARGET" ] || TARGET=$(sdb devices | grep -v 'List of' | awk 'NF{print $1; exit}')
[ -n "$TARGET" ] || { echo "no device after sdb connect $TV_IP"; exit 1; }

echo "[install] target=$TARGET type=$TYPE pkg=$(basename "$PKG")"
tizen install -s "$TARGET" --name "$(basename "$PKG")" -- "$(cd "$(dirname "$PKG")" && pwd)"
echo "[install] done"

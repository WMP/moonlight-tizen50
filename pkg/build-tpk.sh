#!/bin/sh
# Build the no-root .NET host .tpk (dotnet-host/) around the native engine .so.
# Run inside the moonlight-tizen-build image (has .NET SDK + Tizen CLI), e.g.:
#   docker run --rm -v "$PWD:/work/repo" -v /tmp/claude-1000/engine-build:/work/eng \
#     moonlight-tizen-build sh /work/repo/pkg/build-tpk.sh /work/eng/libmlengine.so
#
# Arg 1: path to the prebuilt libmlengine.so (build-engine-so.sh output).
# tizen40 TFM builds with the OLD Tizen.NET.Sdk NuGet + a modern .NET SDK; no
# `dotnet workload install tizen` needed (nuvio tools/tpk.sh). See docs/DEV-CERT-PATH.md.
set -e
SO=${1:?usage: build-tpk.sh <path-to-libmlengine.so>}
HERE=$(cd "$(dirname "$0")" && pwd)
HOST="$HERE/dotnet-host"

[ -f "$SO" ] || { echo "engine .so not found: $SO" >&2; exit 1; }
mkdir -p "$HOST/lib"
cp "$SO" "$HOST/lib/libmlengine.so"
echo "engine: $(file "$HOST/lib/libmlengine.so" | cut -d, -f1-3)"

cd "$HOST"
dotnet build MoonlightHost.csproj -c Release -nologo \
  -p:TizenManifestFile="$HOST/tizen-manifest.xml"

TPK=$(find "$HOST/bin" -name '*.tpk' | head -1)
[ -n "$TPK" ] || { echo "no .tpk produced" >&2; exit 1; }
echo "built: $TPK"
unzip -l "$TPK" | grep -E 'libmlengine.so|MoonlightHost.dll' || true

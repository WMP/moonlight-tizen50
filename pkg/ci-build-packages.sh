#!/usr/bin/env bash
# CI build of the installable packages (release + debug .tpk/.wgt) into ./out.
# Shared by .github/workflows/build-packages.yml and dev-loop.yml; run from the
# repo root on the runner, after the workflow has set:
#   VER (file-name safe), APP_VERSION (optional), BUILD_COMMIT,
#   TV_LIBS_URL, TV_LIBS_TOKEN, GH_TOKEN, GITHUB_ACTOR, GITHUB_REPOSITORY_OWNER.
set -euo pipefail

# VER ends up in file names; a branch name like feature/x must be slugged before.
case "${VER:-}" in
  ''|*[!A-Za-z0-9._-]*) echo "::error::VER '${VER:-}' is not file-name safe (use only A-Z a-z 0-9 . _ -)"; exit 1 ;;
esac

# 1. Build image (self-contained; from ghcr.io, see build-image.yml)
IMAGE=$(sh pkg/image-ref.sh "$GITHUB_REPOSITORY_OWNER")
echo "$GH_TOKEN" | docker login ghcr.io -u "$GITHUB_ACTOR" --password-stdin || echo "login failed, will build locally"
# the tag is the hash of this checkout's Dockerfile/fetch scripts/NuGet pins;
# not published yet (build-image.yml still running) -> build it here
if ! docker pull "$IMAGE"; then
  echo "::warning::$IMAGE not on ghcr.io yet, building it locally"
  docker build -f pkg/Dockerfile -t "$IMAGE" .
fi

# 2. TV libraries (proprietary, from a private store)
mkdir -p tdeps
# TV_LIBS_URL points at the tarball in a PRIVATE store (e.g. a private
# GitHub release asset: https://api.github.com/repos/<owner>/<repo>/releases/assets/<id>).
# TV_LIBS_TOKEN authorizes the download; the content is never public.
curl -fsSL \
  -H "Authorization: Bearer $TV_LIBS_TOKEN" \
  -H "Accept: application/octet-stream" \
  "$TV_LIBS_URL" -o tv-libs.tar
tar -xf tv-libs.tar -C tdeps
test -f tdeps/tv-libs/usr/lib/libesplusplayer.so \
  || { echo "tv-libs tarball missing usr/lib/libesplusplayer.so"; exit 1; }

# 3. Packages
mkdir -p out
# --network none: the image holds every dependency; the TV libraries are
# mounted read-only where the image expects them. Nothing is downloaded.
docker run --rm --network none \
  -v "$PWD:/work/repo" \
  -v "$PWD/tdeps/tv-libs:/opt/tizen-deps/tv-libs:ro" \
  -v "$PWD/out:/work/out" \
  -e HOME=/home/developer \
  -e VER -e APP_VERSION -e BUILD_COMMIT \
  "$IMAGE" bash -euo pipefail -c '
    cd /work
    REPO=/work/repo MLT=/work/repo/third_party/moonlight-tizen
    DEPS=/opt/tizen-deps BPD=/opt/backport-deps DEPSBUILD=/work/depsbuild

    # 1-2. toolchain, sysroot and crypto/http deps are baked into the image
    # 3. core/crypto/gamestream objects + libmbedcrypto_tv.a
    MLT="$MLT" DEPS="$DEPS" BPD="$BPD" OUT="$DEPSBUILD" "$REPO/build-deps-lib.sh"

    # Steps 4-6 run twice: release (normal) and debug (trace) packages.
    for V in release debug; do
      export BUILD_VARIANT=$V
      if [ "$V" = debug ]; then SFX=-debug; else SFX=; fi
      # 4. the engine shared library
      MLT="$MLT" DEPS="$DEPS" BPD="$BPD" DEPSBUILD="$DEPSBUILD" \
        OUT=/work/engine-$V "$REPO/mlclient/build-engine-so.sh"
      # 5. the .NET host .tpk (Apps2Samsung target)
      "$REPO/pkg/build-tpk.sh" /work/engine-$V/libmlengine.so | tee /work/tpk-$V.log
      TPK=$(sed -n "s/^built: //p" /work/tpk-$V.log | tail -1)
      [ -f "$TPK" ] || { echo "no .tpk for $V"; exit 1; }
      cp "$TPK" "/work/out/org.moonlight.host-$VER$SFX.tpk"
      # 6. the web UI .wgt (needs MLT; signed by the image dev cert)
      # SIGN_PROFILE=dev: the profile built into the image; a freshly added one fails with
      # "Invaild password" (CLI 2.5.25 writes no .pwd). The native
      # tpk steps after the .wgt fail in this image ("Cannot provide a package
      # function"), so ignore its exit code and fail only if no .wgt was made.
      REPO="$REPO" MLT="$MLT" DEPS="$DEPS" BPD="$BPD" DEPSBUILD="$DEPSBUILD" \
        SIGN_PROFILE=dev "$REPO/pkg/build-package.sh" || true
      # shortcut: build-package.sh packages ui/ together with the ui/.buildResult
      # copy made by build-web, so every file is in the .wgt twice. Repackage
      # without it; remove this when the core script is fixed.
      rm -rf "$REPO"/pkg/out/ui/.buildResult "$REPO"/pkg/out/ui/*.wgt
      tizen package -t wgt -s dev -- "$REPO/pkg/out/ui"
      set -- "$REPO"/pkg/out/ui/*.wgt
      [ $# -eq 1 ] && [ -f "$1" ] || { echo "expected exactly one .wgt for $V, got: $*"; exit 1; }
      cp "$1" "/work/out/Moonlight-$VER$SFX.wgt"
    done
    ls -la /work/out
  '

#!/bin/sh
# Tag of the build image = hash of everything baked into it, so a build always
# uses the image made from its own Dockerfile/fetch scripts/NuGet versions.
cd "$(dirname "$0")/.." && cat pkg/Dockerfile .dockerignore fetch-tizen-sources.sh fetch-backport-deps.sh \
  pkg/dotnet-host/MoonlightHost.csproj | sha256sum | cut -c1-12 | sed 's/^/deps-/'

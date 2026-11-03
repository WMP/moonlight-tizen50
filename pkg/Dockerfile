# Self-contained build image for Moonlight Tizen 5.0: everything the build needs is
# baked in, so a build downloads nothing (CI runs it with --network none to prove it).
#
#   Tizen Studio CLI + sdb + a sample dev cert   (base: vitalets/tizen-webos-sdk)
#   .NET SDK 8 + the host's NuGet packages        /opt/nuget/packages
#   Tizen 5.0 cross toolchain (GCC 6.2), sysroot  /opt/tizen-deps    (fetch-tizen-sources.sh)
#   mbedTLS source, OpenSSL 1.0.2 / curl headers  /opt/backport-deps (fetch-backport-deps.sh)
#
# NOT in the image: the TV's own libraries (libesplusplayer.so and the rest of
# tv-libs/). They are proprietary and must not be redistributed: mount them at
# /opt/tizen-deps/tv-libs when you build (README "Build").
#
# Build (context = repo root):  docker build -f pkg/Dockerfile -t moonlight-tizen-build .
# Published by .github/workflows/build-image.yml as
#   ghcr.io/<owner>/moonlight-tizen-build:deps-<hash>   (hash: pkg/image-tag.sh)
FROM docker.io/vitalets/tizen-webos-sdk

RUN apt-get update && apt-get install -y --no-install-recommends \
      make cmake pkg-config file xz-utils rsync curl ca-certificates \
      git unzip zip cpio libarchive-tools \
    && rm -rf /var/lib/apt/lists/*

# .NET SDK for the .tpk host (dotnet-host/). The tizen40 TFM builds with the OLD
# Tizen.NET.Sdk NuGet package + any modern .NET SDK; no `dotnet workload install tizen`.
ENV DOTNET_ROOT=/opt/dotnet \
    PATH=/opt/dotnet:$PATH \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=1 \
    NUGET_PACKAGES=/opt/nuget/packages \
    TIZEN_DEPS=/opt/tizen-deps \
    BACKPORT_DEPS=/opt/backport-deps
RUN curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --channel 8.0 --install-dir /opt/dotnet \
    && rm /tmp/dotnet-install.sh \
    && dotnet --info | head -n 5

# Tizen 5.0 cross toolchain + sysroot + reference sources (the downloaded rpms are
# dropped once extracted).
COPY fetch-tizen-sources.sh fetch-backport-deps.sh /opt/build-scripts/
RUN /opt/build-scripts/fetch-tizen-sources.sh /opt/tizen-deps && rm -rf /opt/tizen-deps/rpm

# Crypto/http deps (mbedTLS source, OpenSSL 1.0.2 + curl 7.59 headers).
RUN OUT=/opt/backport-deps /opt/build-scripts/fetch-backport-deps.sh \
    && rm -rf /opt/backport-deps/*/.git

# The host's NuGet packages (exact versions in the .csproj, so a later restore finds
# them here and does not go to the network).
COPY pkg/dotnet-host/MoonlightHost.csproj /tmp/nuget-warm/
RUN cd /tmp/nuget-warm && mkdir -p lib && touch lib/libmlengine.so \
    && dotnet restore MoonlightHost.csproj \
    && rm -rf /tmp/nuget-warm \
    && chmod -R a+rwX /opt/nuget /opt/tizen-deps /opt/backport-deps

WORKDIR /work
CMD ["/bin/bash"]

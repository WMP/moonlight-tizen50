# Headless build+install image for Moonlight Tizen 5.0, built on the SAME community
# Tizen image the Jellyfin installer uses: vitalets/docker-tizen-webos-sdk. It already
# ships the Tizen Studio CLI (installed headless via the web-cli installer), `sdb`, and
# a sample dev author cert + profiles.xml — so we no longer install Tizen Studio
# ourselves (this replaces the old install-tizen-cli.sh / slim-tizen-cli.sh).
#
# Mounted at run time (heavy/device-specific, never committed):
#   /work/repo (this repo), /work/mlt (a moonlight-tizen checkout),
#   /work/tizen-deps (fetch-tizen-sources.sh: cross toolchain + sysroot + tv-libs)
#
# Flow:
#   - engine:   build-engine-so.sh  -> libmlengine.so (mounted arm gcc-6.2 toolchain)
#   - web .wgt: build-package.sh     -> moonlight-tizen UI, signed by the image's CLI
#   - install:  install-on-tv.sh <TV_IP> <pkg.wgt|.tpk> [cert_pw]  (sdb + tizen install)
#
# Build:  docker build -t moonlight-tizen-build pkg
FROM docker.io/vitalets/tizen-webos-sdk

# Extra build tools the core/engine scripts use (the cross toolchain itself is mounted).
RUN apt-get update && apt-get install -y --no-install-recommends \
      make cmake pkg-config file xz-utils rsync curl ca-certificates \
      git unzip cpio libarchive-tools \
    && rm -rf /var/lib/apt/lists/*

# .NET SDK for the .tpk host (dotnet-host/). The Tizen 4/5 TFM (tizen40)
# builds with the OLD Tizen.NET.Sdk delivered as a NuGet package + any modern .NET
# SDK; it needs NO `dotnet workload install tizen` (that is only for net6+/tizen7+).
# The vitalets Tizen CLI already present does the signing.
ENV DOTNET_ROOT=/opt/dotnet \
    PATH=/opt/dotnet:$PATH \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
RUN curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --channel 8.0 --install-dir /opt/dotnet \
    && rm /tmp/dotnet-install.sh \
    && dotnet --info | head -n 5

WORKDIR /work
CMD ["/bin/bash"]

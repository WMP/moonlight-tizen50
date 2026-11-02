# moonlight-tizen5.0

A **native** [Moonlight](https://moonlight-stream.org/) GameStream client for
**Samsung TVs running Tizen 5.0**, with low-latency hardware video — **no NaCl,
no WebAssembly**.

It streams games from a GameStream host (Sunshine, GeForce Experience,
[Wolf](https://games-on-whales.github.io/wolf/)) and decodes video on the TV's
hardware video plane through the vendor library `libesplusplayer.so`.

> Status: **experimental / work in progress.** The streaming engine is verified
> end-to-end on real hardware (pairing → launch → RTSP → hardware decode, 2–3
> frame player latency). The hybrid app package (UI + service) is still being
> brought up. See [BACKPORT](docs/BACKPORT.en.md) ([PL](docs/BACKPORT.md)) for the step-by-step state.

> **Where to make changes:** the engine and UI are developed in
> **[moonlight-tizen50-core](https://github.com/WMP/moonlight-tizen50-core)**. This repo
> is that upstream **plus `patches/`** (the device-integration overlay), re-assembled by
> `.github/workflows/sync-from-clean.yml`. Edit code in core; here only the overlay
> (`patches/`), the device-only files (`pkg/service/`, `pkg/roottest/`) and `docs/` are
> maintained directly. See [`AGENTS.md`](AGENTS.md).

## Install (no root required)

**You do not build anything.** CI builds the packages on every release
(`.github/workflows/build-packages.yml`); download them from the
[Releases](../../releases) page and install with
**[Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung)**.

1. On the TV: enable **Developer Mode** (Apps → `12345` → on, enter your PC's IP,
   reboot).
2. Download the package for your TV from Releases:
   - `org.moonlight.host-*.tpk` — the native engine (Tizen 4.0–5.5).
   - `Moonlight-*.wgt` — the web UI.
3. Open **Apps2Samsung**, point it at the TV. It reads the TV's **DUID**, signs
   each package for that DUID, and installs it. Packages are shipped signed only
   with a sample dev cert on purpose — Apps2Samsung re-signs per-DUID, which is
   what the TV accepts (a fixed distributor cert would lock installs to a preset
   list of TVs). No Samsung certificate to create by hand, no `sdb` commands.

The hardware video path runs from this sandboxed, dev-signed app — **root is not
required** (the esplusplayer path runs under a `User::Pkg` SMACK label). Root is
only needed for the raw-binary CLI test harness (`esppplay`, `mlclient`).

> Developer alternative (no Apps2Samsung): `pkg/install-on-tv.sh <TV_IP>
> <package> [cert_password]` does `sdb connect` + `tizen install`, re-signing with
> a Samsung DUID cert you supply (`author.p12`/`distributor.p12`). Apps2Samsung is
> the same DUID signing step, automated.

## Why this exists

[moonlight-tizen](https://github.com/brightcraft/moonlight-tizen) is a
WebAssembly app that needs **Tizen 5.5+** because it uses Samsung EMSS (elementary
media stream source) for the fast, low-latency video path. **Tizen 5.0 has no
EMSS** and the public 5.0 SDK offers only the buffering `capi-media-player` path
(the same high-latency route the old NaCl client used).

This project backports the fast path to 5.0 by reusing moonlight-tizen's
reusable parts and replacing the EMSS video/audio path with a native engine built
on `libesplusplayer.so`. That library is **internal / not in the public SDK**, so
its ABI was reverse-engineered from the device binary — see
[PROVENANCE](docs/PROVENANCE.en.md).

## What is reused vs. new

| | |
|---|---|
| **Reused unchanged** | `moonlight-common-c` (streaming core), `libgamestream` (pairing/HTTP), the web UI (host list, pairing, app grid, settings) |
| **Replaced** | EMSS video → `video_esplus.c`, EMSS/WebAudio → `audio_opus.c`, WASM embind transport → a message-port bridge, cert handling → native |
| **New** | the reverse-engineered `esplusplayer_tv50.h`, the message-port service + JS bridge, `esplus_player.c`, `input_evdev.c`, `gs_client.c` (+ own `xml_search`), the whole build/packaging |

Full map and attribution in [PROVENANCE](docs/PROVENANCE.en.md); design in
[ARCHITECTURE](docs/ARCHITECTURE.en.md).

## Architecture (hybrid package)

```
┌─────────────────────────── Tizen hybrid package ───────────────────────────┐
│  Web UI (wgt)  — moonlight-tizen front-end, unchanged logic                 │
│      │  tizen.messageport  (mlbridge.js replaces the WASM embind transport)  │
│      ▼                                                                       │
│  Native service (tpk)  — mlservice.c                                         │
│      makeCert / httpInit / openUrl / pair / startStream / sendKeyboardEvent  │
│      → moonlight-common-c + libgamestream + esplusplayer video/audio + input │
└─────────────────────────────────────────────────────────────────────────────┘
```

During streaming the native service owns the hardware video overlay; the web UI
is the menu/launcher.

## Build (headless, Docker)

Everything builds in a container — no GUI, no host Tizen install needed. Heavy or
device-specific inputs are **mounted**, never committed:

- `tizen-deps/` — cross toolchain + Tizen 5.0 sysroot/sources (`fetch-tizen-sources.sh`)
- `tv-libs/` — `.so` libraries copied from **your** TV (never redistribute these)

The build image is based on **`vitalets/docker-tizen-webos-sdk`** — the same
headless Tizen CLI image the Jellyfin community installer uses. It ships the Tizen
Studio CLI (installed non-interactively via the web-cli installer), `sdb`, and a
sample dev author cert + `profiles.xml`, so there is no GUI installer and no
Tizen-Studio install step of our own.

```bash
# 1. one-time: fetch toolchain + Tizen 5.0 sources, copy the TV's libraries
fetch-tizen-sources.sh ./tizen-deps
ssh root@<TV> 'cd / && tar cf - lib usr/lib' | tar xf - -C tizen-deps/tv-libs

# 2. build the image (vitalets base + a few build tools)
docker build -t moonlight-tizen-build pkg

# 3. build the engine .so + the web .wgt inside the container
#    (first: git submodule update --init --depth 1 third_party/moonlight-tizen)
docker run --rm -v "$PWD":/work/repo \
  moonlight-tizen-build bash -lc '
    REPO=/work/repo MLT=/work/repo/third_party/moonlight-tizen DEPS=/work/repo/tizen-deps \
      /work/repo/pkg/build-package.sh'
```

Then install on the TV exactly like Jellyfin — `sdb` + `tizen install`, in
Developer Mode:

```bash
docker run --rm --network host -v "$PWD":/work/repo moonlight-tizen-build \
  /work/repo/pkg/install-on-tv.sh <TV_IP> <package.wgt|.tpk> [cert_password]
```

> **Packaging status.** The web `.wgt` builds, signs and installs headless on the
> vitalets image (the Tizen-Studio-install problem is gone). The native engine ships
> as `libmlengine.so` driven by a thin exe (root path) or a **.NET host `.tpk`**
> (no-root dev-cert path — see `docs/DEV-CERT-PATH.md`). The `.tpk` uses the `tizen40`
> TFM, built with a plain .NET SDK + the `Tizen.NET.Sdk` NuGet (NO `dotnet workload
> install tizen`); the vitalets Tizen CLI does the packaging/signing. End users
> install with Apps2Samsung (per-DUID signing); developers can use `install-on-tv.sh`
> with a Samsung DUID cert, since recent TVs reject the default Tizen cert
> ("Invalid certificate chain").

## Releasing

CI (`.github/workflows/build-packages.yml`) builds the `.tpk` + `.wgt` and attaches
them to a GitHub Release.

1. One-time: host the TV libraries **privately** (they must never be public) and give
   CI read-only access:
   - create a **private** repo, e.g. `WMP/tv-libs`, and upload the tarball as a release
     asset — `gh release create v1 tv-libs.tar -R WMP/tv-libs` (the tar must contain
     `tv-libs/usr/lib/libesplusplayer.so` and the other TV `.so` files);
   - get the asset's API URL:
     `gh api repos/WMP/tv-libs/releases/tags/v1 --jq '.assets[0].url'`;
   - create a **fine-grained PAT** scoped to *Contents: read* on `WMP/tv-libs` only;
   - add two repo secrets here (*Settings → Secrets and variables → Actions*):
     **`TV_LIBS_URL`** = that asset API URL, **`TV_LIBS_TOKEN`** = the PAT.
   CI downloads the tar with the token in an `Authorization` header, so the URL alone
   is useless to anyone else. Without both secrets the native build is skipped (the run
   stays green but produces no packages).
2. The `third_party/moonlight-tizen` submodule must resolve (CI checks it out with
   `submodules: recursive`).
3. Push a tag `vX.Y.Z` (or run it from *Actions → Run workflow*). CI builds in the
   Docker image and uploads `org.moonlight.host-*.tpk` + `Moonlight-*.wgt` to the Release.
4. Users install the released packages with Apps2Samsung (per-DUID signing).

For the sync workflow to open PRs, enable *Settings → Actions → General → Allow GitHub
Actions to create and approve pull requests*.

## Requirements

- Samsung TV, **Tizen 5.0**, Muse.M, armv7l.
- A GameStream host on the LAN (Sunshine / GeForce Experience / Wolf).
- To build: Linux with `podman`, and the TV's `/lib` + `/usr/lib` (copied once).

## Repository layout

```
esplusplayer_tv50.h   reverse-engineered ABI of the TV's libesplusplayer.so
mlclient/             native engine + CLI harness (video, audio, input, pairing)
pkg/                  hybrid package: Dockerfile, service (tpk), ui (wgt), build
tests/                esppplay probe + host self-tests used during bring-up
docs/                 BACKPORT / PROVENANCE / ARCHITECTURE / DEV-CERT (EN + PL)
third_party/          moonlight-tizen (web UI + moonlight-common-c), git submodule
fetch-*.sh build-*.sh reproducible build scripts
```

## Credits & license

This project stands on several others. What came from where:

| Project | What we took from it |
|---|---|
| [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) (moonlight-stream) | The streaming core — RTSP, control stream, FEC, the decoder/audio callback ABI. Used unchanged. |
| [moonlight-embedded](https://github.com/moonlight-embedded/moonlight-embedded) (irtimmer) | `libgamestream` (pairing + GameStream HTTP) and its `client.c` as the reference for our native pairing/launch orchestration (`gs_client.c`). |
| [moonlight-tizen](https://github.com/brightcraft/moonlight-tizen) (brightcraft) | The web UI — host list, pairing, app grid, settings — reused as-is, with the WASM/EMSS video transport swapped for our message-port bridge. The WASM port we backport the fast path *from*. |
| [moonlight-chrome](https://github.com/moonlight-stream/moonlight-chrome) → [moonlight-tizen-nacl](https://github.com/OneLiberty/moonlight-tizen-nacl) (OneLiberty) | The original NaCl Tizen lineage this repo forked; the pre-5.5 UI and gamepad fixes. |
| [nuvio-native-legacy](https://github.com/iqui27/nuvio-native-legacy) (iqui27) | The **native `.tpk` design for Tizen 4/5**: loading the engine `.so` from memory via `memfd` (UEP blocks file `dlopen`), the `tizen40`/`TVGLApplication` host shape, **PUBLIC / no-`drminfo`** signing, per-DUID install, and the self-update-by-downloaded-`.so` idea. We aligned our host/engine to it; no code copied. |
| [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung) | The end-user install tool — reads the TV DUID, signs each package for it, installs. |
| [vitalets/docker-tizen-webos-sdk](https://github.com/vitalets/docker-tizen-webos-sdk) + the [Jellyfin](https://github.com/jellyfin/jellyfin-tizen) / [georift](https://github.com/Georift/install-jellyfin-tizen) install flow | The headless build+install toolchain (Tizen CLI + `sdb`), the pattern our Docker image and `install-on-tv.sh` follow. |

**New / our own:** the reverse-engineered `libesplusplayer.so` ABI
(`esplusplayer_tv50.h`), the native engine (`esplus_player.c`, `video_esplus.c`,
`audio_opus.c`, `input_evdev.c`), the message-port bridge, and the build/packaging.
Full file-level map and attribution in [PROVENANCE](docs/PROVENANCE.en.md).

**License:** `moonlight-common-c` and `libgamestream` are GPLv3, so this project is
**GPLv3**.

Samsung's TV libraries (`libesplusplayer.so` etc.) are **not** included and must not
be redistributed; they are used only locally, linked against copies from your own
device.

## Disclaimer

Independent, unofficial project. Reverse-engineering notes are engineering aids;
items marked "assumed" need verification on the target hardware before relying on
them. Not affiliated with or endorsed by Samsung, NVIDIA, or the Moonlight project.

# moonlight-tizen50-core

A **native** [Moonlight](https://moonlight-stream.org/) GameStream client for
**Samsung TVs running Tizen 5.0**, with low-latency hardware video.

It streams games from a GameStream host (Sunshine, GeForce Experience,
[Wolf](https://games-on-whales.github.io/wolf/)) and decodes video on the TV's
hardware video plane through the vendor library `libesplusplayer.so`.

> Status: **experimental / work in progress.** The streaming engine runs
> end-to-end on real hardware (pairing → launch → RTSP → hardware decode, 2–3
> frame player latency). The app package (UI + engine host) is still being
> brought up.

## Why this exists

The WebAssembly [moonlight-tizen](https://github.com/brightcraft/moonlight-tizen)
app needs **Tizen 5.5+** because it uses Samsung EMSS (elementary media stream
source) for the fast, low-latency video path. **Tizen 5.0 has no EMSS**, and the
public 5.0 SDK offers only the buffering `capi-media-player` path (high latency).

This project brings the fast path to 5.0 by reusing moonlight-tizen's portable
parts and replacing the EMSS video/audio path with a native engine built on
`libesplusplayer.so`, the TV's hardware ES player.

## What is reused vs. new

| | |
|---|---|
| **Reused unchanged** | `moonlight-common-c` (streaming core), `libgamestream` (pairing/HTTP), the web UI (host list, pairing, app grid, settings) |
| **Replaced** | EMSS video → `video_esplus.c`, EMSS/WebAudio → `audio_opus.c`, the WASM transport → a message-port bridge, cert handling → native |
| **New** | `esplusplayer_tv50.h` (the TV's ES-player C API), the message-port bridge + JS shim, `esplus_player.c`, `input_evdev.c`, `gs_client.c` (+ own `xml_search`), and the build/packaging |

## Architecture

```
┌──────────────────────── Tizen app ────────────────────────┐
│  Web UI (.wgt)  — moonlight-tizen front-end                │
│      │  tizen.messageport  (mlbridge.js)                    │
│      ▼                                                      │
│  .NET host (.tpk)  — loads libmlengine.so (the engine)     │
│      makeCert / pair / startStream / input                 │
│      → moonlight-common-c + libgamestream + esplusplayer   │
└────────────────────────────────────────────────────────────┘
```

During streaming the engine owns the hardware video overlay; the web UI is the
menu/launcher.

## Build (headless, Docker)

Everything builds in a container. Heavy or device-specific inputs are
**mounted**, never committed:

- `tizen-deps/` — cross toolchain + Tizen 5.0 sysroot/sources (`fetch-tizen-sources.sh`)
- `tv-libs/` — `.so` libraries copied from **your** TV (never redistribute these)

The build image is based on **`vitalets/docker-tizen-webos-sdk`** — the same
headless Tizen CLI image the Jellyfin community installer uses (Tizen Studio CLI,
`sdb`, a sample dev cert) plus a .NET SDK for the `.tpk` host.

```bash
# 1. one-time: fetch toolchain + Tizen 5.0 sources
fetch-tizen-sources.sh ./tizen-deps
# then provide your TV's /lib and /usr/lib under tizen-deps/tv-libs/
# (your device's own files — used only locally, never redistributed)

# 2. build the image
docker build -t moonlight-tizen-build pkg

# 3. build the engine .so, the .NET host .tpk, and the web .wgt in the container
docker run --rm -v "$PWD":/work/repo -v <moonlight-tizen>:/work/mlt \
  moonlight-tizen-build bash -lc '
    REPO=/work/repo MLT=/work/mlt DEPS=/work/repo/tizen-deps
    mlclient/build-engine-so.sh                 # -> libmlengine.so
    pkg/build-tpk.sh /work/engine/libmlengine.so # -> the .NET host .tpk
    REPO=$REPO MLT=$MLT pkg/build-package.sh     # -> the web .wgt'
```

## Install (no build needed)

CI builds the packages on every release
(`.github/workflows/build-packages.yml`); download them from the
[Releases](../../releases) page and install with
**[Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung)**.

1. On the TV: enable **Developer Mode** (Apps → `12345` → on, enter your PC's IP,
   reboot).
2. Download the package for your TV from Releases:
   - `org.moonlight.host-*.tpk` — the native engine host (Tizen 4.0–5.5).
   - `Moonlight-*.wgt` — the web UI.
3. Open **Apps2Samsung**, point it at the TV. It reads the TV's **DUID**, signs
   each package for that DUID, and installs it. Packages ship signed only with a
   sample dev cert; Apps2Samsung re-signs per-DUID, which is what the TV accepts.
   No Samsung certificate to create by hand, no `sdb` commands.

> Developer alternative: `pkg/install-on-tv.sh <TV_IP> <package> [cert_password]`
> does `sdb connect` + `tizen install`, re-signing with a Samsung DUID-bound cert
> you supply. Apps2Samsung is the same step, automated.

## Requirements

- Samsung TV, **Tizen 5.0**, Muse.M, armv7l.
- A GameStream host on the LAN (Sunshine / GeForce Experience / Wolf).
- To build: Linux with Docker/podman, and the TV's `/lib` + `/usr/lib` (copied once).

## Repository layout

```
esplusplayer_tv50.h   C API for the TV's libesplusplayer.so
mlclient/             native engine + CLI harness (video, audio, input, pairing)
pkg/                  app package: Dockerfile, .NET host (tpk), web UI (wgt), build
fetch-*.sh build-*.sh reproducible build scripts
esppplay.c            minimal H.264 → esplusplayer probe used during bring-up
```

## Credits & license

This project stands on several others:

| Project | What we use |
|---|---|
| [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) | The streaming core — RTSP, control stream, FEC, the decoder/audio callback ABI. Used unchanged. |
| [moonlight-embedded](https://github.com/moonlight-embedded/moonlight-embedded) | `libgamestream` (pairing + GameStream HTTP), and its `client.c` as the reference for our native pairing/launch (`gs_client.c`). |
| [moonlight-tizen](https://github.com/brightcraft/moonlight-tizen) | The web UI — host list, pairing, app grid, settings — reused as-is, with the WASM transport swapped for our message-port bridge. |
| [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung) | The end-user install tool — reads the TV DUID, signs each package for it, installs. |
| [vitalets/docker-tizen-webos-sdk](https://github.com/vitalets/docker-tizen-webos-sdk) + the [Jellyfin](https://github.com/jellyfin/jellyfin-tizen) install flow | The headless build+install toolchain our Docker image and `install-on-tv.sh` follow. |

**License:** `moonlight-common-c` and `libgamestream` are GPLv3, so this project is
**GPLv3**.

Samsung's TV libraries (`libesplusplayer.so` etc.) are **not** included and must not
be redistributed; they are used only locally, linked against copies from your own
device.

## Disclaimer

Independent, unofficial project. Not affiliated with or endorsed by Samsung,
NVIDIA, or the Moonlight project.

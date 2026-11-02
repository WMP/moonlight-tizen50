# Backport moonlight-tizen → native Tizen 5.0 — status

*(Polish step-by-step log: [BACKPORT.md](BACKPORT.md). Code map: [PROVENANCE.en.md](PROVENANCE.en.md).)*

Goal: a fast native Moonlight client on Tizen 5.0 (armv7l softfp, glibc 2.24,
GCC 6.2), video via `libesplusplayer.so`, ~2–3 frame latency. Source:
brightcraft/moonlight-tizen (WASM/EMSS).

## What is shared vs. rewritten

Reused unchanged: `moonlight-common-c` (core), `libgamestream` (pairing/HTTP),
`h264bitstream`, `opus`, `enet`, and the web UI. Only the ~6 platform-shim files
(video, audio, input, transport/cert) are rewritten. See PROVENANCE.en.md.

Tizen 5.0 libraries (checked in tv-libs): `libcrypto.so.1.0.0`, `libssl`,
`libcurl.so.4.5.0`, PulseAudio, ALSA, libevdev, Ecore_Wl2, GLES/EGL,
`libesplusplayer.so`. Missing: libopus, libenet (both vendored). No dev headers
in the sysroot for openssl/curl/mbedtls — supplied by fetch-backport-deps.sh.

## Milestones (verified unless noted)

- **M1.1 core builds** — `build-core.sh`: moonlight-common-c + enet + reedsolomon,
  27/27 with GCC 6.2 on the sysroot, 0 errors. Only two trivial Emscripten shims
  in `Platform.h`. Biggest risk (does GCC 6.2 build the core) retired.
- **M1.2 deps link** — mbedTLS 2.28.8 (96/96) for PlatformCrypto; libgamestream
  on OpenSSL 1.0.2o + curl 7.59 headers; whole chain links to an ARM ELF against
  the real tv-libs, no glibc symbol > 2.24.
- **M1.3 video sink** — `video_esplus.c`: `DECODER_RENDERER_CALLBACKS` over
  esplusplayer; whole video path links. 0 warnings.
- **M1.3b cert + orchestration** — `cert_init.c` (qemu-tested X.509 gen/load);
  `gs_client.c` ports the GameStream serverinfo→pair→launch→RTSP flow from
  moonlight-embedded, with an own dependency-free `xml_search` (unit-tested).
- **M2 audio** — esplusplayer audio ABI reverse-engineered (24 B struct, **OPUS=8**,
  not the public 7). `esplus_player.c` shares one player for video+audio (one
  clock); `audio_opus.c` submits audio/x-opus with an RFC 7845 OpusHead.
- **M3 input** — `input_evdev.c`: `/dev/input` → LiSend* (keyboard/mouse/gamepad).

## End-to-end test (verified on TV + a Wolf host)

Against host `192.168.123.3` = **Wolf** (games-on-whales) over GameStream: the
native client paired (PIN via Wolf's /pin API), launched an app, completed the
RTSP handshake, set up the video and audio sinks, enumerated input (the TV remote
IS on evdev), and grabbed the hardware overlay. The stream ended on the **host**
side (a Wolf config error: the app runner's Wayland socket), not the client.
Decode genuinely works (dmesg: first key frame + first set-display). Fixes landed:
clean overlay release on connection-terminated; pairing persistence (detect via
HTTPS serverinfo + stored server cert).

## No-root feasibility (verified)

SMACK labels: `libesplusplayer.so` = `_`, `/dev/video*`/`/dev/sdp*`/`/dev/input/*`
= `*`. Running the probe under a sandbox `User::Pkg` label decoded video — so a
dev-signed `.tpk` can drive esplusplayer **without root**.

## Hybrid package (in progress)

Reuse moonlight-tizen's web UI + our native engine, wired by a Tizen message-port
bridge. Done: native service + bridge (`mlservice.c`, builds/links), JS transport
shim (`mlbridge.js`), UI assembly, headless Docker (engine builds in-container, web
`.wgt` signs). Remaining: native `.tpk` packaging (needs the Tizen native packaging
component; the GUI installer can't run headless), hybrid combine, install, and
on-device validation of the message-port round-trips + streaming.

## Open items to confirm on a real stream

`opensslconf.h` correctness / the TV's `OPENSSL_NO_*` flags; rikey/pairing against
GFE; an EMSS `kUltraLow` equivalent (no `set_low_latency_mode` export — look in
buffer/display settings); remote routing (evdev vs CEC); esplusplayer accepting the
OpusHead as codec_data; the two-track prepare sequence.

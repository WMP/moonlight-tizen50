# Code provenance and reverse-engineering results

*(Polish version: [PROVENANCE.md](PROVENANCE.md))*

A **native** Moonlight port for **Samsung TV Tizen 5.0** (armv7l softfp, glibc 2.24,
GCC 6.2), with no NaCl and no WASM. It combines the reusable parts of
**moonlight-tizen** (brightcraft) with a new native engine built on the TV's
internal library `libesplusplayer.so`.

Why a separate port: moonlight-tizen is WASM/Emscripten and uses Samsung EMSS,
which exists only from Tizen 5.5. Tizen 5.0 has no EMSS and no public API for the
low-latency elementary-stream path — the only route is native esplusplayer, whose
ABI had to be read from the machine code.

Legend: **[UP]** taken from moonlight-tizen unchanged · **[MOD]** replaced ·
**[NEW]** new (no equivalent/API) · **[RE]** reverse-engineering result.

---

## 1. Taken from upstream unchanged — [UP]

| Component | Origin | How used |
|---|---|---|
| `moonlight-common-c` (Connection, RTSP, Control/Video/Audio stream, depacketizer, ENet, Reed-Solomon) | moonlight-tizen submodule (upstream) | compiled natively with GCC 6.2; 27/27 files, 0 errors |
| `libgamestream` — `http.c`, `pairing.c`, `mkcert.c` | moonlight-tizen | compiled against OpenSSL 1.0.2o headers (matching the TV) |
| `h264bitstream`, `opus`, `enet` | vendored in moonlight-tizen | available to link (absent on the TV) |
| **Web UI** — `wasm/platform/*.js`, `index.html`, `static/`, `res/` | moonlight-tizen | packaged into the wgt **with unchanged logic** (host list, pairing, app grid, box art, settings) |

The only UI change: remove the WASM module loader (`platform.js`) and inject
`mlbridge.js` — see section 3.

---

## 2. What had to be replaced — [MOD]

| Original (moonlight-tizen) | Our replacement | Why |
|---|---|---|
| `wasm/wasmplayer.cpp` (EMSS `ElementaryMediaTrack.AppendPacket`) | `mlclient/video_esplus.c` | no EMSS on 5.0 → esplusplayer |
| `wasm/auddec.cpp` (EMSS audio / WebAudio) | `mlclient/audio_opus.c` | same; audio/x-opus path into esplusplayer |
| `Module.*` + `sendMessage`/`handleMessage` transport (embind) | `pkg/ui/mlbridge.js` (message-port client) | no WASM/embind on 5.0 |
| `http.cpp` MakeCert/LoadCert/HttpInit (embind) | `cert_init.c` (CLI) / cert handling in `mlservice.c` (service) | native instead of embind |
| `Platform.h` — unconditional `#include <emscripten.h>` and `Limelog`→`emscripten_log` | two small shims (`emscripten.h` shim) | the core calls no emscripten API; shims only to compile |
| GameStream orchestration done in JS via WASM (serverinfo/pair/launch) | `gs_client.c` (CLI variant) or JS-driven over the bridge (hybrid) | on 5.0 HTTP/crypto must run natively |

---

## 3. New code — no equivalent or no API — [NEW]

| File | What | Why new |
|---|---|---|
| `esplusplayer_tv50.h` | ABI header of the TV's old esplusplayer | **no public API** — see section 5 |
| `pkg/service/mlservice.c` | Tizen service-app + **message-port bridge** dispatching the 11 UI functions to the engine | upstream UI calls WASM via embind; natively **no such method exists** — a wgt↔service IPC is required |
| `pkg/ui/mlbridge.js` | `tizen.messageport` client replacing the WASM transport | same, on the JS side |
| `mlclient/esplus_player.c` | one shared esplusplayer for video+audio (TV allows one per process) | a coordination layer not present in EMSS |
| `mlclient/input_evdev.c` | input via `/dev/input` → `LiSend*` | native equivalent of HTML5 input |
| `mlclient/gs_client.c` | native orchestration + **own `xml_search`/`xml_status`** | moonlight-tizen references `xml_search` but **ships no `xml.c`** (the JS did it); written without an expat dependency |
| `mlclient/main.c` | native CLI client main | ties it together outside the browser |
| Build/packaging: `fetch-tizen-sources.sh`, `fetch-backport-deps.sh`, `build-core.sh`, `build-deps-lib.sh`, `build-client.sh`, `build-video-path.sh`, `pkg/Dockerfile`, `pkg/build-package.sh`, `pkg/service/build-service.sh`, `service/tizen-manifest.xml`, `ui/config.xml` | environment + hybrid package | new toolchain for 5.0 + the .tpk/.wgt package |

Bridge contract (1:1 with moonlight-tizen's embind exports):
`makeCert, httpInit, openUrl, pair, startStream, stopStream, cancelRequest,
sendKeyboardEvent, stun, wakeOnLan, toggleStats` — details in `pkg/ARCHITECTURE.md`.

---

## 4. Crypto/HTTP — supplied dependencies

The public 5.0 sysroot has no headers; versions chosen to match the TV libraries:
- **mbedTLS 2.28.8** (vendored source) — for the core's `PlatformCrypto.c`.
- **OpenSSL 1.0.2o** + **libcurl 7.59.0** — headers fetched, linked against the TV `.so`.
Confirmed: the whole chain links to an ARM ELF, no glibc symbol above 2.24.

---

## 5. Reverse-engineering results for `libesplusplayer.so` — [RE]

Public sources (esplusplayer 0.0.3 from Tizen 6.5) have the **new** API; the TV has
the **old** one. The following was read from the machine code (objdump);
**ASM** = confirmed in assembly, **TV** = confirmed by a run on the device.

Calling conventions:
- Functions return a **bool** (1/0), not a Tizen error code. [ASM]
- `submit_packet` returns a status where **4 = success**. [ASM, TV]
- `pts`/`duration` are in **nanoseconds** — the library divides by 1,000,000
  (constant `0xF4240` in the assembly). [ASM]
- Callbacks have **no `user_data`**; offsets in a global `listener_bridge`
  (error +4, buffer_status +8, resource_conflicted +12, eos +16,
  ready_to_prepare +20, prepare_async_done +24). [ASM]
- **No `esplusplayer_destroy` export** (though the public headers have it). [ASM]

Structures:
- `video_stream_info` = **36 B**: codec_data@0, codec_data_length@4, mime@8,
  width@12, height@16, max_width@20, max_height@24, framerate_num@28,
  framerate_den@32. Offsets confirmed by the dlog strings ("from->width" etc.). [ASM]
- `es_packet`: type@0, buffer@4, size@8, pts@16, duration@24. [ASM]
- `audio_stream_info` = **24 B**: codec_data@0, len@4, mime@8, bitrate@12,
  channels@16, sample_rate@20. Offsets from the log strings. [ASM]

Enums (read from the jump tables in `SetMimeType_`):
- Video: H263=1, **H264=2**, HEVC=3, MPEG1/2/4=4/5/6, VP8=7, VP9=8, WMV3=9
  (no AV1/MJPEG). [ASM]
- Audio: AAC=1, MP2=2, MP3=3, AC3=4, EAC3=5, VORBIS=6, **DTS=7, OPUS=8**.
  **Note: differs from the public enum** — the TV inserts DTS=7, so OPUS=8 (not 7).
  Taking the public value would select the wrong codec. [ASM]
- `DisplayType`: NONE=0, **OVERLAY=1**, EVAS=2. Any other value → `assert_fail`
  (abort). The video window is `_Ecore_Wl2_Window*`. [ASM]

Behaviour (on the TV):
- Prepare sequence: `ready_to_prepare` → feed ~9–10 packets → `prepare_async_done`. [TV]
- Resource-conflict model: acquiring the video plane **stops the broadcast decoder**
  (`sdp-dvde-dec0`); esplusplayer uses a different decoder (`sdp-mfc-dec0`) on the
  same plane. Releasing it does not restart broadcast — only the TV shell does. [TV]
- `[sdp-mfc-dec0] ERROR invalid user data a20003` is **harmless** (decoding
  proceeds). The `GStreamer-CRITICAL` (caps) errors come from esplusplayer's own
  pipeline — they appear even for pure video with no audio, so **not from our
  OpusHead**. [TV]

No-root access (SMACK/Cynara):
- `libesplusplayer.so` has SMACK label `_` (loadable by anyone); `/dev/video*`,
  `/dev/sdp*`, `/dev/input/event*` have label `*` (accessible to any process).
  esplusplayer does not link Cynara. [TV]
- Test: `esppplay` run under a sandbox label `User::Pkg::...` **decoded video**
  (create/open/overlay/prepare/submit OK) — installation via a dev package without
  root is feasible. [TV]

---

## 6. Measured results [TV]

- In-player latency: **2–3 frames** (34–51 ms queue at 1080p60), stable; a small
  preroll-pts step brings it down from a constant ~150 ms.
- End-to-end test against a **Wolf** host (games-on-whales) over GameStream:
  pairing, app launch, RTSP, stream init, video/audio setup, input enumeration
  (remote on evdev) — everything on the client side worked. The stream ended on
  the host (a Wolf config error: the runner's Wayland socket), not the client.
- esplusplayer accepts raw **H.264 Annex-B** (no AVCC/extradata). [TV]

---

## 7. Method

RE tooling: `arm-linux-gnueabi-objdump`/`nm`/`readelf` from the Tizen 6.2
toolchain, jump-table analysis (`tbb`/`tbh`), PC-relative string resolution,
correlation with `dlog` log strings (field names) and the public 0.0.3 headers
(names, not values). Every result is marked confirmed-in-asm, confirmed-on-TV, or
assumed.

> Engineering aid. Before any external use, the items marked "assumed" need
> verification on the target hardware.

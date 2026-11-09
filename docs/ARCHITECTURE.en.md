# Hybrid Moonlight package for Tizen 5.0 (moonlight-tizen UI + native engine)

*(Polish version: [ARCHITECTURE.md](ARCHITECTURE.md))*

Goal: the **moonlight-tizen web UI** appears in the TV app bar, while streaming is
done by **our native engine** through `libesplusplayer.so` (EMSS exists only from
5.5). No NaCl, no WASM.

## Tizen hybrid package

One package contains two applications (a Tizen "hybrid app"):
- **UI = wgt** (web): `wasm/platform/*.js` + `index.html` + `res/` from
  moonlight-tizen, with unchanged logic. Only the `sendMessage`/`handleMessage`
  transport layer is swapped.
- **Service = tpk** (native, service-application): our engine + the bridge server.

## UI ↔ service contract (11 functions, 1:1 with moonlight-tizen's embind exports)

JS calls them via `sendMessage(name, args)`; results return via `handleMessage`.
In the original they went to the WASM module; here they go over the bridge to the
native service.

| Function | Native equivalent (already have) |
|---|---|
| `makeCert` | mkcert / cert_init.c |
| `httpInit(cert,key,uniqueId)` | ml_cert_init (cert_init.c) |
| `openUrl(url,ppk,bin)` | http_request (libgamestream http.c) |
| `pair(major,addr,port,pin)` | gs_pair (libgamestream pairing.c) |
| `startStream([host,port,w,h,fps,bitrate,rikey,rikeyid,appVer,gfeVer,sessionUrl0,codecMode,...])` | LiStartConnection + video_esplus + audio_opus (esplus_player) |
| `stopStream` | LiStopConnection + esplus_close |
| `cancelRequest` | http_cancel_request |
| `sendKeyboardEvent(key,action,mods)` | LiSendKeyboardEvent |
| `stun` | LiFindExternalAddressIP4 |
| `wakeOnLan` | magic packet |
| `toggleStats` | local |

Note: in the hybrid, JS does pairing/launch by calling `openUrl`/`pair`, then
`startStream` with a ready `sessionUrl0` and `rikey` (as in the original — see
index.js `sendMessage('startRequest', [...])`). So our gs_client.c orchestration
(serverinfo→pair→launch) is here **split into `openUrl`/`pair` calls** driven by
JS — or we keep gs_client whole and add a single `connect(host,appid,pin)`.
Decided at bridge-implementation time.

## Bridge: Tizen message-port (recommended)

- JS: `tizen.messageport` (local message port); native: C `message_port` API.
- No network/ports; native wgt↔tpk IPC within one package.
- Large data (serverinfo XML, applist, box art) as byte messages.
- Fallback: a localhost WebSocket (service = WS server, UI = client).

## Video + input during streaming

- The service renders via esplusplayer to the hardware overlay plane. The web UI
  above it shows a transparent page during streaming so the overlay is visible.
- Input: JS forwards keys via `sendKeyboardEvent` (as the original); evdev
  (input_evdev.c) is an altersupplement (gamepad).
- Exiting the stream via the shell (Back/Home) → the shell restores the source
  (this resolves the "black screen" seen in CLI tests).

## What the Docker builds (pkg/Dockerfile + build-package.sh)

1. Native service: fetch-tizen-sources + fetch-backport-deps + build the engine
   (reuses build-deps-lib.sh/build-client.sh) → service ELF + bridge.
2. UI: copies `wasm/platform` + `index.html` + `res`, swaps the
   sendMessage/handleMessage transport for the bridge client.
3. Assembles the hybrid package (wgt UI + tpk service), signs with the `Moonlight`
   profile (Tizen CLI + cert, mounted from the host or generated in-container).

Mounted inputs (not in the repo): `tizen-deps/` (toolchain+sources), `tv-libs/`
(the TV's `.so`), the Tizen CLI, the signing cert.

## Status

See the Polish `ARCHITECTURE.md` and `../BACKPORT.en.md` for the step-by-step
state. The engine core (cert/http/pair/video/audio/input) works natively and is
tested end-to-end against a host.

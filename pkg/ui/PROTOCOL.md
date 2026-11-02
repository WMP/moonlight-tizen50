# UI ↔ host message-port protocol

The web UI (`Moonlight5.ui`, `pkg/ui/mlbridge.js`) talks to the .NET host
(`org.moonlight.host.app`, `pkg/dotnet-host/Program.cs`, a service app) over Tizen
message ports. The host calls the engine (`libmlengine.so`, `mlclient/mlengine.h`).

| Direction | App id | Port | Trusted |
|-----------|--------|------|---------|
| UI → host (commands) | `org.moonlight.host.app` | `moonlight` | no |
| host → UI (replies, events) | `Moonlight5.ui` | `moonlightui` | no |
| UI → host (debug console relay) | `org.moonlight.host.app` | `mllog` | no |

When the `moonlight` port is missing, the UI starts the host with
`tizen.application.launchAppControl` (operation `default`, explicit app id; fallback
`tizen.application.launch`) and retries with backoff; it asks again every 8 missed
connects. Commands sent before the port exists are queued. A command's timeout starts
when it is sent, not when it is queued.

## Message shape

Every message is a list of `{key, value}` (a Bundle on the host side). All values
are strings, except the binary `data` of an `openUrl` reply (bytes).

- Command: `cmd`, `id` (decimal string, unique per UI session), plus the fields below.
- Reply: `id`, plus either the result fields or `error` (string). One reply per command.
- Event: `event` (string, no `id`). The UI passes it to `handleMessage(event)` of
  moonlight-tizen's `messages.js`, which expects the same strings as the WASM build,
  for example `streamTerminated: 0` or `Connection Established`.

### Large messages (parts)

A Tizen message port rejects a message over about 4 KB. A message whose fields
together exceed 1536 bytes is sent as ordered parts, in both directions:

- each part: `id` (and `cmd`, UI → host), `seq` (0-based), `total`, `field` (the field
  name), `chunk` (up to 512 characters of a string, or up to 1536 bytes of binary data);
- the receiver collects the parts of an `id` (duplicates are ignored), joins the
  chunks of each field in `seq` order, and then handles the joined message as usual;
- small messages stay one message, without `seq`/`total`.

This covers `openUrl` replies (serverinfo, applist, box art) and `httpInit` (cert + key).

The host runs `openUrl`, `pair`, `makeCert` and `httpInit` one at a time (libgamestream
uses one global cancel flag). `cancelRequest` does not wait for that queue.

## Commands

| `cmd` | Fields | Reply on success | Engine call |
|-------|--------|------------------|-------------|
| `makeCert` | – | `cert`, `key` (PEM) | `ml_engine_make_cert` |
| `httpInit` | `cert`, `key`, `uniqueId` | – | `ml_engine_http_init` |
| `openUrl` | `url`, `ppk` (server cert PEM, may be absent), `binary` (`true`/`false`) | `data` (string, or bytes when `binary=true`) | `ml_engine_open_url` |
| `pair` | `major`, `addr`, `port`, `pin` | `serverCert` (PEM) | `ml_engine_pair` |
| `cancelRequest` | – | – | `ml_engine_cancel_request` |
| `startStream` | `host`, `w`, `h`, `fps`, `bitrate`, `rikey` (hex), `rikeyid`, `appVer`, `gfeVer`, `rtspUrl`, `codecMode`, `videoCodec` (`H264`/`HEVC`/`AV1`), `hdr` (`true`/`false`), `audioConfig` (`Stereo`/`51Surround`/`71Surround`) | – | `ml_engine_start_session` |
| `stopStream` | – | – | `ml_engine_stop` |
| `sendKeyboardEvent` | `key`, `action`, `mods` | – | `ml_engine_send_key` |
| `toggleStats` | – | – | none (not supported yet) |
| `stun` | – | `result` = `""` | none (not supported yet) |
| `wakeOnLan` | `mac` | `error` = `not supported` | none (not supported yet) |

An engine error becomes `error` = the engine return code as a decimal string (the
same values the WASM build rejects with, for example `-1` = GS_FAILED).

## Events

| `event` | When |
|---------|------|
| `Connection Established` | `startStream` succeeded (`LiStartConnection` returned 0) |
| `streamTerminated: <code>` | a started session ended (`ml_engine_running()` went 1 → 0): `0` after `stopStream`; `-1` when `startStream` failed |

## Timeouts (UI side)

Each command rejects after 15 s with `timeout`. `openUrl` uses its own timeout
argument when it has one, and then also sends `cancelRequest`. `pair` waits up to
120 s, because the user types the PIN on the PC.

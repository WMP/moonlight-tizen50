# Audio path failure on Tizen 5.0 esplusplayer — `prepare_async_done ok=0`, error `0xfe6c1008`

Investigation date: 2026-11-06. Symptom sessions: 21:21, 21:36, 21:47 (TV console logs
`tv-console-Moonlight5.ui-20261106-*.txt` + Wolf logs `wolf-last-minute-*.log`).

Verdict markers: **[F]** = fact, verified in decompiled TV libraries or logs;
**[P]** = hypothesis/probable. Decompiled sources live in `tv-recon/game-mode-re/decompiled/`
(`libesplusplayer.so.c`, `libtrackrenderer.so.c`, `libcapi-media-epplayer.so.0.4.0.c`,
`libtrackrenderer.so.static-init.c`), taken from this exact TV (QE65Q70R, Tizen 5.0,
model group 19_MUSEM_QTV). SHA-256 manifests in `game-mode-re/decompiled/SHA256SUMS`.

---

## 1. Symptom

After `/launch` or `/resume` the TV shows one video frame and never updates it. Wolf keeps
streaming and receives gamepad input. Engine log excerpt (session 21:47):

```
esplus_prepare: waiting for the other sink
esplusplayer_add_audio_stream_info opus -> 1
esplus_prepare prepare_async -> 1
cb ready_to_prepare stream=1 ; cb ready_to_prepare stream=0
esplus_submit #1..#235 -> 0 (NOT_PREPARED)   <- audio only; video waits for IDR
esplus_submit #236 -> 4                       <- first video frame (IDR)
cb player error 0xfe6c1008
cb prepare_async_done ok=0
esplus_start -> 1
(esplusplayer_get_playing_time returns false on every poll)
```

Diagnostics line (times in ms since `prepare_async`): `err=2044` fires 1–2 ms after `a1=2043`
(first accepted audio packet). First accepted audio PTS = 1175 ms, first video PTS = 0 ms
(audio arrived ~2 s before the first IDR because the client's first IDR request is lost —
Wolf starts its pipelines later; see §5).

## 2. What `0xfe6c1008` is — [F]

Three independent confirmations from the decompiled TV libraries:

1. `libcapi-media-epplayer.so.0.4.0.c`, `__epplayer_convert_error_code` (~line 3180):
   `0xfe6c1008` maps to the symbol **`EPPLAYER_ERROR_NOT_SUPPORTED_FORMAT`**.
2. `libtrackrenderer.so.c`, `plusplayer::trackrenderer::internal::HandleOmxError`
   (ELF 0x2f324): a GST error originating from an element whose name contains `omx`,
   whose GstElement class is **"Audio"**, with GError code 1 → returns `0xfe6c1008`
   (`0xfe6c002e` when a second flag is false).
3. Same file, `ConvertGstStreamError` (case 1 = `GST_STREAM_ERROR_DECODE`) and
   `ConvertGstCoreError(12)` both → `0xfe6c1008`.

Error propagation path — [F]: omx audio decoder posts `GST_MESSAGE_ERROR` on the bus →
`TrackRenderer::GstBusSyncHandlerCb_` (libtrackrenderer.so, ELF 0x324a8, `GST_MESSAGE_ERROR`
branch) → `trackrenderer::HandleError` → error callback → `PrepareTask_` sees `Prepare_`
fail → `OnPrepareDone(false)` → `cb prepare_async_done ok=0`. The `0xfe6c…` prefix is the
plusplayer/epplayer error space (not the public Tizen `player_error_types` enum).

**Conclusion: the audio decoder rejected the stream format. This is not a clock/PTS error.**

Independent on-TV verification (no code change): run
`dlogutil PLUSPLAYER TIZEN_N_EPPLAYER` during a session — `HandleError` logs the element
class, element name and GError text; `__epplayer_convert_error_code` logs the full mapping.

## 3. Why: Opus has no decoder on this TV — [F], decisive

- The esplusplayer C API exposes `ESPLUSPLAYER_AUDIO_MIME_TYPE_OPUS = 8` and
  `AudioStream::SetMimeType_` (libesplusplayer.so, ELF 0x424dc, case 8) maps it to the
  Gst caps string `audio/x-opus`. **[F]**
- `libtrackrenderer.so` (with `libtrackrenderer.so.static-init.c`) contains the static
  caps→decoder map used to build the audio pipeline. Entries pair mime types with the
  `omx_audiodec` element / `OMX.SDP.AUDIO.DECODER.INST0` component (and a second
  `OMX.SDP.AUDANY_PRIMARY.Decoder` family). Supported audio caps in the table:
  `audio/mpeg, x-aac, x-ac3, ac3, x-eac3, x-ac4, x-adpcm, x-wma, x-vorbis,
  x-pn-realaudio, x-true-hd, x-gst-fourcc-mha1, x-gst-fourcc-mhm1, x-alaw, x-mulaw`
  (plus `audio/x-raw_tz` for the secure path). **The string `x-opus` does not occur
  anywhere in libtrackrenderer.so.** **[F]**
- Timeline match — [F]: the error fires 1–2 ms after the *first audio buffer reaches the
  decoder*, not when audio submits start (submits began 2 s earlier and were rejected with
  NOT_PREPARED before the pipeline was linked). Caps are only validated by the decoder when
  the first buffer flows (during the PREROLL into PAUSED transition that prepare performs).

So on this TV the OPUS enum is a dead entry: the esplusplayer API accepts the stream-info,
but trackrenderer/OMX has no Opus decoder, the caps are rejected at preroll, and prepare
completes with `ok=0` + `EPPLAYER_ERROR_NOT_SUPPORTED_FORMAT`.

Supporting detail (moot given the above, but recorded):
- `codec_data` is attached to caps as the `codec_data` field (a GstBuffer) by
  `GstCapsBuilder::MakeGstBufferWithCodecData` (libtrackrenderer.so); `streamheader` is not
  used. **[F]** The RFC 7845 OpusHead (19 B, pre-skip 3840) we build is a reasonable value
  for that field.
- `channels`/`sample_rate` from the stream-info struct go into caps fields
  `channels`/`rate` (`track::audio::FillDefault`); `bitrate` is only logged. **[F]**
- Packets: yes, raw elementary-stream payloads, no container — `esplusplayer_submit_packet`
  wraps each buffer into a GstBuffer on the sink pad. **[F]**

## 4. NOT_PREPARED for audio while video "works" — [F]

This is a sampling artifact of the engine, not player asymmetry. The engine does not submit
video before an IDR frame (`cc: Waiting for IDR frame`), so every submit in that window is
audio. Decisive control log `tv-recon/tv-console-Moonlight5.ui-20261104-214435.txt`:
a session where the IDR never arrived — **588 audio submits → 0 (NOT_PREPARED), prepare
never completed, no player error**. Audio alone never unblocks submission.

In session 21:47 the first video submit (#236, the IDR) returned `4` immediately and audio
returns `4` from the next packet on (a1−v1 = 1 ms). The unlock coincides with the video
branch being able to preroll. `SubmitPacketCommon_` (libesplusplayer.so, ELF 0x35690)
returns status 0 when the state-machine state is below the prepared threshold.
**[P, high confidence]** Mechanism: prepare links both branches and drives the pipeline to
PAUSED; sinks need preroll buffers on *both* branches (including `directvideosink`), so
until video data exists the player is not prepared and rejects everything. Not a documented
"first packet must be video" rule.

## 5. Wolf-side timeline — [F, from wolf-last-minute-214727.log]

- 20:47:01.356 — client's first IDR request arrives (ENET `IDR_FRAME`), but the Wolf video
  pipeline only started at 20:47:01.494 and the audio pipeline at 20:47:01.824 → the first
  IDR request is lost. Second request at 20:47:03.508 ("Forcing IDR").
- Hence `first_du_ms=2247` and the 1175 ms audio PTS offset (engine kept incrementing the
  audio PTS counter across rejected packets).

Wolf logs can confirm/refute this timing, but they **cannot** adjudicate the prepare failure
itself: PTS values are computed client-side (video from `presentationTimeMs`, audio from a
local counter). The prepare failure cause is the TV audio decoder (§2–3).

## 6. Could PTS mismatch (audio 1175 ms vs video 0 ms) cause `ok=0`? — [P, strong no]

Nothing in decompiled `libtrackrenderer.so` / `libesplusplayer.so` compares stream start PTS
during prepare; prepare builds pipelines and sets `audio-delay-info-for-avsync` on the audio
sink (compensating decode latency by video resolution). A 1175 ms A/V offset would wreck
sync after start, but it does not fail prepare. This also matches the timing: the error
arrives 1–2 ms after the first buffer reaches the decoder, not 2 s earlier when "bad PTS"
audio first got submitted.

## 7. "Why not transcode to AAC?" — rejected, with protocol facts

- **[F]** GameStream/Moonlight audio is **always Opus** — there is no codec negotiation.
  moonlight-common-c (submodule, `src/SdpGenerator.c:481-496`, `src/RtspConnection.c:754`)
  negotiates only the channel configuration (`x-nv-audio.surround.*`,
  `a=fmtp:97 surround-params=…`; payload 97 = Opus). `src/AudioStream.c` has exactly one
  decode path: Opus (`opusHeaderByte`, `decodeAndPlaySample`). GFE behaved the same (hence
  the GFE channel-mapping swap code in common-c); Sunshine/Wolf replicate it — the Wolf
  pipeline hardcodes `opusenc … audio-type=restricted-lowdelay`.
- **[F]** The client has no way to announce unsupported audio codecs: unlike video
  (`supportedVideoFormats` in `/launch`), no audio-codec capability field exists.
- **[F]** The TV's hardware decoders (trackrenderer map, §3) do include AAC — but feeding
  them would require local Opus→AAC transcoding in the client (extra CPU + latency) to
  achieve exactly what direct Opus→PCM decode achieves. Also, Opus `restricted-lowdelay`
  5 ms frames are inherently lower-latency than AAC (encoder priming ~2048 samples ≈ 42 ms).

Correct design consequence: the engine must treat "input stream is always Opus" and
"this platform has no hardware Opus path" as facts, and decode Opus in software
(vendored libopus) and submit `audio/x-raw` PCM to the same esplusplayer instance (keeps
the shared A/V clock and the working submit path) — with PulseAudio output as the
alternative backend. This is exactly the fallback already sketched in
`mlclient/audio_opus.c`'s header comment. The enum values 9+ in `esplusplayer_tv50.h`
are the PCM (`audio/x-raw`) mime variants; the x-raw caps variants in
`AudioStream::SetMimeType_` (libesplusplayer.so ELF 0x424dc, cases 9–0xf) set internal
format fields (endianness/width/layout) accordingly.

## 8. Minimal discriminating tests (A = bad Opus/codec_data, B = PTS-axis mismatch)

- **T0, no code change [F]:** `dlogutil PLUSPLAYER TIZEN_N_EPPLAYER` during a session —
  direct proof the error comes from `omx_audiodec`.
- **T1, decisive (one run):** extend the video-only test tool (which yields `ok=1`) with
  the Opus audio track (same 48 kHz/2 ch/5 ms packets, OpusHead codec_data) and
  **PTS(audio) = PTS(video), one shared axis starting at 0**. `ok=0` + `0xfe6c1008`
  → hypothesis A (format). `ok=1` → hypothesis B (PTS).
- **T2, control:** video + `audio/x-raw` PCM (mime 9+) with audio PTS deliberately offset
  by ~1.2 s → `ok=1` proves a PTS offset alone does not kill prepare (completes the case
  for A).

## 9. Fix plan (see PROMPT-esplus-audio-pcm.md for the implementation prompt)

1. Vendor/build libopus for ARM (Tizen 5.0 sysroot, arm-linux-gnueabi-gcc 6.2, same as the
   rest of the engine deps). Decode in `decodeAndPlaySample` and submit PCM.
2. `esplusplayer_audio_stream_info` with a PCM mime (9+, per `esplusplayer_tv50.h`),
   `channels`/`sample_rate` set; no `codec_data` (codec_data_length = 0).
3. Keep the shared-clock design: same player instance, same submit path
   (`esplus_submit`), PTS policy unchanged (audio counter +5 ms/packet from the first
   packet) — or align audio start to the first video PTS (see 4).
4. Independent improvements (not required for the fix):
   - Align the audio PTS axis to the first accepted video frame to remove the 1175 ms
     startup skew caused by the lost first IDR request.
   - Send a second IDR request after a short delay (or once the first video data is seen)
     so it cannot be lost before Wolf's pipeline is up (`first_du_ms≈2.2 s` sessions).

## 10. Caveat

All library conclusions come from this TV's firmware libraries. Other firmware versions
may map errors differently — hence T0/dlog as the on-device verification step.

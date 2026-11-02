/*
 * video_esplus.c - moonlight-common-c DECODER_RENDERER_CALLBACKS backed by the
 * shared Tizen esplusplayer (esplus_player.c). The native equivalent of the WASM
 * fork's wasm/wasmplayer.cpp (EMSS ElementaryMediaTrack.AppendPacket).
 *
 * moonlight-common-c delivers each frame as a chain of LENTRY buffers already in
 * Annex-B; esplusplayer accepts raw Annex-B H.264 (proven by esppplay). We
 * concatenate the chain and submit one ES packet with the frame's PTS.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Limelight.h"
#include "esplusplayer_tv50.h"

extern esplusplayer_handle esplus_create_open(void);
extern esplusplayer_handle esplus_handle(void);
extern int esplus_set_window(int w, int h);
extern void esplus_prepare(void);
extern int esplus_submit(const esplusplayer_es_packet *pkt);
extern void esplus_close(void);

#define VLOG(...) fprintf(stderr, "[video] " __VA_ARGS__)

static int s_fps;
static uint8_t *s_scratch;
static size_t s_scratch_cap;
static uint64_t s_base_pts_ms;

static int ensure_scratch(size_t need) {
  if (need <= s_scratch_cap) return 0;
  size_t cap = s_scratch_cap ? s_scratch_cap : 65536;
  while (cap < need) cap *= 2;
  uint8_t *p = realloc(s_scratch, cap);
  if (!p) return -1;
  s_scratch = p; s_scratch_cap = cap;
  return 0;
}

static int VidDecSetup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  (void)context; (void)drFlags;
  s_fps = redrawRate > 0 ? redrawRate : 60;
  uint32_t mime;
  if (videoFormat & VIDEO_FORMAT_MASK_H264) mime = ESPLUSPLAYER_VIDEO_MIME_TYPE_H264;
  else if (videoFormat & VIDEO_FORMAT_MASK_H265) mime = ESPLUSPLAYER_VIDEO_MIME_TYPE_HEVC;
  else { VLOG("unsupported videoFormat 0x%x\n", videoFormat); return -1; }

  if (!esplus_create_open()) return -1;        /* audio init may have created it already */
  if (esplus_set_window(width, height) != 0) return -1;
  esplusplayer_video_stream_info vi = {NULL, 0, mime, (uint32_t)width, (uint32_t)height,
                                       (uint32_t)width, (uint32_t)height, (uint32_t)s_fps, 1};
  if (!esplusplayer_add_video_stream_info(esplus_handle(), &vi)) { VLOG("add_video_stream_info failed\n"); return -1; }
  VLOG("setup %dx%d@%d fmt=0x%x\n", width, height, s_fps, videoFormat);
  return 0;
}

static void VidDecStart(void) { s_base_pts_ms = 0; esplus_prepare(); }
static void VidDecStop(void) {}
static void VidDecCleanup(void) { esplus_close(); free(s_scratch); s_scratch = NULL; s_scratch_cap = 0; }

static int VidDecSubmit(PDECODE_UNIT du) {
  if (ensure_scratch((size_t)du->fullLength)) return DR_NEED_IDR;
  size_t off = 0;
  for (PLENTRY e = du->bufferList; e; e = e->next) {
    memcpy(s_scratch + off, e->data, (size_t)e->length);
    off += (size_t)e->length;
  }
  if (s_base_pts_ms == 0) s_base_pts_ms = du->presentationTimeMs;
  uint64_t pts_ms = du->presentationTimeMs - s_base_pts_ms;
  esplusplayer_es_packet pkt = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, (char *)s_scratch, (uint32_t)off,
                                pts_ms * 1000000ull, (1000000000ull / s_fps)};
  int r = esplus_submit(&pkt);
  if (r == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS || r == ESPLUSPLAYER_SUBMIT_STATUS_FULL
      || r == ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED)
    return DR_OK;
  VLOG("submit_packet failed %d\n", r);
  return DR_NEED_IDR;
}

void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS cb) {
  LiInitializeVideoCallbacks(cb);
  cb->setup = VidDecSetup;
  cb->start = VidDecStart;
  cb->stop = VidDecStop;
  cb->cleanup = VidDecCleanup;
  cb->submitDecodeUnit = VidDecSubmit;
  cb->capabilities = CAPABILITY_DIRECT_SUBMIT;
}

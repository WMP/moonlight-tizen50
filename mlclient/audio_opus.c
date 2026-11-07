/*
 * audio_opus.c - moonlight-common-c AUDIO_RENDERER_CALLBACKS. The host's Opus frames are
 * decoded in the engine with libopus (fixed point, from the submodule; libopus_tv.a) to S16LE
 * PCM, which is handed to the host through ml_engine_set_audio_cb (mlengine.h); the host sends
 * it to the web UI, which plays it with Web Audio.
 *
 * Platform limitation: the TV player rejects engine-fed audio streams, so the esplusplayer
 * carries only video (esplus_player.c) and audio does not share its clock.
 * Without a callback at init the stream goes on without sound: init still returns 0 and
 * decodeAndPlaySample returns at once (no decode: nobody could play it).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>

#include "Limelight.h"
#include "mldiag.h"
#include "mlengine.h"
#include "mltrace.h"

#define ALOG(...) fprintf(stderr, "[audio] " __VA_ARGS__)

static ml_engine_audio_cb s_cb_set;   /* set by the host, read once per session at init (atomic) */
void ml_engine_set_audio_cb(ml_engine_audio_cb cb) { __atomic_store_n(&s_cb_set, cb, __ATOMIC_RELEASE); }
void ml_engine_set_audio_drops(int dropped) { mldiag_adrops(dropped); }

static int s_samples_per_frame = 240, s_channels = 2, s_rate = 48000;
static OpusMSDecoder *s_dec;
static opus_int16 *s_pcm;   /* one frame, reused: the callback copies it */
static ml_engine_audio_cb s_cb;   /* the callback of this session; NULL = no audio output */
static unsigned s_nerr;

static void ar_cleanup(void) {
  if (s_dec) opus_multistream_decoder_destroy(s_dec);
  free(s_pcm);
  s_dec = NULL; s_pcm = NULL; s_cb = NULL;
}

static int ar_init(int audioConfig, const POPUS_MULTISTREAM_CONFIGURATION cfg, void *ctx, int flags) {
  (void)audioConfig; (void)ctx; (void)flags;
  MLT("audio init cfg=0x%x %d Hz %d ch streams=%d", audioConfig, cfg ? cfg->sampleRate : 0,
      cfg ? cfg->channelCount : 0, cfg ? cfg->streams : 0);
  if (!cfg || cfg->channelCount <= 0 || cfg->channelCount > 8 || cfg->sampleRate <= 0 || cfg->samplesPerFrame <= 0) { MLT("audio init -> -1 (config)"); return -1; }
  ar_cleanup();   /* a decoder left over from a previous session */
  s_samples_per_frame = cfg->samplesPerFrame;
  s_channels = cfg->channelCount;
  s_rate = cfg->sampleRate;
  s_nerr = 0;

  int err = 0;
  s_dec = opus_multistream_decoder_create(cfg->sampleRate, cfg->channelCount, cfg->streams, cfg->coupledStreams, cfg->mapping, &err);
  if (!s_dec) {
    ALOG("opus decoder create failed %d\n", err); MLT("audio init -> -1 (opus %d)", err);
    mldiag_adec(0, err);
    return -1;
  }
  s_pcm = malloc((size_t)s_samples_per_frame * (size_t)s_channels * sizeof *s_pcm);
  if (!s_pcm) { ar_cleanup(); ALOG("out of memory\n"); return -1; }

  s_cb = __atomic_load_n(&s_cb_set, __ATOMIC_ACQUIRE);
  mldiag_aout(s_cb != NULL);
  if (!s_cb) { ALOG("audio output unavailable (no callback)\n"); MLT("audio output unavailable (no callback)"); }
  ALOG("init %d Hz, %d ch, %d samples/frame, PCM S16LE -> host callback %s\n", cfg->sampleRate, cfg->channelCount, cfg->samplesPerFrame, s_cb ? "set" : "missing");
  return 0;
}

static void ar_start(void) {}
static void ar_stop(void) {}

static void ar_decodePlay(char *data, int len) {
  if (!s_dec || !s_pcm || !s_cb) return;
  /* no data = a lost packet: libopus conceals it */
  int n = (data == NULL || len <= 0)
      ? opus_multistream_decode(s_dec, NULL, 0, s_pcm, s_samples_per_frame, 0)
      : opus_multistream_decode(s_dec, (const unsigned char *)data, len, s_pcm, s_samples_per_frame, 0);
  if (n <= 0) {
    mldiag_adec(0, n);
    if (s_nerr++ < 4 || s_nerr % 256 == 0) MLT("opus decode error %d (#%u)", n, s_nerr);
    return;   /* nothing is handed over: the UI fills the gap */
  }
  mldiag_adec(1, 0);
  mldiag_aframe(n * s_channels * 2);
  s_cb((const short *)s_pcm, n, s_channels, s_rate);   /* the host copies and returns at once */
}

void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb) {
  LiInitializeAudioCallbacks(cb);
  cb->init = ar_init;
  cb->start = ar_start;
  cb->stop = ar_stop;
  cb->cleanup = ar_cleanup;
  cb->decodeAndPlaySample = ar_decodePlay;
  /* no CAPABILITY_DIRECT_SUBMIT: moonlight-common-c queues the packets and calls decodeAndPlaySample from its
     own audio decoder thread, so libopus never runs on the receive thread */
  cb->capabilities = 0;
}

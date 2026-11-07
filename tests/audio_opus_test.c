/*
 * Host self-test for mlclient/audio_opus.c (Opus -> PCM S16LE -> the host audio callback), driven through
 * AudEsplus_FillCallbacks. The callback is a stub; libopus encodes the test frames.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus_multistream.h>
#include "Limelight.h"
#include "mlengine.h"
#include "mldiag.h"

void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS c) { memset(c, 0, sizeof *c); }
void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb);

/* the host callback stub: records what audio_opus.c hands over */
static int n_cb, last_samples, last_ch, last_rate; static long long sum_abs;
static void audio_cb(const short *pcm, int samples, int channels, int rate) {
  assert(pcm);
  for (int i = 0; i < samples * channels; i++) sum_abs += pcm[i] < 0 ? -pcm[i] : pcm[i];   /* the buffer is readable */
  n_cb++; last_samples = samples; last_ch = channels; last_rate = rate;
}

#define MS 1000000ull
enum { SPF = 240 };

int main(void) {
  OPUS_MULTISTREAM_CONFIGURATION cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.sampleRate = 48000; cfg.channelCount = 2; cfg.streams = 1; cfg.coupledStreams = 1;
  cfg.samplesPerFrame = SPF; cfg.mapping[0] = 0; cfg.mapping[1] = 1;

  int err = 0;
  OpusMSEncoder *enc = opus_multistream_encoder_create(48000, 2, 1, 1, cfg.mapping, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
  assert(enc && err == OPUS_OK);
  unsigned char fr[4][400]; int fl[4];
  for (int k = 0; k < 4; k++) {
    opus_int16 pcm[SPF * 2];
    for (int i = 0; i < SPF; i++) { pcm[2 * i] = (opus_int16)(((i + k * SPF) * 300) % 20000); pcm[2 * i + 1] = (opus_int16)-pcm[2 * i]; }
    fl[k] = opus_multistream_encode(enc, pcm, SPF, fr[k], sizeof fr[k]);
    assert(fl[k] > 0);
  }
  opus_multistream_encoder_destroy(enc);

  AUDIO_RENDERER_CALLBACKS cb;
  AudEsplus_FillCallbacks(&cb);
  assert(cb.capabilities & CAPABILITY_DIRECT_SUBMIT);

  for (int round = 0; round < 2; round++) {   /* round 1: cleanup, then a second init */
    mldiag_reset(NULL);
    n_cb = 0; ml_engine_set_audio_cb(audio_cb);
    assert(cb.init(0, &cfg, NULL, 0) == 0);
    cb.start();

    for (int k = 0; k < 3; k++) cb.decodeAndPlaySample((char *)fr[k], fl[k]);
    assert(n_cb == 3 && last_samples == SPF && last_ch == 2 && last_rate == 48000);   /* 960 B per 5 ms frame */
    assert(sum_abs > 0);

    cb.decodeAndPlaySample(NULL, 0);   /* loss: concealed frame */
    assert(n_cb == 4 && last_samples == SPF);
    cb.decodeAndPlaySample((char *)fr[0], 0);   /* len 0 with data: also a loss */
    assert(n_cb == 5 && last_samples == SPF);

    char junk[3] = { (char)0xff, (char)0xff, (char)0xff };   /* undecodable: counted, no callback */
    cb.decodeAndPlaySample(junk, (int)sizeof junk);
    assert(n_cb == 5);
    cb.decodeAndPlaySample((char *)fr[3], fl[3]);
    assert(n_cb == 6);

    char line[4096];
    ml_engine_set_audio_drops(7);
    mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " adec=6 adec_fail=1/-4 err="));   /* -4 = OPUS_INVALID_PACKET */
    assert(strstr(line, " | aout webaudio frames=6 bytes=5760 sent=0 drops=7 sendfail=0 | "));
    ml_engine_set_audio_host_stats(5, 2, 1); mldiag_last(line, sizeof line);   /* read time: the new counters */
    assert(strstr(line, " | aout webaudio frames=6 bytes=5760 sent=5 drops=2 sendfail=1 | "));

    cb.stop(); cb.cleanup();
    cb.decodeAndPlaySample((char *)fr[0], fl[0]);   /* no decoder: ignored */
    assert(n_cb == 6);
  }

  /* no callback: init still succeeds, decode returns early (no call, no crash) */
  mldiag_reset(NULL);
  ml_engine_set_audio_cb(NULL); n_cb = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  cb.start(); cb.decodeAndPlaySample((char *)fr[0], fl[0]); cb.decodeAndPlaySample(NULL, 0); cb.stop();
  assert(n_cb == 0);
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout none reason=no-callback | ") && strstr(line, " adec=0 ")); }
  cb.cleanup();
  /* the callback is read at init: one set later does not apply to a running session */
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  ml_engine_set_audio_cb(audio_cb); cb.decodeAndPlaySample((char *)fr[0], fl[0]); assert(n_cb == 0);
  cb.cleanup();

  cfg.channelCount = 0;
  assert(cb.init(0, &cfg, NULL, 0) == -1);
  puts("audio_opus_test OK");
  return 0;
}

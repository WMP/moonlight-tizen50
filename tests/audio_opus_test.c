/*
 * Host self-test for the native audio-io output in mlclient/audio_opus.c (dlopen'd
 * capi-media-audio-io, ring buffer, fallback to the host callback). No real library:
 * built with -DML_AUDIO_IO_FAKE, the test sets the ml_aio_* function pointers that
 * audio_opus.c otherwise resolves with dlopen/dlsym. Run: sh tests/build-esplus-ecore-test.sh
 */
#define ML_AUDIO_IO_FAKE 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <opus_multistream.h>
#include "Limelight.h"
#include "esplus_stats.h"
#include "mlengine.h"
#include "mldiag.h"

/* the fake seam: audio_opus.c makes these non-static under ML_AUDIO_IO_FAKE */
typedef struct ml_audio_out_s *ml_audio_out_h;
extern int (*ml_aio_create)(int rate, int ch, int type, ml_audio_out_h *out);
extern int (*ml_aio_prepare)(ml_audio_out_h out);
extern int (*ml_aio_unprepare)(ml_audio_out_h out);
extern int (*ml_aio_destroy)(ml_audio_out_h out);
extern int (*ml_aio_write)(ml_audio_out_h out, void *buf, unsigned len);
extern int (*ml_aio_get_buffer_size)(ml_audio_out_h out, int *size);

void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS c) { memset(c, 0, sizeof *c); }
void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb);
esplus_stats_t esplus_stats;

/* the host callback stub: records what audio_opus.c hands over */
static int n_cb, last_samples, last_ch, last_rate;
static void audio_cb(const short *pcm, int samples, int channels, int rate) {
  assert(pcm);
  n_cb++; last_samples = samples; last_ch = channels; last_rate = rate;
}

/* ---- fake audio-io ---- */
static int create_calls, create_rc, prepare_calls, prepare_rc, destroy_calls, unprepare_calls;
static int write_rc;                              /* < 0: audio_out_write fails */
static unsigned write_calls;
static size_t wrote_bytes;
static int created_rate, created_ch, created_type, got_bufsize;
static unsigned write_delay_us;                   /* stub ms-sleep per write: a slow device */

static int st_create(int rate, int ch, int type, ml_audio_out_h *out) {
  create_calls++; created_rate = rate; created_ch = ch; created_type = type;
  if (create_rc) return create_rc;
  *out = (ml_audio_out_h)&create_calls;   /* a non-NULL handle */
  return 0;
}
static int st_prepare(ml_audio_out_h h) { (void)h; prepare_calls++; return prepare_rc; }
static int st_unprepare(ml_audio_out_h h) { (void)h; unprepare_calls++; return 0; }
static int st_destroy(ml_audio_out_h h) { (void)h; destroy_calls++; return 0; }
static int st_write(ml_audio_out_h h, void *buf, unsigned len) {
  (void)h; (void)buf;
  write_calls++;
  if (write_delay_us) usleep(write_delay_us);
  if (write_rc) return write_rc;
  wrote_bytes += len;
  return (int)len;
}
static int st_get_buffer_size(ml_audio_out_h h, int *size) { (void)h; got_bufsize++; *size = 4096; return 0; }

static void aio_install(int ok) {
  ml_aio_create = ok ? st_create : NULL;
  ml_aio_prepare = ok ? st_prepare : NULL;
  ml_aio_unprepare = ok ? st_unprepare : NULL;
  ml_aio_destroy = ok ? st_destroy : NULL;
  ml_aio_write = ok ? st_write : NULL;
  ml_aio_get_buffer_size = ok ? st_get_buffer_size : NULL;
}

enum { SPF = 240 };

int main(void) {
  aio_install(1);

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

  /* === native path: create ok, frames flow to audio_out_write === */
  mldiag_reset(NULL);
  ml_engine_set_audio_cb(audio_cb); n_cb = 0;
  write_calls = 0; wrote_bytes = 0; write_delay_us = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  assert(create_calls == 1 && created_rate == 48000 && created_ch == 0x81 /* STEREO */ && created_type == 0x71 /* S16_LE */);
  assert(ml_engine_get_audio_route() == 2);
  cb.start();
  for (int k = 0; k < 6; k++) cb.decodeAndPlaySample((char *)fr[k % 4], fl[k % 4]);
  { size_t wb = 0;
    for (int i = 0; i < 10000; i++) { wb = wrote_bytes; if (wb >= 6 * (size_t)(SPF * 2 * 2)) break; usleep(500); }
    assert(wb == 6 * (size_t)(SPF * 2 * 2));   /* everything reached the device */
  }
  assert(n_cb == 0);                       /* no callback: the native path owns this session */
  { ml_engine_audio_stats st; st.struct_size = sizeof st;
    assert(ml_engine_get_audio_stats(&st) == 0);
    assert(st.route == 2 && st.frames == 6);
  }
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout native frames=")); assert(!strstr(line, "aout webaudio")); }
  cb.stop(); cb.cleanup();
  assert(destroy_calls == 1 && unprepare_calls == 1);
  assert(ml_engine_get_audio_route() == 0);   /* cleanup: no session, no route */

  /* === fallback at init: create fails -> the callback path, reason in the line === */
  mldiag_reset(NULL);
  create_rc = -13; n_cb = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  assert(ml_engine_get_audio_route() == 1);
  cb.start();
  for (int k = 0; k < 3; k++) cb.decodeAndPlaySample((char *)fr[k], fl[k]);
  assert(n_cb == 3);
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout webaudio fallback=native reason=create--13 frames=3 ")); }
  cb.stop(); cb.cleanup();
  create_rc = 0;

  /* === fallback mid-session: prepare fails -> the callback takes over === */
  mldiag_reset(NULL);
  prepare_rc = -14; n_cb = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  assert(ml_engine_get_audio_route() == 2);   /* native until the worker's prepare fails */
  cb.start();
  for (int k = 0; k < 3; k++) cb.decodeAndPlaySample((char *)fr[k], fl[k]);
  { int route = 0;
    for (int i = 0; i < 10000; i++) { route = ml_engine_get_audio_route(); if (route == 1) break; usleep(500); }
    assert(route == 1);   /* fell back */
  }
  n_cb = 0;
  cb.decodeAndPlaySample((char *)fr[0], fl[0]);
  assert(n_cb == 1);   /* frames now go to the callback */
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout webaudio fallback=native reason=prepare--14 "));   /* the frame count is timing-dependent */ }
  cb.stop(); cb.cleanup();
  prepare_rc = 0;

  /* === fallback mid-session: write fails === */
  mldiag_reset(NULL);
  write_rc = -15;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  cb.start();
  cb.decodeAndPlaySample((char *)fr[0], fl[0]);
  { int route = 0;
    for (int i = 0; i < 10000; i++) { route = ml_engine_get_audio_route(); if (route == 1) break; usleep(500); }
    assert(route == 1);
  }
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout webaudio fallback=native reason=write--15")); }
  write_rc = 0;
  cb.stop(); cb.cleanup();

  /* === no symbols at all -> callback path, no create call === */
  mldiag_reset(NULL);
  aio_install(0); create_calls = 0; n_cb = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  assert(create_calls == 0 && ml_engine_get_audio_route() == 1);
  cb.start();
  cb.decodeAndPlaySample((char *)fr[0], fl[0]);
  assert(n_cb == 1);
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout webaudio fallback=native reason=no-symbols frames=1 ")); }
  cb.stop(); cb.cleanup();
  aio_install(1);

  /* === no callback and no library: no output, still no crash === */
  mldiag_reset(NULL);
  aio_install(0); ml_engine_set_audio_cb(NULL); n_cb = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  assert(ml_engine_get_audio_route() == 0);
  cb.start();
  cb.decodeAndPlaySample((char *)fr[0], fl[0]);
  cb.decodeAndPlaySample(NULL, 0);
  assert(n_cb == 0);
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout none reason=no-callback | ")); }
  cb.stop(); cb.cleanup();
  aio_install(1);
  ml_engine_set_audio_cb(audio_cb);

  /* === ring bound: a slow device (write blocks) must not grow the latency without end === */
  mldiag_reset(NULL);
  write_delay_us = 20 * 1000;   /* each write ~20 ms: the device cannot keep up */
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  cb.start();
  for (int k = 0; k < 200; k++) cb.decodeAndPlaySample((char *)fr[k % 4], fl[k % 4]);   /* 1 s of audio */
  usleep(400 * 1000);   /* let the worker fall behind and the cap kick in */
  { ml_engine_audio_stats st; st.struct_size = sizeof st;
    assert(ml_engine_get_audio_stats(&st) == 0);
    assert(st.route == 2);
    assert(st.overruns + st.drops > 0);           /* the cap dropped data */
    assert(st.buf_ms <= 130);                     /* bounded near the 2x target */
  }
  { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " | aout native frames=200 ")); }
  write_delay_us = 0;
  cb.stop(); cb.cleanup();

  /* === 5.1: audio-io takes mono/stereo only -> callback path === */
  mldiag_reset(NULL);
  OPUS_MULTISTREAM_CONFIGURATION cfg6 = cfg;
  cfg6.channelCount = 6; cfg6.streams = 4; cfg6.coupledStreams = 2;
  for (int i = 0; i < 6; i++) cfg6.mapping[i] = i;
  n_cb = 0; create_calls = 0;
  assert(cb.init(0, &cfg6, NULL, 0) == 0);
  assert(create_calls == 0 && ml_engine_get_audio_route() == 1);
  cb.stop(); cb.cleanup();

  /* === second session after all that: native again (state fully reset) === */
  mldiag_reset(NULL);
  n_cb = 0; create_calls = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  assert(create_calls == 1 && ml_engine_get_audio_route() == 2);
  cb.decodeAndPlaySample((char *)fr[0], fl[0]);
  cb.stop(); cb.cleanup();

  /* bad config still fails */
  cfg.channelCount = 0;
  assert(cb.init(0, &cfg, NULL, 0) == -1);
  (void)last_samples; (void)last_ch; (void)last_rate; (void)got_bufsize;
  puts("audio_opus_test OK");
  return 0;
}

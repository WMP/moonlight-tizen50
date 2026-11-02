/*
 * esplusplayer_tv50.h - C API of /usr/lib/libesplusplayer.so on Samsung TV Tizen 5.0
 * (old "plusplayer" ABI). Reconstructed from the TV binary with objdump.
 *
 * Legend:
 *   [ASM]  confirmed in the disassembly of the TV library (address given)
 *   [PUB]  taken from public esplusplayer 0.0.3 (Tizen 6.5) headers, NOT checked
 *          in the TV binary
 *   [TV]   confirmed by a run on the TV
 *
 * Main differences to the public (new) API:
 *   - Most functions return a C++ bool (1 = OK, 0 = fail), NOT a Tizen error
 *     code. A NULL handle and a "not implemented" vtable slot also return 0.  [ASM]
 *   - submit_packet returns esplusplayer_submit_status (4 = success).          [ASM]
 *   - es_packet.pts / duration are NANOSECONDS. The library divides them by
 *     1 000 000 before EsPacket::Create (internal unit is ms).  [ASM 0x340f8]
 *   - Callbacks have NO user_data argument.                                    [ASM]
 *   - esplusplayer_create() keeps ONE global player. A second create() deletes
 *     the first player. Use one player per process.          [ASM 0x337d4]
 */
#ifndef ESPLUSPLAYER_TV50_H
#define ESPLUSPLAYER_TV50_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Handle = plusplayer::EsPlusPlayer* (C++ object with vtable).  [ASM 0x337a8] */
typedef void *esplusplayer_handle;

/* Passed unchanged as plusplayer::StreamType.  [PUB]; the values are used
 * unchanged by EsPacket::Create  [ASM 0x3412c] */
typedef enum {
  ESPLUSPLAYER_STREAM_TYPE_AUDIO = 0,
  ESPLUSPLAYER_STREAM_TYPE_VIDEO = 1,
} esplusplayer_stream_type;

/* Passed unchanged to VideoStream::SetMimeType. Switch in
 * VideoStream::SetMimeType_ [ASM 0x32878] maps:
 * 1 x-h263, 2 x-h264, 3 x-h265, 4/5/6 mpeg v1/v2/v4, 7 x-vp8, 8 x-vp9,
 * 9 x-wmv (wmvversion 3), other "unknown". No AV1 / MJPEG on this version. */
typedef enum {
  ESPLUSPLAYER_VIDEO_MIME_TYPE_UNKNOWN = 0,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_H263 = 1,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_H264 = 2,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_HEVC = 3,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_MPEG1 = 4,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_MPEG2 = 5,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_MPEG4 = 6,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_VP8 = 7,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_VP9 = 8,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_WMV3 = 9,
} esplusplayer_video_mime_type;

/* plusplayer::DisplayType. ConvertToTrackRendererDisplayType accepts only
 * 0, 1, 2; other values call __assert_fail (process abort).
 * [ASM libplusplayer-core 0x10d08]. The Ecore_Wl2 window path accepts only
 * OVERLAY (1), other values abort.  [ASM libtrackrenderer 0x1eb56] */
typedef enum {
  ESPLUSPLAYER_DISPLAY_TYPE_NONE = 0,
  ESPLUSPLAYER_DISPLAY_TYPE_OVERLAY = 1,
  ESPLUSPLAYER_DISPLAY_TYPE_EVAS = 2,
} esplusplayer_display_type;

/* [PUB] order; not checked in the binary. */
typedef enum {
  ESPLUSPLAYER_DISPLAY_MODE_LETTER_BOX = 0,
  ESPLUSPLAYER_DISPLAY_MODE_ORIGIN_SIZE,
  ESPLUSPLAYER_DISPLAY_MODE_FULL_SCREEN,
  ESPLUSPLAYER_DISPLAY_MODE_CROPPED_FULL,
  ESPLUSPLAYER_DISPLAY_MODE_ORIGIN_OR_LETTER,
  ESPLUSPLAYER_DISPLAY_MODE_DST_ROI,
} esplusplayer_display_mode;

/* [PUB]; the TV value is passed unchanged to the callback [ASM 0x35140]. */
typedef enum {
  ESPLUSPLAYER_BUFFER_STATUS_UNDERRUN = 0,
  ESPLUSPLAYER_BUFFER_STATUS_OVERRUN = 1,
} esplusplayer_buffer_status;

/* plusplayer::PacketSubmitStatus from EsPlayer::SubmitPacketCommon_ [ASM 0x25690]:
 * 0 state <= NONE (not opened/prepared), 1 null packet or no active track,
 * 2 GstBuffer alloc failed, 4 success (compared with "cmp r0,#4").
 * 3 = FULL is [PUB] only. */
typedef enum {
  ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED = 0,
  ESPLUSPLAYER_SUBMIT_STATUS_INVALID_PACKET = 1,
  ESPLUSPLAYER_SUBMIT_STATUS_OUT_OF_MEMORY = 2,
  ESPLUSPLAYER_SUBMIT_STATUS_FULL = 3,
  ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS = 4,
} esplusplayer_submit_status;

/* 36 bytes. Offsets and field names confirmed by the dlog strings
 * "from->width" etc. and loads [r1,#0..#32]  [ASM 0x34854].
 * codec_data is copied (new[] + memcpy) when codec_data_length != 0. */
typedef struct {
  char *codec_data;           /* +0  */
  uint32_t codec_data_length; /* +4  */
  uint32_t mime_type;         /* +8  esplusplayer_video_mime_type */
  uint32_t width;             /* +12 */
  uint32_t height;            /* +16 */
  uint32_t max_width;         /* +20 */
  uint32_t max_height;        /* +24 */
  uint32_t framerate_num;     /* +28 */
  uint32_t framerate_den;     /* +32 */
} esplusplayer_video_stream_info;

/* Passed unchanged to AudioStream::SetMimeType. Switch in AudioStream::SetMimeType_
 * [ASM 0x324dc, tbh indexed by value-1] maps the TV's audio mime enum. NOTE: the
 * TV order DIFFERS from public 0.0.3 - it inserts DTS=7, so OPUS=8 (not 7).
 * Confirmed values: 1 AAC, 2 MP2, 3 MP3, 4 AC3, 5 EAC3, 6 VORBIS, 7 DTS,
 * 8 OPUS (audio/x-opus), 9.. PCM variants (audio/x-raw). */
typedef enum {
  ESPLUSPLAYER_AUDIO_MIME_TYPE_UNKNOWN = 0,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_AAC = 1,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_MP2 = 2,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_MP3 = 3,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_AC3 = 4,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_EAC3 = 5,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_VORBIS = 6,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_DTS = 7,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_OPUS = 8,
} esplusplayer_audio_mime_type;

/* 24 bytes. Offsets confirmed by the dlog strings "from->bitrate/channels/
 * sample_rate/codec_data_length" and loads [r1,#0..#20]  [ASM 0x342f8]. */
typedef struct {
  char *codec_data;           /* +0  */
  uint32_t codec_data_length; /* +4  */
  uint32_t mime_type;         /* +8  esplusplayer_audio_mime_type */
  uint32_t bitrate;           /* +12 */
  uint32_t channels;          /* +16 */
  uint32_t sample_rate;       /* +20 */
} esplusplayer_audio_stream_info;

/* Reads [r1,#0] type, [r1,#4] buffer, [r1,#8] size, ldrd [r1,#16] pts,
 * ldrd [r1,#24] duration [ASM 0x34070]. With EABI 8-byte alignment of
 * uint64_t the natural C layout gives the same offsets (checked below).
 * buffer is copied (new[] + memcpy), the caller keeps ownership.
 * buffer_size == 0 is handled as EOS for that track [ASM 0x25902]. */
typedef struct {
  uint32_t type;        /* +0  esplusplayer_stream_type */
  char *buffer;         /* +4  */
  uint32_t buffer_size; /* +8  */
  uint64_t pts;         /* +16 NANOSECONDS */
  uint64_t duration;    /* +24 NANOSECONDS */
} esplusplayer_es_packet;

#ifdef __cplusplus
static_assert(sizeof(esplusplayer_video_stream_info) == 36, "layout");
static_assert(sizeof(esplusplayer_audio_stream_info) == 24, "layout");
#else
_Static_assert(sizeof(esplusplayer_video_stream_info) == 36, "layout");
_Static_assert(sizeof(esplusplayer_audio_stream_info) == 24, "layout");
_Static_assert(__builtin_offsetof(esplusplayer_es_packet, pts) == 16, "layout");
_Static_assert(__builtin_offsetof(esplusplayer_es_packet, duration) == 24, "layout");
#endif

/* Callbacks: no user_data. Offsets in the global listener_bridge: error +4,
 * buffer_status +8, resource_conflicted +12, eos +16, ready_to_prepare +20,
 * prepare_async_done +24  [ASM 0x33938..0x33c30, 0x350e8, 0x34fb8, 0x35168] */
typedef void (*esplusplayer_error_cb)(uint32_t plusplayer_error_type); /* raw value, enum not mapped */
typedef void (*esplusplayer_buffer_status_cb)(uint32_t stream_type, uint32_t status);
typedef void (*esplusplayer_resource_conflicted_cb)(void);              /* [ASM] no args seen; assumed */
typedef void (*esplusplayer_eos_cb)(void);                              /* assumed */
typedef void (*esplusplayer_ready_to_prepare_cb)(uint32_t stream_type); /* [ASM 0x34f94] */
typedef void (*esplusplayer_prepare_async_done_cb)(int result_bool);    /* [ASM 0x35014] */

/* Return type "int" below is a C++ bool (0/1) unless noted. */
esplusplayer_handle esplusplayer_create(void);
int esplusplayer_open(esplusplayer_handle h);    /* vtable[2] */
int esplusplayer_close(esplusplayer_handle h);
int esplusplayer_prepare_async(esplusplayer_handle h); /* vtable[4] */
int esplusplayer_start(esplusplayer_handle h);
int esplusplayer_stop(esplusplayer_handle h);
int esplusplayer_pause(esplusplayer_handle h);
int esplusplayer_resume(esplusplayer_handle h);

/* Needs state > NONE (call after open) or returns 0  [ASM 0x23e54]. */
int esplusplayer_set_display(esplusplayer_handle h, esplusplayer_display_type type,
                             void *evas_object);
/* window = Ecore_Wl2_Window*; only DISPLAY_TYPE_OVERLAY. Uses
 * ecore_thread_main_loop_begin/end, so ecore must be initialised. [ASM] */
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type type,
                                   void *ecore_wl2_window, int x, int y, int w, int h_);
int esplusplayer_set_display_mode(esplusplayer_handle h, esplusplayer_display_mode mode); /* [PUB] */
int esplusplayer_set_display_visible(esplusplayer_handle h, int visible);                 /* [PUB] */

int esplusplayer_add_video_stream_info(esplusplayer_handle h,
                                       const esplusplayer_video_stream_info *info); /* vtable[18] */
int esplusplayer_add_audio_stream_info(esplusplayer_handle h,
                                       const esplusplayer_audio_stream_info *info);

/* Returns esplusplayer_submit_status (NOT bool). vtable[19]. */
int esplusplayer_submit_packet(esplusplayer_handle h, const esplusplayer_es_packet *pkt);
int esplusplayer_submit_eos_packet(esplusplayer_handle h, esplusplayer_stream_type type); /* [PUB] signature */

int esplusplayer_get_state(esplusplayer_handle h);                        /* not analysed */
int esplusplayer_get_playing_time(esplusplayer_handle h, uint64_t *time); /* [PUB] signature, unit not checked */
int esplusplayer_set_audio_mute(esplusplayer_handle h, int mute);         /* [PUB] */

void esplusplayer_set_error_cb(esplusplayer_handle h, esplusplayer_error_cb cb);
void esplusplayer_set_buffer_status_cb(esplusplayer_handle h, esplusplayer_buffer_status_cb cb);
void esplusplayer_set_resource_conflicted_cb(esplusplayer_handle h, esplusplayer_resource_conflicted_cb cb);
void esplusplayer_set_eos_cb(esplusplayer_handle h, esplusplayer_eos_cb cb);
void esplusplayer_set_ready_to_prepare_cb(esplusplayer_handle h, esplusplayer_ready_to_prepare_cb cb);
void esplusplayer_set_prepare_async_done_cb(esplusplayer_handle h, esplusplayer_prepare_async_done_cb cb);

#ifdef __cplusplus
}
#endif
#endif

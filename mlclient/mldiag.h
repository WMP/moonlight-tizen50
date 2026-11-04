/*
 * mldiag.h - engine-internal session diagnostics (not part of the ABI, mlengine.h).
 * Collects counters and a few moonlight-common-c log lines in release builds too, and
 * on a failed session builds ONE "ML-ERROR ..." line. Pure C, no engine headers.
 * Holds no host address, key, PIN, URL or certificate.
 */
#ifndef MLDIAG_H
#define MLDIAG_H

typedef struct {
  int width, height, fps, bitrate_kbps, packet_size, supported_video_formats, color_space,
      color_range, audio_config, server_codec_mode_support, encryption_flags;
  const char *app_version, *gfe_version;   /* copied; may be NULL */
} mldiag_cfg;

void mldiag_reset(const mldiag_cfg *cfg);                 /* session start: clears everything */
void mldiag_cc_line(const char *line);                    /* one moonlight-common-c log line */
void mldiag_video_setup(int fmt, int w, int h, int fps, int rc);
void mldiag_video_du(int is_idr);                         /* every decode unit */
void mldiag_submit(int is_video, int status);             /* esplus_submit result; 0 = success, else the player status */
void mldiag_player(const char *what, int rc);             /* "open", "win", "prep", "pdone" (prepare_async_done) or "start" */
/* Build, store and print the line. extra values come from the caller. */
void mldiag_fail(int code, int stage, const char *stage_name, int host_feature_flags,
                 int rtt, int rtt_var, unsigned player_errors, unsigned player_last_error);
/* Returns the stored code (0 = none); copies the line (NUL-terminated, truncated) if buf && len > 0. */
int mldiag_last(char *buf, int len);

#endif

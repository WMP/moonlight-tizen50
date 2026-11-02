/*
 * mlengine.h - public C ABI of libmlengine.so.
 *
 * One stable surface used by both the .NET (TizenFX) host and the CLI harness:
 *   - the .NET host loads the .so and P/Invokes these symbols (1:1 DllImport);
 *   - the CLI harness (main.c) links this .so directly for bring-up/testing.
 * Keep this ABI additive: never change a signature, only append new functions.
 */
#ifndef MLENGINE_H
#define MLENGINE_H
#ifdef __cplusplus
extern "C" {
#endif

/* Start a GameStream session. Non-blocking: does cert init, GameStream
 * orchestration (serverinfo->pair->launch->RTSP), LiStartConnection and input,
 * then returns 0 with streaming running on background threads; non-zero on
 * failure. pin may be NULL when the host is already paired. datadir NULL -> /tmp.
 * The strings need only stay valid for the duration of this call. */
int  ml_engine_start(const char *host, int appId, const char *pin,
                     int width, int height, int fps, int bitrate_kbps,
                     const char *datadir);

/* 1 while a session is active, 0 once it has ended (host drop / error / stop). */
int  ml_engine_running(void);

/* Block until the session ends or ml_engine_stop() is called. */
void ml_engine_wait(void);

/* Stop the session and release the HW video overlay. Idempotent. */
void ml_engine_stop(void);

/* Per-step calls used by the web UI through the host (pkg/ui/PROTOCOL.md).
 * Out strings/buffers are malloc'd: release them with ml_engine_free(). All return
 * 0 on success, else an engine/libgamestream error code (GS_* from errors.h). */
int  ml_engine_make_cert(char **cert_pem, char **key_pem);
int  ml_engine_http_init(const char *cert_pem, const char *key_pem, const char *unique_id);
int  ml_engine_open_url(const char *url, const char *ppk, unsigned char **out, int *out_len);
int  ml_engine_pair(int server_major, const char *address, int http_port, const char *pin,
                    char **server_cert_pem);
void ml_engine_cancel_request(void);
int  ml_engine_start_session(const char *host, int width, int height, int fps, int bitrate_kbps,
                             const char *rikey_hex, int rikeyid, const char *app_version,
                             const char *gfe_version, const char *rtsp_url,
                             int server_codec_mode_support, const char *video_codec, int hdr,
                             const char *audio_config);
void ml_engine_send_key(int key_code, int key_action, int modifiers);
void ml_engine_free(void *p);

#ifdef __cplusplus
}
#endif
#endif /* MLENGINE_H */

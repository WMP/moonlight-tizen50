/*
 * mlengine.h - public C ABI of libmlengine.so.
 *
 * One stable surface used by BOTH delivery paths:
 *   - root path: a thin native exe (main.c) links this .so directly;
 *   - no-root path: a .NET (TizenFX) host loads the .so and P/Invokes these
 *     same four symbols (1:1 DllImport).
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

#ifdef __cplusplus
}
#endif
#endif /* MLENGINE_H */

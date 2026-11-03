/*
 * mltrace.h - debug-build trace macro. MLT(fmt, ...) writes one line to stderr and
 * to Tizen dlog (tag MLENGINE). Without -DMLT_TRACE it expands to nothing, so
 * release builds carry no trace code and no libdl use.
 * dlog_print is resolved at run time (no link-time dependency on libdlog). In the
 * .NET host it is NOT in the global symbol scope (the runtime loads libdlog with
 * RTLD_LOCAL and the engine comes from a memfd), so RTLD_DEFAULT alone finds nothing:
 * open libdlog by name and look it up there. Each TU keeps its own cached pointer.
 * ml_log_cb (set with ml_engine_set_log_cb) also receives each line, on demand. It is
 * weak so test programs that do not link engine_api.c still build.
 */
#ifndef MLTRACE_H
#define MLTRACE_H

#ifdef MLT_TRACE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#endif

extern void (*volatile ml_log_cb)(const char *line) __attribute__((weak));

static void mlt_log(const char *fmt, ...) __attribute__((unused, format(printf, 1, 2)));
static void mlt_log(const char *fmt, ...) {
  static int (*dlog)(int, const char *, const char *, ...);
  static int resolved;
  char buf[512];
  va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
  fprintf(stderr, "[mlt] %s\n", buf);
  if (!resolved) {
    static const char *const libs[] = { "libdlog.so.0", "libdlog.so" };
    void *f = dlsym(RTLD_DEFAULT, "dlog_print");
    for (unsigned i = 0; !f && i < sizeof libs / sizeof libs[0]; i++) {
      void *h = dlopen(libs[i], RTLD_NOW);   /* kept open for process life */
      if (h) f = dlsym(h, "dlog_print");
    }
    dlog = (int (*)(int, const char *, const char *, ...))f;
    resolved = 1;
    if (!dlog) fprintf(stderr, "[mlt] dlog_print not found: %s\n", dlerror());
  }
  if (dlog) dlog(3 /* DLOG_DEBUG */, "MLENGINE", "%s", buf);
  if (&ml_log_cb) { void (*cb)(const char *) = ml_log_cb; if (cb) cb(buf); }
}
#define MLT(...) mlt_log(__VA_ARGS__)
#else
#define MLT(...) ((void)0)
#endif

#endif

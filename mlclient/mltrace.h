/*
 * mltrace.h - debug-build trace macro. MLT(fmt, ...) writes one line to stderr and
 * to Tizen dlog (tag MLENGINE). Without -DMLT_TRACE it expands to nothing, so
 * release builds carry no trace code and no libdl use.
 * dlog_print is resolved with dlsym (no link-time dependency on libdlog; the host
 * process already has it loaded). Each TU keeps its own cached pointer.
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

static void mlt_log(const char *fmt, ...) __attribute__((unused, format(printf, 1, 2)));
static void mlt_log(const char *fmt, ...) {
  static int (*dlog)(int, const char *, const char *, ...);
  static int resolved;
  char buf[512];
  va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
  fprintf(stderr, "[mlt] %s\n", buf);
  if (!resolved) { dlog = (int (*)(int, const char *, const char *, ...))dlsym(RTLD_DEFAULT, "dlog_print"); resolved = 1; }
  if (dlog) dlog(3 /* DLOG_DEBUG */, "MLENGINE", "%s", buf);
}
#define MLT(...) mlt_log(__VA_ARGS__)
#else
#define MLT(...) ((void)0)
#endif

#endif

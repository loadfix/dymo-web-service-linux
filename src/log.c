#include "log.h"

#include <stdio.h>
#include <time.h>

static log_level_t g_level = LOG_LVL_INFO;

void log_set_level(log_level_t lvl) {
    g_level = lvl;
}

static const char *level_name(log_level_t lvl) {
    switch (lvl) {
        case LOG_LVL_DEBUG: return "DEBUG";
        case LOG_LVL_INFO:  return "INFO";
        case LOG_LVL_WARN:  return "WARN";
        case LOG_LVL_ERROR: return "ERROR";
    }
    return "?";
}

void log_msg(log_level_t lvl, const char *fmt, ...) {
    if (lvl < g_level) return;
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    FILE *out = (lvl >= LOG_LVL_WARN) ? stderr : stdout;
    fprintf(out, "%s %-5s dymo ", ts, level_name(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}

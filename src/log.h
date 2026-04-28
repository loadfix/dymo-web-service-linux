#ifndef DYMO_LOG_H
#define DYMO_LOG_H

#include <stdarg.h>

typedef enum {
    LOG_LVL_DEBUG,
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR,
} log_level_t;

void log_set_level(log_level_t lvl);
void log_msg(log_level_t lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_msg(LOG_LVL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_LVL_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)

#endif

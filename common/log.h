#ifndef NSIMAJ_LOG_H
#define NSIMAJ_LOG_H

#include <stddef.h>

enum nsimaj_log_level {
    NSIMAJ_LOG_INFO = 0,
    NSIMAJ_LOG_WARN = 1,
    NSIMAJ_LOG_ERROR = 2
};

/* Open log file (create parent dirs). Also mirrors to stderr. */
int nsimaj_log_open(const char *path);

/* Close log file handle. */
void nsimaj_log_close(void);

void nsimaj_log(enum nsimaj_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define nsimaj_log_info(...)  nsimaj_log(NSIMAJ_LOG_INFO, __VA_ARGS__)
#define nsimaj_log_warn(...)  nsimaj_log(NSIMAJ_LOG_WARN, __VA_ARGS__)
#define nsimaj_log_error(...) nsimaj_log(NSIMAJ_LOG_ERROR, __VA_ARGS__)

#endif

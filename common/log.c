#define _DEFAULT_SOURCE
#include "log.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log_fp;
static char g_log_path[512];

static void ensure_parent_dir(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return;
    *slash = '\0';
    util_mkdir_p(dir, 0755);
}

int nsimaj_log_open(const char *path)
{
    if (!path || !path[0])
        return -1;
    ensure_parent_dir(path);
    FILE *fp = fopen(path, "a");
    if (!fp)
        return -1;
    if (g_log_fp && g_log_fp != stderr)
        fclose(g_log_fp);
    g_log_fp = fp;
    snprintf(g_log_path, sizeof g_log_path, "%s", path);
    setvbuf(g_log_fp, NULL, _IOLBF, 0);
    return 0;
}

void nsimaj_log_close(void)
{
    if (g_log_fp && g_log_fp != stderr) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    g_log_path[0] = '\0';
}

static const char *level_name(enum nsimaj_log_level level)
{
    switch (level) {
    case NSIMAJ_LOG_INFO:
        return "INFO";
    case NSIMAJ_LOG_WARN:
        return "WARN";
    case NSIMAJ_LOG_ERROR:
        return "ERROR";
    default:
        return "INFO";
    }
}

void nsimaj_log(enum nsimaj_log_level level, const char *fmt, ...)
{
    char ts[64];
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm))
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", &tm);
    else
        snprintf(ts, sizeof ts, "%ld", (long)now);

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s %s %s\n", ts, level_name(level), msg);
    if (g_log_fp) {
        fprintf(g_log_fp, "%s %s %s\n", ts, level_name(level), msg);
        fflush(g_log_fp);
    }
}

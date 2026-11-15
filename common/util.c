#define _DEFAULT_SOURCE
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

void util_trim(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

int util_read_file(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

int util_write_file(const char *path, const char *data, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    size_t len = strlen(data);
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += (size_t)w;
        len -= (size_t)w;
    }
    close(fd);
    return 0;
}

int util_mkdir_p(const char *path, int mode)
{
    char tmp[512];
    if (strlen(path) >= sizeof tmp)
        return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, (mode_t)mode) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, (mode_t)mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

const char *util_home_dir(void)
{
    /* When launched via sudo, keep config under the real user, not /root. */
    if (geteuid() == 0) {
        const char *su = getenv("SUDO_USER");
        if (su && *su && strcmp(su, "root") != 0) {
            struct passwd *pw = getpwnam(su);
            if (pw && pw->pw_dir && pw->pw_dir[0])
                return pw->pw_dir;
        }
    }
    const char *h = getenv("HOME");
    if (h && *h)
        return h;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : ".";
}

int util_config_dir(char *out, size_t sz)
{
    int n = snprintf(out, sz, "%s/.config/nsimaj", util_home_dir());
    if (n < 0 || (size_t)n >= sz)
        return -1;
    if (util_mkdir_p(out, 0700) < 0)
        return -1;
    /* Detect root-owned leftover that blocks writes as a normal user */
    if (access(out, W_OK) != 0) {
        fprintf(stderr,
                "[nsimaj] cannot write %s (%s)\n"
                "  fix: sudo chown -R \"$USER\":\"$USER\" %s\n",
                out, strerror(errno), out);
        return -1;
    }
    return 0;
}

void util_hostname(char *out, size_t sz)
{
    /* Prefer /etc/hostname — works on Arch without the hostname binary. */
    char buf[256];
    if (util_read_file("/etc/hostname", buf, sizeof buf) >= 0) {
        util_trim(buf);
        if (buf[0]) {
            snprintf(out, sz, "%s", buf);
            return;
        }
    }
    if (gethostname(out, sz) == 0 && out[0] && strcmp(out, "localhost") != 0) {
        out[sz - 1] = '\0';
        return;
    }
    snprintf(out, sz, "unknown");
}

void util_primary_ipv4(char *out, size_t sz)
{
    out[0] = '\0';
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    /* No packets sent; kernel picks the egress interface for this route. */
    inet_pton(AF_INET, "1.1.1.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        struct sockaddr_in local;
        socklen_t len = sizeof local;
        if (getsockname(fd, (struct sockaddr *)&local, &len) == 0)
            inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)sz);
    }
    close(fd);
}

int util_prompt(const char *prompt, char *out, size_t sz, int hide)
{
    if (!out || sz == 0)
        return -1;
    out[0] = '\0';
    if (!isatty(STDIN_FILENO))
        return -1;

    fputs(prompt, stdout);
    fflush(stdout);

    struct termios oldt, newt;
    int hidden = 0;
    if (hide && tcgetattr(STDIN_FILENO, &oldt) == 0) {
        newt = oldt;
        newt.c_lflag &= ~(tcflag_t)ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt) == 0)
            hidden = 1;
    }

    if (!fgets(out, (int)sz, stdin)) {
        if (hidden)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        if (hide)
            fputc('\n', stdout);
        return -1;
    }
    if (hidden)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
    if (hide)
        fputc('\n', stdout);
    util_trim(out);
    return 0;
}

void util_gen_id(char *out, size_t sz)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(out, sz, "%lx-%x", (unsigned long)ts.tv_sec,
             (unsigned)(ts.tv_nsec & 0xfffffff) ^ (unsigned)getpid());
}

long util_now(void)
{
    return (long)time(NULL);
}

int util_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

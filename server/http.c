#define _DEFAULT_SOURCE
#include "http.h"

#include "../common/frame.h"
#include "../common/util.h"
#include "../protocol/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *guess_ctype(const char *path)
{
    if (strstr(path, ".sh"))
        return "text/x-shellscript";
    if (strstr(path, ".service"))
        return "text/plain";
    return "application/octet-stream";
}

static void http_reply(int fd, int code, const char *ctype, const void *body,
                       size_t len)
{
    char hdr[512];
    const char *reason = code == 200 ? "OK" : "Not Found";
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, ctype, len);
    if (n > 0)
        write(fd, hdr, (size_t)n);
    if (body && len)
        write(fd, body, len);
}

static int serve_file(int fd, const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) {
        http_reply(fd, 404, "text/plain", "not found\n", 10);
        return -1;
    }
    int f = open(path, O_RDONLY);
    if (f < 0) {
        http_reply(fd, 404, "text/plain", "not found\n", 10);
        return -1;
    }
    char *buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(f);
        http_reply(fd, 404, "text/plain", "error\n", 6);
        return -1;
    }
    ssize_t r = read(f, buf, (size_t)st.st_size);
    close(f);
    if (r < 0)
        r = 0;
    http_reply(fd, 200, guess_ctype(path), buf, (size_t)r);
    free(buf);
    return 0;
}

int http_listen(struct fleet *f)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)f->http_port);
    if (inet_pton(AF_INET, f->bind_addr, &addr.sin_addr) != 1)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    f->http_fd = fd;
    return 0;
}

void http_close(struct fleet *f)
{
    if (f->http_fd >= 0) {
        close(f->http_fd);
        f->http_fd = -1;
    }
}

int http_accept_once(struct fleet *f)
{
    if (f->http_fd < 0)
        return 0;
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int cfd = accept(f->http_fd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0)
        return 0;

    char req[2048];
    ssize_t n = read(cfd, req, sizeof req - 1);
    if (n <= 0) {
        close(cfd);
        return 0;
    }
    req[n] = '\0';

    char method[16], path[512];
    if (sscanf(req, "%15s %511s", method, path) != 2) {
        close(cfd);
        return 0;
    }

    const char *file = NULL;
    if (strcmp(path, "/") == 0 || strcmp(path, "/install.sh") == 0)
        file = "install.sh";
    else if (strcmp(path, "/nsimaj-agent") == 0)
        file = "nsimaj-agent";
    else if (strcmp(path, "/nsimaj-agent.service") == 0)
        file = "nsimaj-agent.service";
    else if (strncmp(path, "/", 1) == 0)
        file = path + 1;

    if (!file) {
        http_reply(cfd, 404, "text/plain", "not found\n", 10);
        close(cfd);
        return 0;
    }

    char full[NSIMAJ_PATH_LEN];
    snprintf(full, sizeof full, "%s/%s", f->asset_dir, file);

    /* Dynamic install.sh: inject real server IP / ports / secret defaults */
    if (strcmp(file, "install.sh") == 0) {
        char templ[65536];
        if (util_read_file(full, templ, sizeof templ) >= 0) {
            char server_ip[NSIMAJ_IP_LEN];
            snprintf(server_ip, sizeof server_ip, "%s",
                     f->advertise_ip[0] ? f->advertise_ip : "127.0.0.1");

            /* Prefer Host: header from the client request when present */
            const char *host_hdr = strstr(req, "\nHost:");
            if (!host_hdr)
                host_hdr = strstr(req, "\nhost:");
            if (host_hdr) {
                host_hdr += 6;
                while (*host_hdr == ' ' || *host_hdr == '\t')
                    host_hdr++;
                char hostbuf[128];
                size_t hi = 0;
                while (*host_hdr && *host_hdr != '\r' && *host_hdr != '\n' &&
                       *host_hdr != ':' && hi + 1 < sizeof hostbuf)
                    hostbuf[hi++] = *host_hdr++;
                hostbuf[hi] = '\0';
                if (hostbuf[0] && strcmp(hostbuf, "localhost") != 0 &&
                    strcmp(hostbuf, "127.0.0.1") != 0)
                    snprintf(server_ip, sizeof server_ip, "%s", hostbuf);
            }

            char portbuf[16], httpbuf[16];
            snprintf(portbuf, sizeof portbuf, "%d", f->control_port);
            snprintf(httpbuf, sizeof httpbuf, "%d", f->http_port);

            /* Naive placeholder replace into out buffer */
            char out[65536];
            size_t oi = 0;
            const char *p = templ;
            while (*p && oi + 1 < sizeof out) {
                if (strncmp(p, "__NSIMAJ_SERVER__", 17) == 0) {
                    size_t n = strlen(server_ip);
                    if (oi + n >= sizeof out)
                        break;
                    memcpy(out + oi, server_ip, n);
                    oi += n;
                    p += 17;
                } else if (strncmp(p, "__NSIMAJ_PORT__", 15) == 0) {
                    size_t n = strlen(portbuf);
                    if (oi + n >= sizeof out)
                        break;
                    memcpy(out + oi, portbuf, n);
                    oi += n;
                    p += 15;
                } else if (strncmp(p, "__NSIMAJ_HTTP_PORT__", 20) == 0) {
                    size_t n = strlen(httpbuf);
                    if (oi + n >= sizeof out)
                        break;
                    memcpy(out + oi, httpbuf, n);
                    oi += n;
                    p += 20;
                } else if (strncmp(p, "__NSIMAJ_SECRET__", 17) == 0) {
                    size_t n = strlen(f->secret);
                    if (oi + n >= sizeof out)
                        break;
                    memcpy(out + oi, f->secret, n);
                    oi += n;
                    p += 17;
                } else {
                    out[oi++] = *p++;
                }
            }
            out[oi] = '\0';
            http_reply(cfd, 200, "text/x-shellscript", out, oi);
            close(cfd);
            return 1;
        }
    }

    serve_file(cfd, full);
    close(cfd);
    return 1;
}

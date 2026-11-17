#define _DEFAULT_SOURCE
#include "frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../protocol/protocol.h"

int write_full(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            return -1;
        }
        if (w == 0)
            return -1;
        p += (size_t)w;
        left -= (size_t)w;
    }
    return 0;
}

int read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t left = n;
    while (left) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return 0;
        p += (size_t)r;
        left -= (size_t)r;
    }
    return 1;
}

int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int frame_send(int fd, const void *payload, size_t len)
{
    if (len > NSIMAJ_MAX_FRAME)
        return -1;
    uint32_t be = htonl((uint32_t)len);
    if (write_full(fd, &be, 4) < 0)
        return -1;
    if (len && write_full(fd, payload, len) < 0)
        return -1;
    return 0;
}

int frame_send_str(int fd, const char *json)
{
    if (!json)
        return -1;
    return frame_send(fd, json, strlen(json));
}

int frame_recv(int fd, char **out)
{
    uint32_t be;
    int rc = read_full(fd, &be, 4);
    if (rc <= 0)
        return rc;
    uint32_t len = ntohl(be);
    if (len > NSIMAJ_MAX_FRAME)
        return -1;
    char *buf = malloc(len + 1);
    if (!buf)
        return -1;
    if (len) {
        rc = read_full(fd, buf, len);
        if (rc <= 0) {
            free(buf);
            return rc == 0 ? 0 : -1;
        }
    }
    buf[len] = '\0';
    *out = buf;
    return (int)len;
}

void frame_reader_init(struct frame_reader *r)
{
    r->buf = NULL;
    r->len = 0;
    r->cap = 0;
}

void frame_reader_free(struct frame_reader *r)
{
    free(r->buf);
    r->buf = NULL;
    r->len = 0;
    r->cap = 0;
}

static int reader_ensure(struct frame_reader *r, size_t need)
{
    if (r->cap >= need)
        return 0;
    size_t ncap = r->cap ? r->cap : 4096;
    while (ncap < need)
        ncap *= 2;
    char *nbuf = realloc(r->buf, ncap);
    if (!nbuf)
        return -1;
    r->buf = nbuf;
    r->cap = ncap;
    return 0;
}

int frame_reader_feed(struct frame_reader *r, const void *data, size_t n, char **out)
{
    *out = NULL;
    if (reader_ensure(r, r->len + n) < 0)
        return -1;
    memcpy(r->buf + r->len, data, n);
    r->len += n;

    if (r->len < 4)
        return 0;
    uint32_t be;
    memcpy(&be, r->buf, 4);
    uint32_t plen = ntohl(be);
    if (plen > NSIMAJ_MAX_FRAME)
        return -1;
    if (r->len < 4 + plen)
        return 0;

    char *frame = malloc(plen + 1);
    if (!frame)
        return -1;
    memcpy(frame, r->buf + 4, plen);
    frame[plen] = '\0';

    size_t used = 4 + plen;
    memmove(r->buf, r->buf + used, r->len - used);
    r->len -= used;

    *out = frame;
    return (int)plen;
}

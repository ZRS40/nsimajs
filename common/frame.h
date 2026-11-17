#ifndef NSIMAJ_FRAME_H
#define NSIMAJ_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Length-prefixed frames: 4-byte big-endian length + payload (JSON UTF-8). */

int frame_send(int fd, const void *payload, size_t len);
int frame_send_str(int fd, const char *json);

/* Read one full frame into *out (malloc'd). Returns length, 0 on EOF, -1 on error. */
int frame_recv(int fd, char **out);

/* Non-blocking friendly: append available bytes into buf. When a full frame
 * is ready, sets *out (malloc'd) and returns its length; otherwise 0.
 * Returns -1 on hard error / peer closed with incomplete frame. */
struct frame_reader {
    char *buf;
    size_t len;
    size_t cap;
};

void frame_reader_init(struct frame_reader *r);
void frame_reader_free(struct frame_reader *r);
int frame_reader_feed(struct frame_reader *r, const void *data, size_t n, char **out);

int set_nonblock(int fd);
int read_full(int fd, void *buf, size_t n);
int write_full(int fd, const void *buf, size_t n);

#endif

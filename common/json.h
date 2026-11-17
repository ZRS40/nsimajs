#ifndef NSIMAJ_JSON_H
#define NSIMAJ_JSON_H

#include <stddef.h>
#include <sys/types.h>

/* Minimal JSON helpers for nsimaj control messages. */

int json_get_string(const char *json, const char *key, char *out, size_t out_sz);
int json_get_int(const char *json, const char *key, long *out);
int json_has_key(const char *json, const char *key);

/* Escape src into dst (including surrounding quotes). Returns 0 on success. */
int json_quote(char *dst, size_t dst_sz, const char *src);

/* Build helpers append into buf. Return 0 on success, -1 on overflow. */
struct json_buf {
    char *data;
    size_t len;
    size_t cap;
};

int json_buf_init(struct json_buf *b, size_t cap);
void json_buf_free(struct json_buf *b);
int json_buf_printf(struct json_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int json_buf_add_str(struct json_buf *b, const char *key, const char *val);
int json_buf_add_int(struct json_buf *b, const char *key, long val);
int json_buf_add_raw(struct json_buf *b, const char *key, const char *raw_json);

/* Base64 for PTY payloads inside JSON. */
size_t b64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_sz);
ssize_t b64_decode(const char *in, unsigned char *out, size_t out_sz);

#endif

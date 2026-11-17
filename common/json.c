#define _DEFAULT_SOURCE
#include "json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *find_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        if (p > json && p[-1] == '"' && p[klen] == '"') {
            const char *after = skip_ws(p + klen + 1);
            if (*after == ':')
                return skip_ws(after + 1);
        }
        p++;
    }
    return NULL;
}

int json_has_key(const char *json, const char *key)
{
    return find_key(json, key) != NULL;
}

int json_get_string(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *v = find_key(json, key);
    if (!v || *v != '"')
        return -1;
    v++;
    size_t i = 0;
    while (*v && *v != '"') {
        char c = *v++;
        if (c == '\\' && *v) {
            char e = *v++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            default: c = e; break;
            }
        }
        if (i + 1 >= out_sz)
            return -1;
        out[i++] = c;
    }
    out[i] = '\0';
    return 0;
}

int json_get_int(const char *json, const char *key, long *out)
{
    const char *v = find_key(json, key);
    if (!v)
        return -1;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v)
        return -1;
    *out = n;
    return 0;
}

int json_quote(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz < 3)
        return -1;
    size_t o = 0;
    dst[o++] = '"';
    for (const char *p = src; *p; p++) {
        const char *esc = NULL;
        char tmp[2] = {0};
        switch (*p) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            tmp[0] = *p;
            esc = tmp;
            break;
        }
        size_t el = strlen(esc);
        if (o + el + 2 >= dst_sz)
            return -1;
        memcpy(dst + o, esc, el);
        o += el;
    }
    dst[o++] = '"';
    dst[o] = '\0';
    return 0;
}

int json_buf_init(struct json_buf *b, size_t cap)
{
    b->data = malloc(cap);
    if (!b->data)
        return -1;
    b->len = 0;
    b->cap = cap;
    b->data[0] = '\0';
    return 0;
}

void json_buf_free(struct json_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int json_buf_reserve(struct json_buf *b, size_t need)
{
    if (b->cap >= need)
        return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need)
        ncap *= 2;
    char *n = realloc(b->data, ncap);
    if (!n)
        return -1;
    b->data = n;
    b->cap = ncap;
    return 0;
}

int json_buf_printf(struct json_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return -1;
    }
    if (json_buf_reserve(b, b->len + (size_t)n + 1) < 0) {
        va_end(ap2);
        return -1;
    }
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 0;
}

int json_buf_add_str(struct json_buf *b, const char *key, const char *val)
{
    char q[8192];
    if (json_quote(q, sizeof q, val ? val : "") < 0)
        return -1;
    if (b->len > 1 && b->data[b->len - 1] != '{' && b->data[b->len - 1] != ',')
        if (json_buf_printf(b, ",") < 0)
            return -1;
    return json_buf_printf(b, "\"%s\":%s", key, q);
}

int json_buf_add_int(struct json_buf *b, const char *key, long val)
{
    if (b->len > 1 && b->data[b->len - 1] != '{' && b->data[b->len - 1] != ',')
        if (json_buf_printf(b, ",") < 0)
            return -1;
    return json_buf_printf(b, "\"%s\":%ld", key, val);
}

int json_buf_add_raw(struct json_buf *b, const char *key, const char *raw_json)
{
    if (b->len > 1 && b->data[b->len - 1] != '{' && b->data[b->len - 1] != ',')
        if (json_buf_printf(b, ",") < 0)
            return -1;
    return json_buf_printf(b, "\"%s\":%s", key, raw_json);
}

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const unsigned char *in, size_t in_len, char *out, size_t out_sz)
{
    size_t need = ((in_len + 2) / 3) * 4 + 1;
    if (out_sz < need)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < in_len)
            v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < in_len)
            v |= (unsigned)in[i + 2];
        out[o++] = b64_table[(v >> 18) & 63];
        out[o++] = b64_table[(v >> 12) & 63];
        out[o++] = (i + 1 < in_len) ? b64_table[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < in_len) ? b64_table[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

ssize_t b64_decode(const char *in, unsigned char *out, size_t out_sz)
{
    size_t len = strlen(in);
    size_t o = 0;
    unsigned buf = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=')
            break;
        int v = b64_val(in[i]);
        if (v < 0)
            continue;
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_sz)
                return -1;
            out[o++] = (unsigned char)((buf >> bits) & 0xff);
        }
    }
    return (ssize_t)o;
}

#define _DEFAULT_SOURCE
#include "ctl.h"
#include "selfupdate.h"

#include "../common/json.h"
#include "../common/util.h"
#include "../protocol/protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void ctl_init(struct ctl_server *c)
{
    memset(c, 0, sizeof *c);
    c->listen_fd = -1;
    c->client_fd = -1;
    frame_reader_init(&c->reader);
}

static int send_err(int fd, const char *err)
{
    struct json_buf b;
    if (json_buf_init(&b, 256) < 0)
        return -1;
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", "error");
    json_buf_add_str(&b, "error", err);
    json_buf_printf(&b, "}");
    int rc = frame_send_str(fd, b.data);
    json_buf_free(&b);
    return rc;
}

static int send_ok(int fd)
{
    return frame_send_str(fd, "{\"type\":\"ok\"}");
}

int ctl_listen(struct ctl_server *c)
{
    unlink(NSIMAJ_CTL_SOCK);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", NSIMAJ_CTL_SOCK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("ctl bind");
        close(fd);
        return -1;
    }
    chmod(NSIMAJ_CTL_SOCK, 0666);
    if (listen(fd, 2) < 0) {
        close(fd);
        unlink(NSIMAJ_CTL_SOCK);
        return -1;
    }
    set_nonblock(fd);
    c->listen_fd = fd;
    return 0;
}

void ctl_disconnect_client(struct ctl_server *c)
{
    if (c->client_fd >= 0) {
        close(c->client_fd);
        c->client_fd = -1;
    }
    frame_reader_free(&c->reader);
    frame_reader_init(&c->reader);
    c->client_focus = 0;
}

void ctl_close(struct ctl_server *c)
{
    ctl_disconnect_client(c);
    if (c->listen_fd >= 0) {
        close(c->listen_fd);
        c->listen_fd = -1;
    }
    unlink(NSIMAJ_CTL_SOCK);
}

int ctl_has_client(const struct ctl_server *c)
{
    return c->client_fd >= 0;
}

int ctl_client_focusing(const struct ctl_server *c)
{
    return c->client_fd >= 0 && c->client_focus;
}

void ctl_accept(struct ctl_server *c)
{
    if (c->listen_fd < 0)
        return;
    for (;;) {
        int fd = accept(c->listen_fd, NULL, NULL);
        if (fd < 0)
            break;
        if (c->client_fd >= 0) {
            send_err(fd, "busy");
            close(fd);
            continue;
        }
        set_nonblock(fd);
        c->client_fd = fd;
        c->client_focus = 0;
        frame_reader_free(&c->reader);
        frame_reader_init(&c->reader);
    }
}

void ctl_push_state(struct ctl_server *c, struct fleet *f)
{
    if (c->client_fd < 0 || !f)
        return;

    struct json_buf b;
    if (json_buf_init(&b, 65536) < 0)
        return;
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", "state");
    json_buf_add_str(&b, "status", f->status_line);
    json_buf_add_str(&b, "version", NSIMAJ_VERSION);
    json_buf_add_str(&b, "advertise_ip", f->advertise_ip);
    json_buf_add_int(&b, "control_port", f->control_port);
    json_buf_add_int(&b, "http_port", f->http_port);
    json_buf_add_int(&b, "cursor", f->cursor);
    json_buf_add_int(&b, "online", fleet_online_count(f));
    json_buf_add_int(&b, "confirm", f->confirm_pending);

    int n = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->authenticated)
            continue;
        char k[32];
        snprintf(k, sizeof k, "id%d", n);
        json_buf_add_str(&b, k, a->id);
        snprintf(k, sizeof k, "host%d", n);
        json_buf_add_str(&b, k, a->hostname);
        snprintf(k, sizeof k, "ip%d", n);
        json_buf_add_str(&b, k, a->ip);
        snprintf(k, sizeof k, "distro%d", n);
        json_buf_add_str(&b, k, a->distro);
        snprintf(k, sizeof k, "stage%d", n);
        json_buf_add_str(&b, k, a->job_stage[0] ? a->job_stage : "-");
        snprintf(k, sizeof k, "err%d", n);
        json_buf_add_str(&b, k, a->last_error);
        snprintf(k, sizeof k, "on%d", n);
        json_buf_add_int(&b, k, a->online);
        snprintf(k, sizeof k, "upd%d", n);
        json_buf_add_int(&b, k, a->pending_updates);
        snprintf(k, sizeof k, "sel%d", n);
        json_buf_add_int(&b, k, f->selected[i] ? 1 : 0);
        snprintf(k, sizeof k, "slot%d", n);
        json_buf_add_int(&b, k, i);
        n++;
    }
    json_buf_add_int(&b, "n", n);
    json_buf_printf(&b, "}");

    if (frame_send_str(c->client_fd, b.data) < 0)
        ctl_disconnect_client(c);
    else
        f->dirty = 0;
    json_buf_free(&b);
}

void ctl_send_focus_data(struct ctl_server *c, const unsigned char *data,
                         size_t len)
{
    if (c->client_fd < 0 || !c->client_focus || !data || !len)
        return;
    char b64[NSIMAJ_MAX_FRAME];
    size_t n = b64_encode(data, len, b64, sizeof b64);
    if (!n)
        return;
    struct json_buf b;
    if (json_buf_init(&b, n + 64) < 0)
        return;
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", "focus_data");
    json_buf_add_str(&b, "data", b64);
    json_buf_printf(&b, "}");
    if (frame_send_str(c->client_fd, b.data) < 0)
        ctl_disconnect_client(c);
    json_buf_free(&b);
}

static int visible_slot(struct fleet *f, int nth)
{
    int seen = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        if (!f->agents[i].used || !f->agents[i].authenticated)
            continue;
        if (seen == nth)
            return i;
        seen++;
    }
    return -1;
}

static void handle_ctl_msg(struct ctl_server *c, struct fleet *f,
                           const char *json)
{
    char type[64];
    if (json_get_string(json, "type", type, sizeof type) < 0) {
        send_err(c->client_fd, "bad_type");
        return;
    }

    if (strcmp(type, "hello") == 0 || strcmp(type, "get_state") == 0) {
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "set_cursor") == 0) {
        long n = 0;
        json_get_int(json, "n", &n);
        f->cursor = (int)n;
        fleet_touch(f);
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "toggle_select") == 0) {
        int idx = visible_slot(f, f->cursor);
        if (idx >= 0)
            f->selected[idx] = !f->selected[idx];
        fleet_touch(f);
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "select_all") == 0) {
        int any = fleet_selected_count(f) > 0;
        for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
            if (f->agents[i].used && f->agents[i].authenticated)
                f->selected[i] = any ? 0 : 1;
        fleet_touch(f);
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "start_job") == 0) {
        char kind[32] = "";
        char payload[NSIMAJ_CMD_LEN] = "";
        json_get_string(json, "kind", kind, sizeof kind);
        json_get_string(json, "payload", payload, sizeof payload);
        if (!kind[0])
            snprintf(kind, sizeof kind, "%s", JOB_KIND_UPDATE);
        fleet_start_job(f, kind, payload[0] ? payload : NULL);
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "cancel") == 0) {
        fleet_cancel_jobs(f);
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "focus") == 0) {
        long n = f->cursor;
        json_get_int(json, "n", &n);
        int idx = visible_slot(f, (int)n);
        if (idx >= 0 && fleet_focus(f, idx) == 0) {
            c->client_focus = 1;
            send_ok(c->client_fd);
        } else {
            send_err(c->client_fd, "focus_failed");
        }
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "focus_detach") == 0) {
        fleet_focus_detach(f);
        c->client_focus = 0;
        ctl_push_state(c, f);
        return;
    }
    if (strcmp(type, "focus_stdin") == 0) {
        char b64[NSIMAJ_MAX_FRAME];
        if (json_get_string(json, "data", b64, sizeof b64) == 0) {
            unsigned char raw[NSIMAJ_MAX_FRAME];
            ssize_t dn = b64_decode(b64, raw, sizeof raw);
            if (dn > 0)
                fleet_send_focus_stdin(f, raw, (size_t)dn);
        }
        return;
    }
    if (strcmp(type, "focus_resize") == 0) {
        long rows = 24, cols = 80;
        json_get_int(json, "rows", &rows);
        json_get_int(json, "cols", &cols);
        fleet_send_focus_resize(f, (int)rows, (int)cols);
        return;
    }
    if (strcmp(type, "self_update") == 0) {
        send_ok(c->client_fd);
        char msg[256];
        nsimaj_self_update(msg, sizeof msg);
        f->running = 0;
        return;
    }
    send_err(c->client_fd, "unknown");
}

void ctl_pump(struct ctl_server *c, struct fleet *f)
{
    if (c->client_fd < 0)
        return;
    char tmp[8192];
    ssize_t n = read(c->client_fd, tmp, sizeof tmp);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        ctl_disconnect_client(c);
        return;
    }
    if (n == 0) {
        ctl_disconnect_client(c);
        return;
    }
    char *frame = NULL;
    int flen = frame_reader_feed(&c->reader, tmp, (size_t)n, &frame);
    if (flen < 0) {
        ctl_disconnect_client(c);
        return;
    }
    while (flen > 0) {
        handle_ctl_msg(c, f, frame);
        free(frame);
        frame = NULL;
        if (c->client_fd < 0)
            return;
        flen = frame_reader_feed(&c->reader, "", 0, &frame);
        if (flen < 0) {
            ctl_disconnect_client(c);
            return;
        }
    }
}

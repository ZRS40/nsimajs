#define _DEFAULT_SOURCE
#include "client.h"
#include "ctl.h"
#include "tui.h"

#include "../common/json.h"
#include "../common/util.h"
#include "../protocol/protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int client_ensure_daemon(void)
{
    if (access(NSIMAJ_CTL_SOCK, F_OK) == 0)
        return 0;

    fprintf(stderr, "starting nsimajd service...\n");
    int rc = system("systemctl start nsimajd.service 2>/dev/null");
    (void)rc;

    for (int i = 0; i < 30; i++) {
        if (access(NSIMAJ_CTL_SOCK, F_OK) == 0)
            return 0;
        util_sleep_ms(100);
    }
    fprintf(stderr,
            "nsimajd daemon not available (%s missing).\n"
            "Install/start with: sudo ./bootstrap/install-server.sh\n"
            "Or: sudo systemctl start nsimajd\n",
            NSIMAJ_CTL_SOCK);
    return -1;
}

int client_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", NSIMAJ_CTL_SOCK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void client_apply_state(struct fleet *f, const char *json)
{
    long n = 0, cursor = 0, port = NSIMAJ_CONTROL_PORT, http = NSIMAJ_HTTP_PORT;
    long online = 0, confirm = 0;
    json_get_int(json, "n", &n);
    json_get_int(json, "cursor", &cursor);
    json_get_int(json, "control_port", &port);
    json_get_int(json, "http_port", &http);
    json_get_int(json, "online", &online);
    json_get_int(json, "confirm", &confirm);
    (void)online;

    json_get_string(json, "status", f->status_line, sizeof f->status_line);
    json_get_string(json, "advertise_ip", f->advertise_ip, sizeof f->advertise_ip);
    f->control_port = (int)port;
    f->http_port = (int)http;
    f->cursor = (int)cursor;
    /* confirm stays local on client for UX; ignore daemon confirm */

    /* Clear authenticated rows then refill from indexed keys. */
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        f->agents[i].used = 0;
        f->agents[i].authenticated = 0;
        f->selected[i] = 0;
    }
    f->count = 0;

    if (n > NSIMAJ_MAX_AGENTS)
        n = NSIMAJ_MAX_AGENTS;
    for (int i = 0; i < (int)n; i++) {
        char k[32];
        long slot = i;
        snprintf(k, sizeof k, "slot%d", i);
        json_get_int(json, k, &slot);
        if (slot < 0 || slot >= NSIMAJ_MAX_AGENTS)
            slot = i;
        struct agent *a = &f->agents[slot];
        memset(a, 0, sizeof *a);
        a->used = 1;
        a->authenticated = 1;
        a->fd = -1;
        snprintf(k, sizeof k, "id%d", i);
        json_get_string(json, k, a->id, sizeof a->id);
        snprintf(k, sizeof k, "host%d", i);
        json_get_string(json, k, a->hostname, sizeof a->hostname);
        snprintf(k, sizeof k, "ip%d", i);
        json_get_string(json, k, a->ip, sizeof a->ip);
        snprintf(k, sizeof k, "distro%d", i);
        json_get_string(json, k, a->distro, sizeof a->distro);
        snprintf(k, sizeof k, "stage%d", i);
        json_get_string(json, k, a->job_stage, sizeof a->job_stage);
        snprintf(k, sizeof k, "err%d", i);
        json_get_string(json, k, a->last_error, sizeof a->last_error);
        long on = 0, upd = 0, sel = 0;
        snprintf(k, sizeof k, "on%d", i);
        json_get_int(json, k, &on);
        snprintf(k, sizeof k, "upd%d", i);
        json_get_int(json, k, &upd);
        snprintf(k, sizeof k, "sel%d", i);
        json_get_int(json, k, &sel);
        a->online = (int)on;
        a->pending_updates = (int)upd;
        f->selected[slot] = sel ? 1 : 0;
        f->count++;
    }
    f->dirty = 1;
}

int client_send_json(int fd, const char *json)
{
    return frame_send_str(fd, json);
}

int client_send_hello(int fd)
{
    return client_send_json(fd, "{\"type\":\"hello\"}");
}

int client_send_cmd(int fd, const char *type)
{
    char buf[128];
    snprintf(buf, sizeof buf, "{\"type\":\"%s\"}", type);
    return client_send_json(fd, buf);
}

int client_send_cmd_int(int fd, const char *type, const char *key, long val)
{
    char buf[160];
    snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"%s\":%ld}", type, key, val);
    return client_send_json(fd, buf);
}

int client_send_start_job(int fd, const char *kind, const char *payload)
{
    struct json_buf b;
    if (json_buf_init(&b, 512 + (payload ? strlen(payload) : 0)) < 0)
        return -1;
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", "start_job");
    json_buf_add_str(&b, "kind", kind);
    if (payload && payload[0])
        json_buf_add_str(&b, "payload", payload);
    json_buf_printf(&b, "}");
    int rc = client_send_json(fd, b.data);
    json_buf_free(&b);
    return rc;
}

int client_send_focus_stdin(int fd, const unsigned char *data, size_t len)
{
    char b64[NSIMAJ_MAX_FRAME];
    if (!b64_encode(data, len, b64, sizeof b64))
        return -1;
    struct json_buf b;
    if (json_buf_init(&b, strlen(b64) + 64) < 0)
        return -1;
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", "focus_stdin");
    json_buf_add_str(&b, "data", b64);
    json_buf_printf(&b, "}");
    int rc = client_send_json(fd, b.data);
    json_buf_free(&b);
    return rc;
}

int client_send_focus_resize(int fd, int rows, int cols)
{
    char buf[128];
    snprintf(buf, sizeof buf,
             "{\"type\":\"focus_resize\",\"rows\":%d,\"cols\":%d}", rows, cols);
    return client_send_json(fd, buf);
}

/* ---- TUI client loop (actions go to daemon) ---- */

int client_run_tui(void)
{
    if (client_ensure_daemon() < 0)
        return 1;

    int fd = -1;
    for (int i = 0; i < 20; i++) {
        fd = client_connect();
        if (fd >= 0)
            break;
        util_sleep_ms(100);
    }
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s (is another TUI attached?)\n",
                NSIMAJ_CTL_SOCK);
        return 1;
    }
    set_nonblock(fd);

    struct fleet fleet;
    fleet_init(&fleet);
    fleet.running = 1;

    if (client_send_hello(fd) < 0) {
        close(fd);
        return 1;
    }

    tui_init();
    atexit(tui_restore);

    struct frame_reader reader;
    frame_reader_init(&reader);
    long last_draw = 0;

    while (fleet.running) {
        struct pollfd pfds[2];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[1].fd = fd;
        pfds[1].events = POLLIN;

        int pr = poll(pfds, 2, 200);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pfds[0].revents & POLLIN) {
            unsigned char buf[256];
            ssize_t rn = read(STDIN_FILENO, buf, sizeof buf);
            for (ssize_t i = 0; i < rn; i++)
                tui_handle_input_client(&fleet, buf[i], fd);
        }

        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char tmp[65536];
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n <= 0) {
                fprintf(stderr, "\ndaemon disconnected\n");
                break;
            }
            char *frame = NULL;
            int flen = frame_reader_feed(&reader, tmp, (size_t)n, &frame);
            if (flen < 0)
                break;
            while (flen > 0) {
                char type[64];
                if (json_get_string(frame, "type", type, sizeof type) == 0) {
                    if (strcmp(type, "state") == 0)
                        client_apply_state(&fleet, frame);
                    else if (strcmp(type, "focus_data") == 0) {
                        char b64[NSIMAJ_MAX_FRAME];
                        if (json_get_string(frame, "data", b64, sizeof b64) == 0) {
                            unsigned char raw[NSIMAJ_MAX_FRAME];
                            ssize_t dn = b64_decode(b64, raw, sizeof raw);
                            if (dn > 0)
                                tui_write_focus(raw, (size_t)dn);
                        }
                    } else if (strcmp(type, "error") == 0) {
                        char err[128];
                        if (json_get_string(frame, "error", err, sizeof err) == 0)
                            fleet_set_status(&fleet, "error: %s", err);
                    }
                }
                free(frame);
                frame = NULL;
                flen = frame_reader_feed(&reader, "", 0, &frame);
            }
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long now_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        if (!tui_in_focus() && (fleet.dirty || now_ms - last_draw >= 250)) {
            tui_draw(&fleet);
            fleet.dirty = 0;
            last_draw = now_ms;
        }
    }

    tui_restore();
    frame_reader_free(&reader);
    close(fd);
    return 0;
}

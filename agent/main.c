#define _DEFAULT_SOURCE
#include "jobs.h"
#include "osinfo.h"
#include "state.h"

#include "../common/frame.h"
#include "../common/json.h"
#include "../common/util.h"
#include "../protocol/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static struct agent_state *g_st;

int agent_send_json(struct agent_state *st, struct json_buf *b)
{
    if (st->fd < 0)
        return -1;
    return frame_send_str(st->fd, b->data);
}

int agent_send_event(struct agent_state *st, const char *stage, long exit_code,
                     const char *error)
{
    struct json_buf b;
    if (json_buf_init(&b, 512) < 0)
        return -1;
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_JOB_EVENT);
    json_buf_add_str(&b, "job_id", st->job_id);
    json_buf_add_str(&b, "stage", stage);
    if (exit_code >= 0)
        json_buf_add_int(&b, "exit_code", exit_code);
    if (error && error[0])
        json_buf_add_str(&b, "error", error);
    json_buf_printf(&b, "}");
    int rc = agent_send_json(st, &b);
    json_buf_free(&b);
    return rc;
}

static void on_signal(int sig)
{
    (void)sig;
    if (g_st)
        g_st->running = 0;
}

static int load_config(struct agent_state *st, const char *path)
{
    char buf[4096];
    if (util_read_file(path, buf, sizeof buf) < 0)
        return -1;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        util_trim(line);
        if (line[0] && line[0] != '#') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = line;
                char *val = eq + 1;
                util_trim(key);
                util_trim(val);
                if (strcmp(key, "server") == 0)
                    snprintf(st->server_host, sizeof st->server_host, "%s", val);
                else if (strcmp(key, "port") == 0)
                    st->server_port = atoi(val);
                else if (strcmp(key, "http_port") == 0)
                    st->http_port = atoi(val);
                else if (strcmp(key, "secret") == 0)
                    snprintf(st->secret, sizeof st->secret, "%s", val);
                else if (strcmp(key, "id") == 0)
                    snprintf(st->id, sizeof st->id, "%s", val);
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    return 0;
}

static int connect_server(struct agent_state *st)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", st->server_port);
    if (getaddrinfo(st->server_host, portstr, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

    st->fd = fd;
    frame_reader_init(&st->reader);

    st->pending_updates = osinfo_pending_updates(st->family);

    struct json_buf b;
    json_buf_init(&b, 1024);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_HELLO);
    json_buf_add_str(&b, "secret", st->secret);
    json_buf_add_str(&b, "id", st->id);
    json_buf_add_str(&b, "hostname", st->hostname);
    json_buf_add_str(&b, "ip", st->ip);
    json_buf_add_str(&b, "distro", st->distro);
    json_buf_add_str(&b, "family", st->family);
    json_buf_add_str(&b, "agent_version", NSIMAJ_VERSION);
    json_buf_add_int(&b, "pending_updates", st->pending_updates);
    json_buf_printf(&b, "}");
    if (frame_send_str(fd, b.data) < 0) {
        json_buf_free(&b);
        close(fd);
        st->fd = -1;
        return -1;
    }
    json_buf_free(&b);
    return 0;
}

static void disconnect(struct agent_state *st)
{
    agent_focus_detach(st);
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }
    frame_reader_free(&st->reader);
    frame_reader_init(&st->reader);
}

static int send_heartbeat(struct agent_state *st)
{
    st->pending_updates = osinfo_pending_updates(st->family);
    struct json_buf b;
    json_buf_init(&b, 512);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_HEARTBEAT);
    json_buf_add_str(&b, "id", st->id);
    json_buf_add_str(&b, "distro", st->distro);
    json_buf_add_int(&b, "pending_updates", st->pending_updates);
    json_buf_printf(&b, "}");
    int rc = agent_send_json(st, &b);
    json_buf_free(&b);
    return rc;
}

static void handle_message(struct agent_state *st, const char *json)
{
    char type[64];
    if (json_get_string(json, "type", type, sizeof type) < 0)
        return;

    if (strcmp(type, MSG_JOB_START) == 0) {
        char job_id[NSIMAJ_ID_LEN];
        char kind[32];
        char payload[NSIMAJ_CMD_LEN];
        payload[0] = '\0';
        json_get_string(json, "job_id", job_id, sizeof job_id);
        json_get_string(json, "kind", kind, sizeof kind);
        json_get_string(json, "payload", payload, sizeof payload);
        agent_job_start(st, job_id, kind, payload);
    } else if (strcmp(type, MSG_JOB_CANCEL) == 0) {
        char job_id[NSIMAJ_ID_LEN];
        job_id[0] = '\0';
        json_get_string(json, "job_id", job_id, sizeof job_id);
        agent_job_cancel(st, job_id);
    } else if (strcmp(type, MSG_FOCUS_START) == 0) {
        char job_id[NSIMAJ_ID_LEN];
        job_id[0] = '\0';
        json_get_string(json, "job_id", job_id, sizeof job_id);
        agent_focus_start(st, job_id);
    } else if (strcmp(type, MSG_FOCUS_DETACH) == 0) {
        agent_focus_detach(st);
    } else if (strcmp(type, MSG_FOCUS_STDIN) == 0) {
        char b64[NSIMAJ_MAX_FRAME];
        if (json_get_string(json, "data", b64, sizeof b64) == 0) {
            unsigned char raw[NSIMAJ_MAX_FRAME];
            ssize_t n = b64_decode(b64, raw, sizeof raw);
            if (n > 0)
                agent_focus_stdin(st, raw, (size_t)n);
        }
    } else if (strcmp(type, MSG_FOCUS_RESIZE) == 0) {
        long rows = 24, cols = 80;
        json_get_int(json, "rows", &rows);
        json_get_int(json, "cols", &cols);
        agent_focus_resize(st, (int)rows, (int)cols);
    } else if (strcmp(type, MSG_ERROR) == 0) {
        char err[128];
        if (json_get_string(json, "error", err, sizeof err) == 0)
            fprintf(stderr, "server error: %s\n", err);
        st->running = 0;
    }
}

static void pump_server(struct agent_state *st)
{
    char tmp[8192];
    ssize_t n = read(st->fd, tmp, sizeof tmp);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        disconnect(st);
        return;
    }
    if (n == 0) {
        disconnect(st);
        return;
    }
    char *frame = NULL;
    int flen = frame_reader_feed(&st->reader, tmp, (size_t)n, &frame);
    if (flen < 0) {
        disconnect(st);
        return;
    }
    while (flen > 0) {
        handle_message(st, frame);
        free(frame);
        frame = NULL;
        flen = frame_reader_feed(&st->reader, "", 0, &frame);
        if (flen < 0) {
            disconnect(st);
            return;
        }
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--config PATH] [--server HOST] [--port N] [--secret S]\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct agent_state st;
    memset(&st, 0, sizeof st);
    st.fd = -1;
    st.focus_pty = -1;
    st.running = 1;
    st.server_port = NSIMAJ_CONTROL_PORT;
    st.http_port = NSIMAJ_HTTP_PORT;
    snprintf(st.secret, sizeof st.secret, "nsimaj");
    snprintf(st.server_host, sizeof st.server_host, "127.0.0.1");
    util_hostname(st.hostname, sizeof st.hostname);
    util_gen_id(st.id, sizeof st.id);
    /* Prefer stable id from hostname */
    snprintf(st.id, sizeof st.id, "%s", st.hostname);
    util_primary_ipv4(st.ip, sizeof st.ip);
    osinfo_detect(st.distro, sizeof st.distro, st.family, sizeof st.family);
    frame_reader_init(&st.reader);
    g_st = &st;

    const char *config = "/etc/nsimaj/agent.conf";
    int config_loaded = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config = argv[++i];
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            snprintf(st.server_host, sizeof st.server_host, "%s", argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            st.server_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc)
            snprintf(st.secret, sizeof st.secret, "%s", argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (load_config(&st, config) == 0)
        config_loaded = 1;
    (void)config_loaded;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "nsimaj-agent %s host=%s family=%s -> %s:%d\n",
            NSIMAJ_VERSION, st.hostname, st.family, st.server_host,
            st.server_port);

    int backoff_ms = 1000;
    long last_hb = 0;
    long last_job_poll = 0;

    while (st.running) {
        if (st.fd < 0) {
            fprintf(stderr, "connecting to %s:%d ...\n", st.server_host,
                    st.server_port);
            if (connect_server(&st) < 0) {
                util_sleep_ms(backoff_ms);
                if (backoff_ms < 30000)
                    backoff_ms *= 2;
                continue;
            }
            set_nonblock(st.fd);
            backoff_ms = 1000;
            last_hb = util_now();
            fprintf(stderr, "connected, waiting for orders\n");
        }

        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds].fd = st.fd;
        pfds[nfds].events = POLLIN;
        nfds++;
        if (st.focus_pty >= 0) {
            pfds[nfds].fd = st.focus_pty;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int pr = poll(pfds, (nfds_t)nfds, 500);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            disconnect(&st);
            continue;
        }

        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))
            pump_server(&st);

        if (st.focus_pty >= 0)
            agent_focus_poll(&st);

        long now = util_now();
        if (st.fd >= 0 && now - last_hb >= NSIMAJ_HEARTBEAT_SEC) {
            if (send_heartbeat(&st) < 0)
                disconnect(&st);
            last_hb = now;
        }
        if (now != last_job_poll) {
            agent_job_poll(&st);
            last_job_poll = now;
        }
    }

    disconnect(&st);
    fprintf(stderr, "nsimaj-agent stopped\n");
    return 0;
}

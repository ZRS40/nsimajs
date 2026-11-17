#define _DEFAULT_SOURCE
#include "fleet.h"
#include "http.h"
#include "tui.h"

#include "../common/firewall.h"
#include "../common/frame.h"
#include "../common/json.h"
#include "../common/util.h"
#include "../protocol/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static struct fleet *g_fleet;
static int g_headless;

static void on_signal(int sig)
{
    (void)sig;
    if (g_fleet)
        g_fleet->running = 0;
}

static void headless_status(struct fleet *f)
{
    fprintf(stderr, "[fleet] online=%d status=%s\n", fleet_online_count(f),
            f->status_line);
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->authenticated)
            continue;
        fprintf(stderr,
                "  - %s ip=%s distro=%s upd=%d stage=%s online=%d err=%s\n",
                a->hostname, a->ip, a->distro, a->pending_updates, a->job_stage,
                a->online, a->last_error[0] ? a->last_error : "-");
    }
}

static int control_listen(struct fleet *f)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)f->control_port);
    if (inet_pton(AF_INET, f->bind_addr, &addr.sin_addr) != 1)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind control");
        close(fd);
        return -1;
    }
    if (listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    f->listen_fd = fd;
    return 0;
}

static void accept_agents(struct fleet *f)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int cfd = accept(f->listen_fd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0)
            break;
        set_nonblock(cfd);
        char ip[NSIMAJ_IP_LEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        struct agent *a = fleet_alloc(f, cfd, ip);
        if (!a) {
            close(cfd);
            fleet_set_status(f, "Agent table full");
        }
    }
}

static void check_offline(struct fleet *f)
{
    long now = util_now();
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->authenticated)
            continue;
        if (a->online && now - a->last_seen > NSIMAJ_OFFLINE_SEC)
            fleet_disconnect(f, a);
    }
}

static void pump_agent(struct fleet *f, struct agent *a)
{
    if (!a || !a->used || a->fd < 0)
        return;

    char tmp[8192];
    ssize_t n = read(a->fd, tmp, sizeof tmp);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        fleet_disconnect(f, a);
        return;
    }
    if (n == 0) {
        fleet_disconnect(f, a);
        return;
    }

    char *frame = NULL;
    int flen = frame_reader_feed(&a->reader, tmp, (size_t)n, &frame);
    if (flen < 0) {
        fleet_disconnect(f, a);
        return;
    }
    while (flen > 0) {
        int special = fleet_handle_message(f, a, frame);
        if (special < 0) {
            free(frame);
            /* Auth / protocol rejection: drop the slot. */
            fleet_remove(f, a);
            return;
        }
        if (special == 2) {
            /* HELLO merged into an existing roster slot; drain leftover frames. */
            char id[NSIMAJ_ID_LEN];
            id[0] = '\0';
            json_get_string(frame, "id", id, sizeof id);
            free(frame);
            frame = NULL;
            struct agent *ex = id[0] ? fleet_find_id(f, id) : NULL;
            if (ex && ex->used) {
                int more = frame_reader_feed(&ex->reader, "", 0, &frame);
                while (more > 0) {
                    int sp = fleet_handle_message(f, ex, frame);
                    free(frame);
                    frame = NULL;
                    if (sp < 0) {
                        fleet_remove(f, ex);
                        return;
                    }
                    more = frame_reader_feed(&ex->reader, "", 0, &frame);
                }
                if (more < 0)
                    fleet_disconnect(f, ex);
            }
            return;
        }
        if (special == 1 && tui_in_focus() && f->focus_agent >= 0 &&
            &f->agents[f->focus_agent] == a) {
            char b64[NSIMAJ_MAX_FRAME];
            if (json_get_string(frame, "data", b64, sizeof b64) == 0) {
                unsigned char raw[NSIMAJ_MAX_FRAME];
                ssize_t dn = b64_decode(b64, raw, sizeof raw);
                if (dn > 0)
                    tui_write_focus(raw, (size_t)dn);
            }
        }
        free(frame);
        frame = NULL;
        if (!a->used || a->fd < 0)
            return;
        flen = frame_reader_feed(&a->reader, "", 0, &frame);
        if (flen < 0) {
            fleet_disconnect(f, a);
            return;
        }
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--secret SECRET] [--port N] [--http-port N]\n"
            "          [--assets DIR] [--bind ADDR] [--advertise-ip IP]\n"
            "          [--headless] [-y]\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct fleet fleet;
    fleet_init(&fleet);
    g_fleet = &fleet;

    char cwd[NSIMAJ_PATH_LEN];
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, ".");
    if ((size_t)snprintf(fleet.asset_dir, sizeof fleet.asset_dir, "%s/bootstrap",
                         cwd) >= sizeof fleet.asset_dir)
        snprintf(fleet.asset_dir, sizeof fleet.asset_dir, "./bootstrap");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
            snprintf(fleet.secret, sizeof fleet.secret, "%s", argv[++i]);
            fleet.secret_from_cli = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            fleet.control_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc)
            fleet.http_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc)
            snprintf(fleet.asset_dir, sizeof fleet.asset_dir, "%s", argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            snprintf(fleet.bind_addr, sizeof fleet.bind_addr, "%s", argv[++i]);
        else if (strcmp(argv[i], "--advertise-ip") == 0 && i + 1 < argc)
            snprintf(fleet.advertise_ip, sizeof fleet.advertise_ip, "%s",
                     argv[++i]);
        else if (strcmp(argv[i], "--headless") == 0)
            g_headless = 1;
        else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
            ; /* skip prompts: used with existing config / defaults */
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    int skip_prompt = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0 ||
            strcmp(argv[i], "--headless") == 0)
            skip_prompt = 1;

    fleet_load_config(&fleet);
    if (!fleet.advertise_ip[0] && !fleet.iface[0])
        util_primary_ipv4(fleet.advertise_ip, sizeof fleet.advertise_ip);

    if (!skip_prompt) {
        if (fleet_setup_interactive(&fleet) < 0)
            return 1;
    } else {
        if (!fleet.secret[0]) {
            snprintf(fleet.secret, sizeof fleet.secret, "nsimaj");
            fprintf(stderr, "using default secret 'nsimaj' (set with --secret)\n");
        }
        if (!fleet.advertise_ip[0])
            util_primary_ipv4(fleet.advertise_ip, sizeof fleet.advertise_ip);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (control_listen(&fleet) < 0)
        return 1;
    if (http_listen(&fleet) < 0)
        fprintf(stderr, "warning: HTTP bootstrap port %d unavailable\n",
                fleet.http_port);

    /* Open control + bootstrap ports in ufw when present */
    firewall_allow_tcp(fleet.control_port, "nsimaj-control");
    firewall_allow_tcp(fleet.http_port, "nsimaj-bootstrap");

    if (!g_headless) {
        tui_init();
        atexit(tui_restore);
    } else {
        fprintf(stderr, "nsimajd headless on :%d (http :%d) advertise=%s\n",
                fleet.control_port, fleet.http_port,
                fleet.advertise_ip[0] ? fleet.advertise_ip : "?");
        if (fleet.advertise_ip[0])
            fprintf(stderr,
                    "install: curl -fsSL http://%s:%d/install.sh | sudo bash\n",
                    fleet.advertise_ip, fleet.http_port);
        fprintf(stderr, "commands: update | exec <cmd> | cancel | quit\n");
        set_nonblock(STDIN_FILENO);
    }

    long last_draw = 0; /* monotonic ms */
    while (fleet.running) {
        struct pollfd pfds[NSIMAJ_MAX_AGENTS + 4];
        int nfds = 0;
        int stdin_i = -1;

        pfds[nfds].fd = STDIN_FILENO;
        pfds[nfds].events = POLLIN;
        stdin_i = nfds++;

        pfds[nfds].fd = fleet.listen_fd;
        pfds[nfds].events = POLLIN;
        int listen_i = nfds++;

        int http_i = -1;
        if (fleet.http_fd >= 0) {
            pfds[nfds].fd = fleet.http_fd;
            pfds[nfds].events = POLLIN;
            http_i = nfds++;
        }

        int agent_base = nfds;
        int agent_map[NSIMAJ_MAX_AGENTS];
        int n_agents = 0;
        for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
            if (!fleet.agents[i].used || fleet.agents[i].fd < 0)
                continue;
            pfds[nfds].fd = fleet.agents[i].fd;
            pfds[nfds].events = POLLIN;
            agent_map[n_agents++] = i;
            nfds++;
        }

        int pr = poll(pfds, (nfds_t)nfds, 200);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (stdin_i >= 0 && (pfds[stdin_i].revents & POLLIN)) {
            if (g_headless) {
                char line[NSIMAJ_CMD_LEN];
                ssize_t rn = read(STDIN_FILENO, line, sizeof line - 1);
                if (rn > 0) {
                    line[rn] = '\0';
                    util_trim(line);
                    if (strcmp(line, "quit") == 0 || strcmp(line, "q") == 0)
                        fleet.running = 0;
                    else if (strcmp(line, "update") == 0)
                        fleet_start_job(&fleet, JOB_KIND_UPDATE, NULL);
                    else if (strncmp(line, "exec ", 5) == 0)
                        fleet_start_job(&fleet, JOB_KIND_EXEC, line + 5);
                    else if (strcmp(line, "cancel") == 0)
                        fleet_cancel_jobs(&fleet);
                    else if (strcmp(line, "svc") == 0 ||
                             strcmp(line, "service") == 0)
                        fprintf(stderr,
                                "svc: systemctl status|restart nsimajd | "
                                "start nsimajd-update | "
                                "status nsimajd-update.timer\n");
                    else if (line[0])
                        fprintf(stderr, "unknown command: %s\n", line);
                }
            } else {
                unsigned char buf[256];
                ssize_t rn = read(STDIN_FILENO, buf, sizeof buf);
                for (ssize_t i = 0; i < rn; i++)
                    tui_handle_input(&fleet, buf[i]);
            }
        }

        if (pfds[listen_i].revents & POLLIN)
            accept_agents(&fleet);

        if (http_i >= 0 && (pfds[http_i].revents & POLLIN))
            http_accept_once(&fleet);

        for (int i = 0; i < n_agents; i++) {
            if (pfds[agent_base + i].revents & (POLLIN | POLLHUP | POLLERR))
                pump_agent(&fleet, &fleet.agents[agent_map[i]]);
        }

        check_offline(&fleet);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long now_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;

        if (g_headless) {
            if (fleet.dirty || now_ms - last_draw >= 1000) {
                headless_status(&fleet);
                fleet.dirty = 0;
                last_draw = now_ms;
            }
        } else if (!tui_in_focus()) {
            /* Redraw immediately on fleet changes, and at least ~4 Hz */
            if (fleet.dirty || now_ms - last_draw >= 250) {
                tui_draw(&fleet);
                fleet.dirty = 0;
                last_draw = now_ms;
            }
        }
    }

    if (!g_headless)
        tui_restore();
    http_close(&fleet);
    if (fleet.listen_fd >= 0)
        close(fleet.listen_fd);
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
        if (fleet.agents[i].used)
            fleet_remove(&fleet, &fleet.agents[i]);

    printf("nsimajd stopped.\n");
    return 0;
}

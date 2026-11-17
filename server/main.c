#define _DEFAULT_SOURCE
#include "client.h"
#include "ctl.h"
#include "fleet.h"
#include "http.h"
#include "selfupdate.h"
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
static struct ctl_server g_ctl;

static void on_signal(int sig)
{
    (void)sig;
    if (g_fleet)
        g_fleet->running = 0;
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
            fleet_remove(f, a);
            return;
        }
        if (special == 2) {
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
        if (special == 1 && ctl_client_focusing(&g_ctl) && f->focus_agent >= 0 &&
            &f->agents[f->focus_agent] == a) {
            char b64[NSIMAJ_MAX_FRAME];
            if (json_get_string(frame, "data", b64, sizeof b64) == 0) {
                unsigned char raw[NSIMAJ_MAX_FRAME];
                ssize_t dn = b64_decode(b64, raw, sizeof raw);
                if (dn > 0)
                    ctl_send_focus_data(&g_ctl, raw, (size_t)dn);
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
            "Usage: %s [update] [--daemon] [--secret SECRET] [--port N]\n"
            "          [--http-port N] [--assets DIR] [--bind ADDR]\n"
            "          [--advertise-ip IP] [-y]\n"
            "\n"
            "  (no args)  Attach TUI to the running daemon (starts service if needed)\n"
            "  update     Self-update server from latest git tag (needs sudo)\n"
            "  --daemon   Run control plane (systemd); owns ports + agents\n",
            argv0);
}

static int run_daemon(struct fleet *fleet)
{
    g_fleet = fleet;
    ctl_init(&g_ctl);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (control_listen(fleet) < 0)
        return 1;
    if (http_listen(fleet) < 0)
        fprintf(stderr, "warning: HTTP bootstrap port %d unavailable\n",
                fleet->http_port);
    if (ctl_listen(&g_ctl) < 0) {
        fprintf(stderr, "warning: control socket %s unavailable\n", NSIMAJ_CTL_SOCK);
    }

    firewall_allow_tcp(fleet->control_port, "nsimaj-control");
    firewall_allow_tcp(fleet->http_port, "nsimaj-bootstrap");

    fprintf(stderr, "nsimajd daemon on :%d (http :%d) ctl=%s advertise=%s\n",
            fleet->control_port, fleet->http_port, NSIMAJ_CTL_SOCK,
            fleet->advertise_ip[0] ? fleet->advertise_ip : "?");
    if (fleet->advertise_ip[0])
        fprintf(stderr,
                "install: curl -fsSL http://%s:%d/install.sh | sudo bash\n",
                fleet->advertise_ip, fleet->http_port);

    while (fleet->running) {
        struct pollfd pfds[NSIMAJ_MAX_AGENTS + 8];
        int nfds = 0;

        pfds[nfds].fd = fleet->listen_fd;
        pfds[nfds].events = POLLIN;
        int listen_i = nfds++;

        int http_i = -1;
        if (fleet->http_fd >= 0) {
            pfds[nfds].fd = fleet->http_fd;
            pfds[nfds].events = POLLIN;
            http_i = nfds++;
        }

        int ctl_listen_i = -1;
        if (g_ctl.listen_fd >= 0) {
            pfds[nfds].fd = g_ctl.listen_fd;
            pfds[nfds].events = POLLIN;
            ctl_listen_i = nfds++;
        }

        int ctl_client_i = -1;
        if (g_ctl.client_fd >= 0) {
            pfds[nfds].fd = g_ctl.client_fd;
            pfds[nfds].events = POLLIN;
            ctl_client_i = nfds++;
        }

        int agent_base = nfds;
        int agent_map[NSIMAJ_MAX_AGENTS];
        int n_agents = 0;
        for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
            if (!fleet->agents[i].used || fleet->agents[i].fd < 0)
                continue;
            pfds[nfds].fd = fleet->agents[i].fd;
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

        if (pfds[listen_i].revents & POLLIN)
            accept_agents(fleet);

        if (http_i >= 0 && (pfds[http_i].revents & POLLIN))
            http_accept_once(fleet);

        if (ctl_listen_i >= 0 && (pfds[ctl_listen_i].revents & POLLIN))
            ctl_accept(&g_ctl);

        if (ctl_client_i >= 0 &&
            (pfds[ctl_client_i].revents & (POLLIN | POLLHUP | POLLERR)))
            ctl_pump(&g_ctl, fleet);

        for (int i = 0; i < n_agents; i++) {
            if (pfds[agent_base + i].revents & (POLLIN | POLLHUP | POLLERR))
                pump_agent(fleet, &fleet->agents[agent_map[i]]);
        }

        check_offline(fleet);

        if (fleet->dirty && ctl_has_client(&g_ctl))
            ctl_push_state(&g_ctl, fleet);
    }

    ctl_close(&g_ctl);
    http_close(fleet);
    if (fleet->listen_fd >= 0)
        close(fleet->listen_fd);
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
        if (fleet->agents[i].used)
            fleet_remove(fleet, &fleet->agents[i]);

    fprintf(stderr, "nsimajd daemon stopped.\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "update") == 0) {
        char msg[256];
        if (geteuid() != 0)
            fprintf(stderr,
                    "hint: self-update usually needs root (sudo nsimajd update)\n");
        int rc = nsimaj_self_update(msg, sizeof msg);
        if (msg[0])
            fprintf(stderr, "%s\n", msg);
        return rc == 0 ? 0 : 1;
    }

    int daemon_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "--headless") == 0)
            daemon_mode = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    /* Default: TUI client attached to daemon — never binds agent ports. */
    if (!daemon_mode)
        return client_run_tui();

    struct fleet fleet;
    fleet_init(&fleet);

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
        else if (strcmp(argv[i], "--daemon") == 0 ||
                 strcmp(argv[i], "--headless") == 0 ||
                 strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
            ;
        else {
            usage(argv[0]);
            return 1;
        }
    }

    fleet_load_config(&fleet);
    if (!fleet.advertise_ip[0] && !fleet.iface[0])
        util_primary_ipv4(fleet.advertise_ip, sizeof fleet.advertise_ip);

    if (!fleet.secret[0]) {
        snprintf(fleet.secret, sizeof fleet.secret, "nsimaj");
        fprintf(stderr, "using default secret 'nsimaj' (set with --secret)\n");
    }
    if (!fleet.advertise_ip[0])
        util_primary_ipv4(fleet.advertise_ip, sizeof fleet.advertise_ip);

    return run_daemon(&fleet);
}

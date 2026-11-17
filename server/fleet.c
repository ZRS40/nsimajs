#define _DEFAULT_SOURCE
#include "fleet.h"

#include "../common/firewall.h"
#include "../common/json.h"
#include "../common/util.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void fleet_init(struct fleet *f)
{
    memset(f, 0, sizeof *f);
    f->secret[0] = '\0';
    snprintf(f->bind_addr, sizeof f->bind_addr, "0.0.0.0");
    f->advertise_ip[0] = '\0';
    f->iface[0] = '\0';
    f->config_path[0] = '\0';
    f->control_port = NSIMAJ_CONTROL_PORT;
    f->http_port = NSIMAJ_HTTP_PORT;
    f->listen_fd = -1;
    f->http_fd = -1;
    f->focus_agent = -1;
    f->running = 1;
    f->secret_from_cli = 0;
    f->dirty = 1;
    fleet_set_status(f, "Ready. Agents will appear when they connect.");
}

void fleet_touch(struct fleet *f)
{
    f->dirty = 1;
}

static int fleet_config_path(struct fleet *f, char *out, size_t sz)
{
    char dir[NSIMAJ_PATH_LEN];
    if (util_config_dir(dir, sizeof dir) < 0)
        return -1;
    if ((size_t)snprintf(out, sz, "%s/server.conf", dir) >= sz)
        return -1;
    if (f)
        snprintf(f->config_path, sizeof f->config_path, "%s", out);
    return 0;
}

int fleet_config_ready(const struct fleet *f)
{
    return f->secret[0] && f->advertise_ip[0];
}

int fleet_load_config(struct fleet *f)
{
    char path[NSIMAJ_PATH_LEN];
    char buf[4096];
    if (fleet_config_path(f, path, sizeof path) < 0)
        return -1;
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
                if (strcmp(key, "secret") == 0 && !f->secret_from_cli)
                    snprintf(f->secret, sizeof f->secret, "%s", val);
                else if (strcmp(key, "port") == 0)
                    f->control_port = atoi(val);
                else if (strcmp(key, "http_port") == 0)
                    f->http_port = atoi(val);
                else if (strcmp(key, "bind") == 0)
                    snprintf(f->bind_addr, sizeof f->bind_addr, "%s", val);
                else if (strcmp(key, "advertise_ip") == 0)
                    snprintf(f->advertise_ip, sizeof f->advertise_ip, "%s", val);
                else if (strcmp(key, "iface") == 0)
                    snprintf(f->iface, sizeof f->iface, "%s", val);
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    return 0;
}

int fleet_save_config(const struct fleet *f)
{
    char path[NSIMAJ_PATH_LEN];
    char dir[NSIMAJ_PATH_LEN];
    if (util_config_dir(dir, sizeof dir) < 0)
        return -1;
    if ((size_t)snprintf(path, sizeof path, "%s/server.conf", dir) >= sizeof path)
        return -1;
    char body[1024];
    snprintf(body, sizeof body,
             "secret=%s\n"
             "port=%d\n"
             "http_port=%d\n"
             "bind=%s\n"
             "advertise_ip=%s\n"
             "iface=%s\n",
             f->secret, f->control_port, f->http_port, f->bind_addr,
             f->advertise_ip, f->iface);
    if (util_write_file(path, body, 0600) < 0) {
        fprintf(stderr, "[nsimaj] failed to write %s\n", path);
        return -1;
    }
    return 0;
}

#define MAX_IFACES 32

struct iface_entry {
    char name[IFNAMSIZ];
    char ip[NSIMAJ_IP_LEN];
};

static int list_ifaces(struct iface_entry *out, int max)
{
    struct ifaddrs *list = NULL, *ifa;
    if (getifaddrs(&list) < 0)
        return -1;
    int count = 0;
    for (ifa = list; ifa && count < max; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        int dup = 0;
        for (int i = 0; i < count; i++)
            if (strcmp(out[i].name, ifa->ifa_name) == 0) {
                dup = 1;
                break;
            }
        if (dup)
            continue;
        snprintf(out[count].name, sizeof out[count].name, "%s", ifa->ifa_name);
        inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                  out[count].ip, sizeof out[count].ip);
        count++;
    }
    freeifaddrs(list);
    return count;
}

static int pick_interface(struct fleet *f)
{
    struct iface_entry ifaces[MAX_IFACES];
    int count = list_ifaces(ifaces, MAX_IFACES);
    if (count <= 0) {
        fprintf(stderr, "No active IPv4 interface found.\n");
        util_primary_ipv4(f->advertise_ip, sizeof f->advertise_ip);
        return f->advertise_ip[0] ? 0 : -1;
    }

    printf("\nAvailable network interfaces:\n\n");
    for (int i = 0; i < count; i++)
        printf(" [%d] %-12s %s\n", i + 1, ifaces[i].name, ifaces[i].ip);

    int choice = 1;
    if (count == 1) {
        printf("\nAutomatically selected: %s (%s)\n", ifaces[0].name,
               ifaces[0].ip);
    } else {
        /* Prefer previously saved iface if still present */
        if (f->iface[0]) {
            for (int i = 0; i < count; i++)
                if (strcmp(ifaces[i].name, f->iface) == 0) {
                    choice = i + 1;
                    break;
                }
        }
        printf("\nSelect an interface (1-%d) [%d]: ", count, choice);
        fflush(stdout);
        char line[64];
        if (fgets(line, sizeof line, stdin)) {
            util_trim(line);
            if (line[0]) {
                int n = atoi(line);
                if (n >= 1 && n <= count)
                    choice = n;
                else {
                    fprintf(stderr, "Invalid selection.\n");
                    return -1;
                }
            }
        }
    }

    snprintf(f->iface, sizeof f->iface, "%s", ifaces[choice - 1].name);
    snprintf(f->advertise_ip, sizeof f->advertise_ip, "%s",
             ifaces[choice - 1].ip);
    return 0;
}

int fleet_setup_interactive(struct fleet *f)
{
    if (!isatty(STDIN_FILENO)) {
        if (!f->secret[0])
            snprintf(f->secret, sizeof f->secret, "nsimaj");
        if (!f->advertise_ip[0])
            util_primary_ipv4(f->advertise_ip, sizeof f->advertise_ip);
        return 0;
    }

    /* Already configured: start immediately, no re-prompt */
    if (fleet_config_ready(f)) {
        char path[NSIMAJ_PATH_LEN];
        if (fleet_config_path(f, path, sizeof path) == 0)
            printf("Loaded config from %s\n", path);
        printf("Using iface=%s ip=%s (secret saved)\n",
               f->iface[0] ? f->iface : "?", f->advertise_ip);
        printf("On each PC: curl -fsSL http://%s:%d/install.sh | sudo bash\n\n",
               f->advertise_ip, f->http_port);
        return 0;
    }

    printf("nsimaj server setup (first run)\n");

    if (pick_interface(f) < 0 || !f->advertise_ip[0]) {
        fprintf(stderr, "advertise IP required\n");
        return -1;
    }
    printf("Advertise IP: %s\n", f->advertise_ip);

    if (!f->secret[0]) {
        if (util_prompt("Join secret (shared with all PCs): ", f->secret,
                        sizeof f->secret, 1) < 0 ||
            !f->secret[0]) {
            fprintf(stderr, "secret required\n");
            return -1;
        }
    }

    char path[NSIMAJ_PATH_LEN];
    if (fleet_save_config(f) == 0) {
        if (fleet_config_path(f, path, sizeof path) == 0)
            printf("Saved %s\n", path);
    } else {
        fprintf(stderr, "ERROR: could not save server.conf — secret will be lost\n");
        return -1;
    }

    char buf[64];
    printf("Open ufw ports %d (control) and %d (bootstrap)? [Y/n]: ",
           f->control_port, f->http_port);
    fflush(stdout);
    if (fgets(buf, sizeof buf, stdin)) {
        util_trim(buf);
        if (!(buf[0] == 'n' || buf[0] == 'N')) {
            char cmd[256];
            snprintf(cmd, sizeof cmd,
                     "ufw allow %d/tcp comment 'nsimaj-control' && "
                     "ufw allow %d/tcp comment 'nsimaj-bootstrap'",
                     f->control_port, f->http_port);
            if (geteuid() == 0)
                system(cmd);
            else {
                char sudo_cmd[300];
                snprintf(sudo_cmd, sizeof sudo_cmd, "sudo %s", cmd);
                if (system(sudo_cmd) != 0)
                    fprintf(stderr,
                            "ufw failed — run manually:\n  sudo ufw allow %d/tcp\n  "
                            "sudo ufw allow %d/tcp\n",
                            f->control_port, f->http_port);
                else
                    printf("ufw rules added.\n");
            }
        }
    }

    printf("\nOn each PC run:\n  curl -fsSL http://%s:%d/install.sh | sudo bash\n\n",
           f->advertise_ip, f->http_port);
    return 0;
}

struct agent *fleet_alloc(struct fleet *f, int fd, const char *ip)
{
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        if (!f->agents[i].used) {
            struct agent *a = &f->agents[i];
            memset(a, 0, sizeof *a);
            a->used = 1;
            a->fd = fd;
            a->online = 1;
            a->last_seen = util_now();
            a->job_exit = -1;
            a->focusing = 0;
            frame_reader_init(&a->reader);
            if (ip)
                snprintf(a->ip, sizeof a->ip, "%s", ip);
            snprintf(a->hostname, sizeof a->hostname, "(connecting)");
            snprintf(a->id, sizeof a->id, "pending-%d", i);
            f->count++;
            fleet_touch(f);
            return a;
        }
    }
    return NULL;
}

struct agent *fleet_find_id(struct fleet *f, const char *id)
{
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
        if (f->agents[i].used && strcmp(f->agents[i].id, id) == 0)
            return &f->agents[i];
    return NULL;
}

struct agent *fleet_find_fd(struct fleet *f, int fd)
{
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
        if (f->agents[i].used && f->agents[i].fd == fd)
            return &f->agents[i];
    return NULL;
}

void fleet_mark_offline(struct fleet *f, struct agent *a)
{
    if (!a || !a->used)
        return;
    if (f->focus_agent >= 0 && &f->agents[f->focus_agent] == a)
        f->focus_agent = -1;
    a->online = 0;
    a->focusing = 0;
    a->authenticated = 0;
}

void fleet_remove(struct fleet *f, struct agent *a)
{
    if (!a || !a->used)
        return;
    if (a->fd >= 0) {
        close(a->fd);
        a->fd = -1;
    }
    frame_reader_free(&a->reader);
    fleet_mark_offline(f, a);
    a->used = 0;
    if (f->count > 0)
        f->count--;
    fleet_touch(f);
}

int fleet_online_count(const struct fleet *f)
{
    int n = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
        if (f->agents[i].used && f->agents[i].online && f->agents[i].authenticated)
            n++;
    return n;
}

void fleet_clear_selection(struct fleet *f)
{
    memset(f->selected, 0, sizeof f->selected);
}

int fleet_selected_count(const struct fleet *f)
{
    int n = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
        if (f->agents[i].used && f->agents[i].authenticated && f->selected[i])
            n++;
    return n;
}

void fleet_set_status(struct fleet *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->status_line, sizeof f->status_line, fmt, ap);
    va_end(ap);
    fleet_touch(f);
}

int fleet_save_history(struct fleet *f, const char *line)
{
    (void)f;
    char dir[NSIMAJ_PATH_LEN];
    char path[NSIMAJ_PATH_LEN + 32];
    FILE *fp = NULL;
    if (util_config_dir(dir, sizeof dir) == 0) {
        snprintf(path, sizeof path, "%s/jobs.log", dir);
        fp = fopen(path, "a");
    }
    if (!fp)
        fp = fopen("/tmp/nsimaj-jobs.log", "a");
    if (!fp)
        return -1;
    fprintf(fp, "%ld %s\n", util_now(), line);
    fclose(fp);
    return 0;
}

static int send_json(struct agent *a, struct json_buf *b)
{
    if (json_buf_printf(b, "}") < 0)
        return -1;
    return frame_send_str(a->fd, b->data);
}

int fleet_handle_message(struct fleet *f, struct agent *a, const char *json)
{
    char type[64];
    if (json_get_string(json, "type", type, sizeof type) < 0)
        return -1;

    a->last_seen = util_now();
    a->online = 1;

    if (strcmp(type, MSG_HELLO) == 0) {
        char secret[NSIMAJ_SECRET_LEN];
        if (json_get_string(json, "secret", secret, sizeof secret) < 0 ||
            strcmp(secret, f->secret) != 0) {
            struct json_buf b;
            json_buf_init(&b, 256);
            json_buf_printf(&b, "{");
            json_buf_add_str(&b, "type", MSG_ERROR);
            json_buf_add_str(&b, "error", "bad_secret");
            send_json(a, &b);
            json_buf_free(&b);
            return -1;
        }
        json_get_string(json, "id", a->id, sizeof a->id);
        json_get_string(json, "hostname", a->hostname, sizeof a->hostname);
        json_get_string(json, "distro", a->distro, sizeof a->distro);
        json_get_string(json, "family", a->family, sizeof a->family);
        json_get_string(json, "agent_version", a->agent_ver, sizeof a->agent_ver);
        char ip[NSIMAJ_IP_LEN];
        if (json_get_string(json, "ip", ip, sizeof ip) == 0 && ip[0])
            snprintf(a->ip, sizeof a->ip, "%s", ip);
        long pend = 0;
        if (json_get_int(json, "pending_updates", &pend) == 0)
            a->pending_updates = (int)pend;
        a->authenticated = 1;
        fleet_touch(f);
        fleet_set_status(f, "Agent online: %s (%s)", a->hostname, a->ip);

        struct json_buf b;
        json_buf_init(&b, 256);
        json_buf_printf(&b, "{");
        json_buf_add_str(&b, "type", MSG_OK);
        send_json(a, &b);
        json_buf_free(&b);
        return 0;
    }

    if (!a->authenticated)
        return -1;

    if (strcmp(type, MSG_HEARTBEAT) == 0) {
        long pend = 0;
        if (json_get_int(json, "pending_updates", &pend) == 0)
            a->pending_updates = (int)pend;
        char distro[NSIMAJ_DISTRO_LEN];
        if (json_get_string(json, "distro", distro, sizeof distro) == 0)
            snprintf(a->distro, sizeof a->distro, "%s", distro);
        a->online = 1;
        fleet_touch(f);
        return 0;
    }

    if (strcmp(type, MSG_JOB_EVENT) == 0) {
        char job_id[NSIMAJ_ID_LEN];
        char stage[NSIMAJ_STAGE_LEN];
        json_get_string(json, "job_id", job_id, sizeof job_id);
        if (json_get_string(json, "stage", stage, sizeof stage) == 0)
            snprintf(a->job_stage, sizeof a->job_stage, "%s", stage);
        if (job_id[0])
            snprintf(a->job_id, sizeof a->job_id, "%s", job_id);
        long code = -1;
        if (json_get_int(json, "exit_code", &code) == 0)
            a->job_exit = code;
        char err[256];
        if (json_get_string(json, "error", err, sizeof err) == 0)
            snprintf(a->last_error, sizeof a->last_error, "%s", err);
        char line[512];
        snprintf(line, sizeof line, "%s %s stage=%s exit=%ld", a->hostname,
                 a->job_id, a->job_stage, a->job_exit);
        fleet_save_history(f, line);
        fleet_touch(f);
        return 0;
    }

    if (strcmp(type, MSG_FOCUS_DATA) == 0) {
        /* Handled by TUI focus mode via callback path — store not needed. */
        return 1; /* special: focus data available in json */
    }

    if (strcmp(type, MSG_FOCUS_END) == 0) {
        a->focusing = 0;
        if (f->focus_agent >= 0 && &f->agents[f->focus_agent] == a)
            f->focus_agent = -1;
        fleet_set_status(f, "Focus ended on %s", a->hostname);
        return 0;
    }

    return 0;
}

int fleet_start_job(struct fleet *f, const char *kind, const char *payload)
{
    int started = 0;
    char job_id[NSIMAJ_ID_LEN];
    util_gen_id(job_id, sizeof job_id);

    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->online || !a->authenticated)
            continue;
        if (fleet_selected_count(f) > 0 && !f->selected[i])
            continue;

        struct json_buf b;
        if (json_buf_init(&b, 1024) < 0)
            continue;
        json_buf_printf(&b, "{");
        json_buf_add_str(&b, "type", MSG_JOB_START);
        json_buf_add_str(&b, "job_id", job_id);
        json_buf_add_str(&b, "kind", kind);
        if (payload && payload[0])
            json_buf_add_str(&b, "payload", payload);
        if (json_buf_printf(&b, "}") < 0) {
            json_buf_free(&b);
            continue;
        }
        if (frame_send_str(a->fd, b.data) == 0) {
            snprintf(a->job_id, sizeof a->job_id, "%s", job_id);
            snprintf(a->job_kind, sizeof a->job_kind, "%s", kind);
            snprintf(a->job_stage, sizeof a->job_stage, "%s", STAGE_QUEUED);
            a->job_exit = -1;
            a->last_error[0] = '\0';
            started++;
        }
        json_buf_free(&b);
    }

    char hist[256];
    snprintf(hist, sizeof hist, "dispatch kind=%s job=%s hosts=%d", kind, job_id,
             started);
    fleet_save_history(f, hist);
    fleet_set_status(f, "Started %s on %d host(s) [%s]", kind, started, job_id);
    return started;
}

int fleet_cancel_jobs(struct fleet *f)
{
    int n = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->online || !a->authenticated)
            continue;
        if (!a->job_id[0])
            continue;
        if (fleet_selected_count(f) > 0 && !f->selected[i])
            continue;
        struct json_buf b;
        json_buf_init(&b, 256);
        json_buf_printf(&b, "{");
        json_buf_add_str(&b, "type", MSG_JOB_CANCEL);
        json_buf_add_str(&b, "job_id", a->job_id);
        json_buf_printf(&b, "}");
        if (frame_send_str(a->fd, b.data) == 0)
            n++;
        json_buf_free(&b);
    }
    fleet_set_status(f, "Cancel sent to %d host(s)", n);
    return n;
}

int fleet_focus(struct fleet *f, int index)
{
    if (index < 0 || index >= NSIMAJ_MAX_AGENTS)
        return -1;
    struct agent *a = &f->agents[index];
    if (!a->used || !a->online || !a->authenticated)
        return -1;
    if (!a->job_id[0]) {
        fleet_set_status(f, "No active job on %s — starting a shell session tag",
                         a->hostname);
    }

    char job_id[NSIMAJ_ID_LEN];
    if (a->job_id[0])
        snprintf(job_id, sizeof job_id, "%s", a->job_id);
    else {
        util_gen_id(job_id, sizeof job_id);
        snprintf(a->job_id, sizeof a->job_id, "%s", job_id);
        snprintf(a->job_kind, sizeof a->job_kind, "%s", JOB_KIND_EXEC);
        snprintf(a->job_stage, sizeof a->job_stage, "%s", STAGE_RUNNING);
        /* Ask agent to start an interactive shell job if none */
        struct json_buf jb;
        json_buf_init(&jb, 512);
        json_buf_printf(&jb, "{");
        json_buf_add_str(&jb, "type", MSG_JOB_START);
        json_buf_add_str(&jb, "job_id", job_id);
        json_buf_add_str(&jb, "kind", JOB_KIND_EXEC);
        json_buf_add_str(&jb, "payload", "bash");
        json_buf_printf(&jb, "}");
        frame_send_str(a->fd, jb.data);
        json_buf_free(&jb);
        /* Give the agent a moment to create the tmux session before attach */
        util_sleep_ms(400);
    }

    struct json_buf b;
    json_buf_init(&b, 256);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_FOCUS_START);
    json_buf_add_str(&b, "job_id", job_id);
    json_buf_printf(&b, "}");
    if (frame_send_str(a->fd, b.data) < 0) {
        json_buf_free(&b);
        return -1;
    }
    json_buf_free(&b);
    a->focusing = 1;
    f->focus_agent = index;
    fleet_set_status(f, "Focused %s — Ctrl-N to detach", a->hostname);
    return 0;
}

int fleet_focus_detach(struct fleet *f)
{
    if (f->focus_agent < 0)
        return 0;
    struct agent *a = &f->agents[f->focus_agent];
    struct json_buf b;
    json_buf_init(&b, 128);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_FOCUS_DETACH);
    json_buf_printf(&b, "}");
    if (a->used && a->fd >= 0)
        frame_send_str(a->fd, b.data);
    json_buf_free(&b);
    a->focusing = 0;
    f->focus_agent = -1;
    fleet_set_status(f, "Detached. Job keeps running in tmux.");
    return 0;
}

int fleet_send_focus_stdin(struct fleet *f, const unsigned char *data, size_t len)
{
    if (f->focus_agent < 0)
        return -1;
    struct agent *a = &f->agents[f->focus_agent];
    char encoded[NSIMAJ_MAX_FRAME];
    if (!b64_encode(data, len, encoded, sizeof encoded))
        return -1;
    struct json_buf b;
    json_buf_init(&b, 1024 + len * 2);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_FOCUS_STDIN);
    json_buf_add_str(&b, "data", encoded);
    json_buf_printf(&b, "}");
    int rc = frame_send_str(a->fd, b.data);
    json_buf_free(&b);
    return rc;
}

int fleet_send_focus_resize(struct fleet *f, int rows, int cols)
{
    if (f->focus_agent < 0)
        return -1;
    struct agent *a = &f->agents[f->focus_agent];
    struct json_buf b;
    json_buf_init(&b, 128);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_FOCUS_RESIZE);
    json_buf_add_int(&b, "rows", rows);
    json_buf_add_int(&b, "cols", cols);
    json_buf_printf(&b, "}");
    int rc = frame_send_str(a->fd, b.data);
    json_buf_free(&b);
    return rc;
}

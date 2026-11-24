#define _DEFAULT_SOURCE
#include "fleet.h"

#include "../common/firewall.h"
#include "../common/json.h"
#include "../common/log.h"
#include "../common/util.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* System-wide config used by the root systemd daemon. */
#define NSIMAJ_SYSTEM_CONF "/etc/nsimaj/server.conf"

static int fleet_config_path(struct fleet *f, char *out, size_t sz)
{
    if (access(NSIMAJ_SYSTEM_CONF, R_OK) == 0 || geteuid() == 0) {
        if ((size_t)snprintf(out, sz, "%s", NSIMAJ_SYSTEM_CONF) >= sz)
            return -1;
        if (f)
            snprintf(f->config_path, sizeof f->config_path, "%s", out);
        return 0;
    }
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

static int try_read_conf(const char *path, char *buf, size_t sz)
{
    return path && path[0] && util_read_file(path, buf, sz) >= 0 ? 0 : -1;
}

int fleet_load_config(struct fleet *f)
{
    char path[NSIMAJ_PATH_LEN];
    char buf[4096];
    path[0] = '\0';

    if (try_read_conf(NSIMAJ_SYSTEM_CONF, buf, sizeof buf) == 0) {
        snprintf(path, sizeof path, "%s", NSIMAJ_SYSTEM_CONF);
    } else {
        /* Fallbacks when /etc is missing (first boot / pre-install). */
        const char *candidates[4];
        int nc = 0;
        const char *git_user = getenv("NSIMAJ_GIT_USER");
        const char *sudo_user = getenv("SUDO_USER");
        static char p0[NSIMAJ_PATH_LEN], p1[NSIMAJ_PATH_LEN], p2[NSIMAJ_PATH_LEN];
        if (git_user && git_user[0] && strcmp(git_user, "root") != 0) {
            snprintf(p0, sizeof p0, "/home/%s/.config/nsimaj/server.conf", git_user);
            candidates[nc++] = p0;
        }
        if (sudo_user && sudo_user[0] && strcmp(sudo_user, "root") != 0) {
            snprintf(p1, sizeof p1, "/home/%s/.config/nsimaj/server.conf", sudo_user);
            candidates[nc++] = p1;
        }
        {
            char dir[NSIMAJ_PATH_LEN];
            if (util_config_dir(dir, sizeof dir) == 0) {
                snprintf(p2, sizeof p2, "%s/server.conf", dir);
                candidates[nc++] = p2;
            }
        }
        int found = 0;
        for (int i = 0; i < nc; i++) {
            if (try_read_conf(candidates[i], buf, sizeof buf) == 0) {
                snprintf(path, sizeof path, "%s", candidates[i]);
                found = 1;
                break;
            }
        }
        if (!found)
            return -1;
    }
    if (f)
        snprintf(f->config_path, sizeof f->config_path, "%s", path);
    fprintf(stderr, "Loaded config from %s\n", path);
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

    int ok = 0;
    if (geteuid() == 0) {
        util_mkdir_p("/etc/nsimaj", 0755);
        if (util_write_file(NSIMAJ_SYSTEM_CONF, body, 0600) == 0)
            ok = 1;
        else
            fprintf(stderr, "[nsimaj] failed to write %s\n", NSIMAJ_SYSTEM_CONF);
    }
    char dir[NSIMAJ_PATH_LEN];
    if (util_config_dir(dir, sizeof dir) == 0) {
        if ((size_t)snprintf(path, sizeof path, "%s/server.conf", dir) < sizeof path) {
            if (util_write_file(path, body, 0600) == 0)
                ok = 1;
            else
                fprintf(stderr, "[nsimaj] failed to write %s\n", path);
        }
    }
    return ok ? 0 : -1;
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
}

void fleet_disconnect(struct fleet *f, struct agent *a)
{
    if (!a || !a->used)
        return;
    /* Never authenticated: drop the pending slot entirely. */
    if (!a->authenticated) {
        fleet_remove(f, a);
        return;
    }
    char label[NSIMAJ_HOST_LEN];
    snprintf(label, sizeof label, "%s",
             a->hostname[0] ? a->hostname : (a->ip[0] ? a->ip : a->id));
    if (a->fd >= 0) {
        close(a->fd);
        a->fd = -1;
    }
    frame_reader_free(&a->reader);
    frame_reader_init(&a->reader);
    fleet_mark_offline(f, a);
    fleet_touch(f);
    fleet_set_status(f, "Agent offline: %s", label);
    nsimaj_log_warn("agent offline host=%s", label);
}

void fleet_remove(struct fleet *f, struct agent *a)
{
    if (!a || !a->used)
        return;
    int idx = (int)(a - f->agents);
    if (a->fd >= 0) {
        close(a->fd);
        a->fd = -1;
    }
    frame_reader_free(&a->reader);
    fleet_mark_offline(f, a);
    a->authenticated = 0;
    a->used = 0;
    if (idx >= 0 && idx < NSIMAJ_MAX_AGENTS)
        f->selected[idx] = 0;
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

int fleet_agent_job_busy(const struct agent *a)
{
    if (!a || !a->job_id[0] || !a->job_stage[0])
        return 0;
    if (strcmp(a->job_stage, STAGE_DONE) == 0 ||
        strcmp(a->job_stage, STAGE_FAILED) == 0 ||
        strcmp(a->job_stage, STAGE_CANCELLED) == 0 ||
        strcmp(a->job_stage, "-") == 0)
        return 0;
    return 1;
}

int fleet_visible_sorted(struct fleet *f, int *idxs, int max)
{
    int n = 0;
    if (!f || !idxs || max <= 0)
        return 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        if (!f->agents[i].used || !f->agents[i].authenticated)
            continue;
        if (n < max)
            idxs[n] = i;
        n++;
    }
    if (n > max)
        n = max;
    for (int i = 1; i < n; i++) {
        int key = idxs[i];
        int j = i - 1;
        while (j >= 0 &&
               strcasecmp(f->agents[idxs[j]].hostname,
                          f->agents[key].hostname) > 0) {
            idxs[j + 1] = idxs[j];
            j--;
        }
        idxs[j + 1] = key;
    }
    return n;
}

void fleet_queue_clear(struct agent *a)
{
    if (!a)
        return;
    a->queue_len = 0;
    a->queue_head = 0;
    memset(a->queue, 0, sizeof a->queue);
}

static int queue_enqueue(struct agent *a, const char *kind, const char *payload)
{
    if (!a || a->queue_len >= NSIMAJ_QUEUE_CAP)
        return -1;
    int idx = (a->queue_head + a->queue_len) % NSIMAJ_QUEUE_CAP;
    struct pending_job *pj = &a->queue[idx];
    snprintf(pj->kind, sizeof pj->kind, "%s", kind ? kind : JOB_KIND_UPDATE);
    pj->payload[0] = '\0';
    if (payload && payload[0])
        snprintf(pj->payload, sizeof pj->payload, "%s", payload);
    pj->queued_at = util_now();
    a->queue_len++;
    return 0;
}

static int queue_dequeue(struct agent *a, char *kind, size_t kind_sz,
                         char *payload, size_t payload_sz)
{
    if (!a || a->queue_len <= 0)
        return -1;
    struct pending_job *pj = &a->queue[a->queue_head];
    if (kind && kind_sz)
        snprintf(kind, kind_sz, "%s", pj->kind);
    if (payload && payload_sz)
        snprintf(payload, payload_sz, "%s", pj->payload);
    memset(pj, 0, sizeof *pj);
    a->queue_head = (a->queue_head + 1) % NSIMAJ_QUEUE_CAP;
    a->queue_len--;
    return 0;
}

static int dispatch_job_to_agent(struct fleet *f, struct agent *a,
                                 const char *kind, const char *payload,
                                 const char *job_id)
{
    if (!a || !a->online || a->fd < 0)
        return -1;
    struct json_buf b;
    if (json_buf_init(&b, 1024) < 0)
        return -1;
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_JOB_START);
    json_buf_add_str(&b, "job_id", job_id);
    json_buf_add_str(&b, "kind", kind);
    if (payload && payload[0])
        json_buf_add_str(&b, "payload", payload);
    if (json_buf_printf(&b, "}") < 0) {
        json_buf_free(&b);
        return -1;
    }
    int rc = frame_send_str(a->fd, b.data);
    json_buf_free(&b);
    if (rc != 0) {
        snprintf(a->last_error, sizeof a->last_error, "send failed");
        snprintf(a->job_stage, sizeof a->job_stage, "%s", STAGE_FAILED);
        nsimaj_log_error("job send failed host=%s kind=%s job=%s", a->hostname,
                         kind, job_id);
        fleet_set_status(f, "Send failed to %s", a->hostname);
        return -1;
    }
    snprintf(a->job_id, sizeof a->job_id, "%s", job_id);
    snprintf(a->job_kind, sizeof a->job_kind, "%s", kind);
    snprintf(a->job_stage, sizeof a->job_stage, "%s", STAGE_QUEUED);
    a->job_exit = -1;
    a->last_error[0] = '\0';
    nsimaj_log_info("job started host=%s kind=%s job=%s", a->hostname, kind,
                    job_id);
    return 0;
}

int fleet_flush_queue(struct fleet *f, struct agent *a)
{
    if (!f || !a || !a->used || !a->authenticated || !a->online)
        return 0;
    if (fleet_agent_job_busy(a) || a->queue_len <= 0)
        return 0;
    char kind[32];
    char payload[NSIMAJ_QUEUE_PAYLOAD];
    if (queue_dequeue(a, kind, sizeof kind, payload, sizeof payload) < 0)
        return 0;
    char job_id[NSIMAJ_ID_LEN];
    util_gen_id(job_id, sizeof job_id);
    nsimaj_log_info("flush queue host=%s kind=%s remaining=%d", a->hostname,
                    kind, a->queue_len);
    if (dispatch_job_to_agent(f, a, kind, payload[0] ? payload : NULL, job_id) <
        0)
        return -1;
    fleet_set_status(f, "Dequeued %s for %s (%d pending)", kind, a->hostname,
                     a->queue_len);
    return 1;
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
            nsimaj_log_warn("hello rejected bad_secret from %s", a->ip);
            struct json_buf b;
            json_buf_init(&b, 256);
            json_buf_printf(&b, "{");
            json_buf_add_str(&b, "type", MSG_ERROR);
            json_buf_add_str(&b, "error", "bad_secret");
            send_json(a, &b);
            json_buf_free(&b);
            return -1;
        }

        char new_id[NSIMAJ_ID_LEN];
        new_id[0] = '\0';
        json_get_string(json, "id", new_id, sizeof new_id);

        int merged = 0;
        /*
         * Reuse the existing roster slot for this agent id. Move the new
         * connection onto that slot and free the temporary pending row.
         * Returns 2 so the caller stops using the pending pointer.
         */
        if (new_id[0]) {
            struct agent *ex = fleet_find_id(f, new_id);
            if (ex && ex != a) {
                if (ex->fd >= 0 && ex->fd != a->fd) {
                    close(ex->fd);
                    ex->fd = -1;
                }
                frame_reader_free(&ex->reader);
                ex->fd = a->fd;
                ex->reader = a->reader;
                snprintf(ex->ip, sizeof ex->ip, "%s", a->ip);

                /* Detach pending without closing the transferred fd/reader. */
                a->fd = -1;
                memset(&a->reader, 0, sizeof a->reader);
                a->authenticated = 0;
                a->used = 0;
                {
                    int ai = (int)(a - f->agents);
                    if (ai >= 0 && ai < NSIMAJ_MAX_AGENTS)
                        f->selected[ai] = 0;
                }
                if (f->count > 0)
                    f->count--;

                a = ex;
                merged = 1;
            }
        }

        snprintf(a->id, sizeof a->id, "%s", new_id[0] ? new_id : a->id);
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
        a->online = 1;
        a->last_seen = util_now();
        fleet_touch(f);
        fleet_set_status(f, "Agent online: %s (%s)", a->hostname, a->ip);
        nsimaj_log_info("agent online host=%s ip=%s id=%s queued=%d",
                        a->hostname, a->ip, a->id, a->queue_len);

        struct json_buf b;
        json_buf_init(&b, 256);
        json_buf_printf(&b, "{");
        json_buf_add_str(&b, "type", MSG_OK);
        send_json(a, &b);
        json_buf_free(&b);
        fleet_flush_queue(f, a);
        return merged ? 2 : 0;
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
        if (strcmp(a->job_stage, STAGE_FAILED) == 0) {
            if (a->last_error[0])
                fleet_set_status(f, "Failed on %s: %s", a->hostname,
                                 a->last_error);
            else
                fleet_set_status(f, "Failed on %s (exit %ld)", a->hostname,
                                 a->job_exit);
            nsimaj_log_error("job failed host=%s job=%s exit=%ld err=%s",
                             a->hostname, a->job_id, a->job_exit,
                             a->last_error[0] ? a->last_error : "-");
            fleet_flush_queue(f, a);
        } else if (strcmp(a->job_stage, STAGE_DONE) == 0) {
            if (strcmp(a->job_kind, JOB_KIND_UPDATE) == 0)
                a->last_update_at = util_now();
            fleet_set_status(f, "Done on %s — update finished OK", a->hostname);
            nsimaj_log_info("job done host=%s job=%s kind=%s", a->hostname,
                            a->job_id, a->job_kind);
            fleet_flush_queue(f, a);
        } else if (strcmp(a->job_stage, STAGE_CANCELLED) == 0) {
            nsimaj_log_info("job cancelled host=%s job=%s", a->hostname,
                            a->job_id);
            fleet_flush_queue(f, a);
        } else if (strcmp(a->job_stage, "installing_agent") == 0) {
            fleet_set_status(f, "%s: refreshing agent binary…", a->hostname);
        } else if (strcmp(a->job_stage, STAGE_UPGRADING) == 0) {
            fleet_set_status(f, "%s: upgrading packages…", a->hostname);
        } else if (strcmp(a->job_stage, STAGE_UPDATING_INDEXES) == 0) {
            fleet_set_status(f, "%s: refreshing package indexes…", a->hostname);
        } else if (strcmp(a->job_stage, STAGE_PREPARING) == 0) {
            fleet_set_status(f, "%s: preparing…", a->hostname);
        } else if (a->job_stage[0]) {
            fleet_set_status(f, "%s: %s", a->hostname, a->job_stage);
        }
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
    int queued = 0;
    int selected = fleet_selected_count(f) > 0;

    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->authenticated)
            continue;
        if (selected && !f->selected[i])
            continue;

        if (!a->online || fleet_agent_job_busy(a)) {
            if (queue_enqueue(a, kind, payload) == 0) {
                queued++;
                snprintf(a->job_stage, sizeof a->job_stage, "%s", STAGE_QUEUED);
                nsimaj_log_info("job queued host=%s kind=%s pending=%d online=%d",
                                a->hostname, kind, a->queue_len, a->online);
            } else {
                nsimaj_log_warn("queue full host=%s kind=%s", a->hostname, kind);
                fleet_set_status(f, "Queue full on %s", a->hostname);
            }
            continue;
        }

        char job_id[NSIMAJ_ID_LEN];
        util_gen_id(job_id, sizeof job_id);
        if (dispatch_job_to_agent(f, a, kind, payload, job_id) == 0)
            started++;
    }

    char hist[256];
    snprintf(hist, sizeof hist, "dispatch kind=%s started=%d queued=%d", kind,
             started, queued);
    fleet_save_history(f, hist);
    if (started > 0 || queued > 0)
        fleet_set_status(f, "Started %s on %d host(s), queued %d", kind, started,
                         queued);
    else
        fleet_set_status(f, "No hosts for %s", kind);
    nsimaj_log_info("dispatch kind=%s started=%d queued=%d", kind, started,
                    queued);
    return started + queued;
}

int fleet_cancel_jobs(struct fleet *f)
{
    int n = 0;
    int cleared = 0;
    int selected = fleet_selected_count(f) > 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->authenticated)
            continue;
        if (selected && !f->selected[i])
            continue;
        if (a->queue_len > 0) {
            cleared += a->queue_len;
            fleet_queue_clear(a);
        }
        if (!a->online || !a->job_id[0] || !fleet_agent_job_busy(a))
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
    fleet_set_status(f, "Cancel sent to %d host(s), cleared %d queued", n,
                     cleared);
    nsimaj_log_info("cancel sent=%d queue_cleared=%d", n, cleared);
    return n;
}

int fleet_focus(struct fleet *f, int index)
{
    if (index < 0 || index >= NSIMAJ_MAX_AGENTS)
        return -1;
    struct agent *a = &f->agents[index];
    if (!a->used || !a->online || !a->authenticated) {
        nsimaj_log_warn("focus refused host=%s online=%d auth=%d",
                        a->used ? a->hostname : "?", a->online,
                        a->authenticated);
        fleet_set_status(f, "Cannot focus: agent offline or missing");
        return -1;
    }

    char job_id[NSIMAJ_ID_LEN];
    /* Reattach running job; if finished/idle, open a fresh shell tmux. */
    if (a->job_id[0] && fleet_agent_job_busy(a)) {
        snprintf(job_id, sizeof job_id, "%s", a->job_id);
        nsimaj_log_info("focus attach existing job host=%s job=%s", a->hostname,
                        job_id);
    } else {
        util_gen_id(job_id, sizeof job_id);
        snprintf(a->job_id, sizeof a->job_id, "%s", job_id);
        snprintf(a->job_kind, sizeof a->job_kind, "%s", JOB_KIND_EXEC);
        snprintf(a->job_stage, sizeof a->job_stage, "%s", STAGE_RUNNING);
        a->job_exit = -1;
        a->last_error[0] = '\0';
        fleet_set_status(f, "Starting shell on %s…", a->hostname);
        nsimaj_log_info("focus start shell host=%s job=%s", a->hostname, job_id);
        struct json_buf jb;
        json_buf_init(&jb, 512);
        json_buf_printf(&jb, "{");
        json_buf_add_str(&jb, "type", MSG_JOB_START);
        json_buf_add_str(&jb, "job_id", job_id);
        json_buf_add_str(&jb, "kind", JOB_KIND_EXEC);
        json_buf_add_str(&jb, "payload", "bash");
        json_buf_printf(&jb, "}");
        if (frame_send_str(a->fd, jb.data) < 0) {
            json_buf_free(&jb);
            nsimaj_log_error("focus job_start send failed host=%s", a->hostname);
            fleet_set_status(f, "Focus failed: could not start shell on %s",
                             a->hostname);
            return -1;
        }
        json_buf_free(&jb);
        util_sleep_ms(500);
    }

    struct json_buf b;
    json_buf_init(&b, 256);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_FOCUS_START);
    json_buf_add_str(&b, "job_id", job_id);
    json_buf_printf(&b, "}");
    if (frame_send_str(a->fd, b.data) < 0) {
        json_buf_free(&b);
        nsimaj_log_error("focus_start send failed host=%s", a->hostname);
        fleet_set_status(f, "Focus failed: send error to %s", a->hostname);
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

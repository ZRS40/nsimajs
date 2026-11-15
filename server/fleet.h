#ifndef NSIMAJ_FLEET_H
#define NSIMAJ_FLEET_H

#include "../common/frame.h"
#include "../protocol/protocol.h"

#include <stddef.h>

struct agent {
    int fd;
    int used;
    int authenticated;
    char id[NSIMAJ_ID_LEN];
    char hostname[NSIMAJ_HOST_LEN];
    char ip[NSIMAJ_IP_LEN];
    char distro[NSIMAJ_DISTRO_LEN];
    char family[32];
    char agent_ver[32];
    int online;
    int pending_updates;
    char job_id[NSIMAJ_ID_LEN];
    char job_kind[32];
    char job_stage[NSIMAJ_STAGE_LEN];
    long job_exit;
    char last_error[256];
    long last_seen;
    int focusing;
    struct frame_reader reader;
};

struct fleet {
    struct agent agents[NSIMAJ_MAX_AGENTS];
    int count;
    char secret[NSIMAJ_SECRET_LEN];
    char bind_addr[64];
    char advertise_ip[NSIMAJ_IP_LEN]; /* shown in install curl hint */
    char iface[32];                   /* selected network interface name */
    int control_port;
    int http_port;
    char asset_dir[NSIMAJ_PATH_LEN];
    int secret_from_cli; /* skip interactive prompt / overwrite carefully */
    char config_path[NSIMAJ_PATH_LEN]; /* last loaded/saved path for messages */
    int listen_fd;
    int http_fd;
    int focus_agent; /* index or -1 */
    int running;
    int confirm_pending; /* 1 if waiting for update confirm */
    char status_line[256];
    int selected[NSIMAJ_MAX_AGENTS];
    int cursor;
    int dirty; /* TUI should redraw */
};

void fleet_touch(struct fleet *f);

void fleet_init(struct fleet *f);
int fleet_load_config(struct fleet *f);
int fleet_save_config(const struct fleet *f);
/* 1 if secret+advertise_ip already present (can skip setup). */
int fleet_config_ready(const struct fleet *f);
int fleet_setup_interactive(struct fleet *f);
struct agent *fleet_alloc(struct fleet *f, int fd, const char *ip);
struct agent *fleet_find_id(struct fleet *f, const char *id);
struct agent *fleet_find_fd(struct fleet *f, int fd);
void fleet_remove(struct fleet *f, struct agent *a);
void fleet_mark_offline(struct fleet *f, struct agent *a);
int fleet_online_count(const struct fleet *f);
void fleet_clear_selection(struct fleet *f);
int fleet_selected_count(const struct fleet *f);

int fleet_handle_message(struct fleet *f, struct agent *a, const char *json);
int fleet_start_job(struct fleet *f, const char *kind, const char *payload);
int fleet_cancel_jobs(struct fleet *f);
int fleet_focus(struct fleet *f, int index);
int fleet_focus_detach(struct fleet *f);
int fleet_send_focus_stdin(struct fleet *f, const unsigned char *data, size_t len);
int fleet_send_focus_resize(struct fleet *f, int rows, int cols);

int fleet_save_history(struct fleet *f, const char *line);
void fleet_set_status(struct fleet *f, const char *fmt, ...);

#endif

#ifndef NSIMAJ_CLIENT_H
#define NSIMAJ_CLIENT_H

#include "fleet.h"

#include "../common/frame.h"

/* Ensure systemd unit is active and /run/nsimajd.sock exists. Returns 0 on ok. */
int client_ensure_daemon(void);

/* Connect to control socket. Returns fd or -1. */
int client_connect(void);

/* Apply a "state" JSON frame into local fleet mirror (visible rows only). */
void client_apply_state(struct fleet *f, const char *json);

/* Send a ctl JSON command (adds framing). Returns 0 on ok. */
int client_send_json(int fd, const char *json);

int client_send_hello(int fd);
int client_send_cmd(int fd, const char *type);
int client_send_cmd_int(int fd, const char *type, const char *key, long val);
int client_send_start_job(int fd, const char *kind, const char *payload);
int client_send_focus_stdin(int fd, const unsigned char *data, size_t len);
int client_send_focus_resize(int fd, int rows, int cols);

/* Interactive TUI attached to daemon. Returns process exit code. */
int client_run_tui(void);

#endif

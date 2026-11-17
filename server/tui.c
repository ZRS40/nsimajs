#define _DEFAULT_SOURCE
#include "tui.h"
#include "client.h"

#include "../common/util.h"
#include "../protocol/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_term;
static int raw_mode;
static int focus_mode;
static int cmd_mode;
static char cmd_buf[NSIMAJ_CMD_LEN];
static size_t cmd_len;

static void get_winsize(int *rows, int *cols);

void tui_init(void)
{
    if (tcgetattr(STDIN_FILENO, &orig_term) == 0) {
        struct termios raw = orig_term;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        raw_mode = 1;
    }
    write(STDOUT_FILENO, "\033[?1049h\033[2J\033[H\033[?25l", 19);
}

void tui_restore(void)
{
    if (raw_mode)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    write(STDOUT_FILENO, "\033[?1049l\033[?25h\033[0m", 18);
    raw_mode = 0;
    focus_mode = 0;
}

int tui_in_focus(void)
{
    return focus_mode;
}

void tui_enter_focus_client(struct fleet *f, int ctl_fd)
{
    (void)f;
    focus_mode = 1;
    write(STDOUT_FILENO, "\033[2J\033[H\033[?25h", 12);
    const char *hint =
        "\r\n--- nsimaj focus (Ctrl-N to detach) ---\r\n"
        "Watch for UPDATE COMPLETE; agent restarts a few seconds after.\r\n"
        "----------------------------------------\r\n";
    write(STDOUT_FILENO, hint, strlen(hint));
    int rows = 24, cols = 80;
    get_winsize(&rows, &cols);
    client_send_focus_resize(ctl_fd, rows, cols);
}

void tui_leave_focus(struct fleet *f)
{
    (void)f;
    focus_mode = 0;
    write(STDOUT_FILENO, "\033[2J\033[H\033[?25l", 12);
}

void tui_write_focus(const unsigned char *data, size_t len)
{
    if (focus_mode && len)
        write(STDOUT_FILENO, data, len);
}

static void get_winsize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *rows = ws.ws_row > 0 ? ws.ws_row : 24;
        *cols = ws.ws_col > 0 ? ws.ws_col : 80;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static int visible_count(struct fleet *f)
{
    int n = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++)
        if (f->agents[i].used && f->agents[i].authenticated)
            n++;
    return n;
}

void tui_draw(struct fleet *f)
{
    if (focus_mode)
        return;

    int rows, cols;
    get_winsize(&rows, &cols);
    (void)cols;

    write(STDOUT_FILENO, "\033[H\033[2J", 7);

    char line[512];
    int     n = snprintf(line, sizeof line,
                     "\033[1mnsimaj\033[0m control  v%s  |  online %d  |  "
                     "port %d  |  daemon\r\n",
                     NSIMAJ_VERSION, fleet_online_count(f), f->control_port);
    write(STDOUT_FILENO, line, (size_t)n);

    if (f->update_banner[0]) {
        n = snprintf(line, sizeof line, "\033[33m! %s\033[0m\r\n",
                     f->update_banner);
        write(STDOUT_FILENO, line, (size_t)n);
    }

    n = snprintf(line, sizeof line,
                 "keys: j/k move  space select  a all  u update  e exec  "
                 "c cancel  f focus  S self-upd  q quit  : cmd\r\n");
    write(STDOUT_FILENO, line, (size_t)n);

    n = snprintf(line, sizeof line,
                 "%-3s %-2s %-18s %-14s %-10s %4s %-10s %-18s\r\n",
                 "#", "S", "HOST", "IP", "DISTRO", "UPD", "STAGE", "ERR");
    write(STDOUT_FILENO, line, (size_t)n);
    write(STDOUT_FILENO, "----------------------------------------------------------------"
                         "--------------------\r\n",
          86);

    int vc = visible_count(f);
    if (f->cursor >= vc)
        f->cursor = vc > 0 ? vc - 1 : 0;
    if (f->cursor < 0)
        f->cursor = 0;

    int row = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        struct agent *a = &f->agents[i];
        if (!a->used || !a->authenticated)
            continue;
        int selected = f->selected[i];
        int is_cur = (row == f->cursor);
        int failed = a->job_stage[0] && strcmp(a->job_stage, STAGE_FAILED) == 0;
        const char *on = a->online ? "on" : "off";
        char stage[NSIMAJ_STAGE_LEN];
        snprintf(stage, sizeof stage, "%s", a->job_stage[0] ? a->job_stage : "-");
        const char *err = a->last_error[0] ? a->last_error : "-";
        const char *hi = is_cur ? "\033[7m" : (failed ? "\033[31m" : "");

        n = snprintf(line, sizeof line,
                     "%s%-3d %c%-2s %-18.18s %-14.14s %-10.10s %4d %-10.10s %-18.18s\033[0m\r\n",
                     hi, row, selected ? '*' : ' ', on, a->hostname, a->ip,
                     a->distro, a->pending_updates, stage, err);
        write(STDOUT_FILENO, line, (size_t)n);
        row++;
        if (row >= rows - 6)
            break;
    }

    if (row == 0) {
        const char *ip = f->advertise_ip[0] ? f->advertise_ip : "SERVER_IP";
        n = snprintf(line, sizeof line,
                     "\r\n  No agents yet. On each PC run:\r\n"
                     "  curl -fsSL http://%s:%d/install.sh | sudo bash\r\n"
                     "  (script will ask for the join secret and save it)\r\n",
                     ip, f->http_port);
        write(STDOUT_FILENO, line, (size_t)n);
    }

    n = snprintf(line, sizeof line, "\r\n\033[1m%s\033[0m\r\n", f->status_line);
    write(STDOUT_FILENO, line, (size_t)n);

    if (cmd_mode) {
        n = snprintf(line, sizeof line, ":%s\033[?25h", cmd_buf);
        write(STDOUT_FILENO, line, (size_t)n);
    } else if (f->confirm_pending == 2) {
        write(STDOUT_FILENO,
              "Confirm SERVER self-update (restart)? [y/N] ", 45);
    } else if (f->confirm_pending == 1) {
        write(STDOUT_FILENO,
              "Confirm package update on selected/all hosts? [y/N] ", 52);
    } else {
        write(STDOUT_FILENO, "\033[?25l", 6);
    }
}

static void request_self_update(struct fleet *f)
{
    f->confirm_pending = 2;
    snprintf(f->status_line, sizeof f->status_line, "Confirm SERVER self-update?");
    f->dirty = 1;
}

static int run_cmdline_client(struct fleet *f, int ctl_fd)
{
    if (strcmp(cmd_buf, "self-update") == 0 ||
        strcmp(cmd_buf, "selfupdate") == 0) {
        request_self_update(f);
    } else if (strncmp(cmd_buf, "update", 6) == 0) {
        f->confirm_pending = 1;
        snprintf(f->status_line, sizeof f->status_line, "Confirm package update?");
        f->dirty = 1;
    } else if (strncmp(cmd_buf, "exec ", 5) == 0) {
        client_send_start_job(ctl_fd, JOB_KIND_EXEC, cmd_buf + 5);
    } else if (strcmp(cmd_buf, "cancel") == 0) {
        client_send_cmd(ctl_fd, "cancel");
    } else if (strncmp(cmd_buf, "focus ", 6) == 0) {
        int n = atoi(cmd_buf + 6);
        client_send_cmd_int(ctl_fd, "set_cursor", "n", n);
        if (client_send_cmd_int(ctl_fd, "focus", "n", n) == 0)
            tui_enter_focus_client(f, ctl_fd);
    } else if (strcmp(cmd_buf, "quit") == 0 || strcmp(cmd_buf, "q") == 0) {
        f->running = 0;
    } else {
        snprintf(f->status_line, sizeof f->status_line, "Unknown command: %s",
                 cmd_buf);
        f->dirty = 1;
    }
    cmd_mode = 0;
    cmd_len = 0;
    cmd_buf[0] = '\0';
    return 0;
}

int tui_handle_input_client(struct fleet *f, unsigned char ch, int ctl_fd)
{
    if (focus_mode) {
        if (ch == 0x0e) {
            client_send_cmd(ctl_fd, "focus_detach");
            tui_leave_focus(f);
            return 0;
        }
        client_send_focus_stdin(ctl_fd, &ch, 1);
        return 0;
    }

    if (f->confirm_pending) {
        if (ch == 'y' || ch == 'Y') {
            int kind = f->confirm_pending;
            f->confirm_pending = 0;
            if (kind == 2) {
                tui_restore();
                client_send_cmd(ctl_fd, "self_update");
                f->running = 0;
            } else {
                client_send_start_job(ctl_fd, JOB_KIND_UPDATE, NULL);
            }
        } else if (ch == 'n' || ch == 'N' || ch == 0x1b || ch == 0x03) {
            f->confirm_pending = 0;
            snprintf(f->status_line, sizeof f->status_line, "Update cancelled.");
            f->dirty = 1;
        }
        return 0;
    }

    if (cmd_mode) {
        if (ch == 0x1b || ch == 0x03) {
            cmd_mode = 0;
            cmd_len = 0;
            cmd_buf[0] = '\0';
        } else if (ch == '\r' || ch == '\n') {
            run_cmdline_client(f, ctl_fd);
        } else if (ch == 0x7f || ch == 0x08) {
            if (cmd_len)
                cmd_buf[--cmd_len] = '\0';
        } else if (ch >= 32 && ch < 127 && cmd_len + 1 < sizeof cmd_buf) {
            cmd_buf[cmd_len++] = (char)ch;
            cmd_buf[cmd_len] = '\0';
        }
        return 0;
    }

    int vc = visible_count(f);
    switch (ch) {
    case 'q':
    case 'Q':
    case 0x03:
        f->running = 0;
        break;
    case 'j':
    case 0x0e:
        if (f->cursor + 1 < vc) {
            f->cursor++;
            client_send_cmd_int(ctl_fd, "set_cursor", "n", f->cursor);
        }
        break;
    case 'k':
    case 0x10:
        if (f->cursor > 0) {
            f->cursor--;
            client_send_cmd_int(ctl_fd, "set_cursor", "n", f->cursor);
        }
        break;
    case ' ':
        client_send_cmd(ctl_fd, "toggle_select");
        break;
    case 'a':
    case 'A':
        client_send_cmd(ctl_fd, "select_all");
        break;
    case 'u':
    case 'U':
        f->confirm_pending = 1;
        snprintf(f->status_line, sizeof f->status_line,
                 "Confirm package update on selected (or all if none)?");
        f->dirty = 1;
        break;
    case 'e':
    case 'E':
        cmd_mode = 1;
        cmd_len = 0;
        snprintf(cmd_buf, sizeof cmd_buf, "exec ");
        cmd_len = 5;
        break;
    case 'c':
    case 'C':
        client_send_cmd(ctl_fd, "cancel");
        break;
    case 'f':
    case 'F':
        client_send_cmd_int(ctl_fd, "set_cursor", "n", f->cursor);
        if (client_send_cmd_int(ctl_fd, "focus", "n", f->cursor) == 0)
            tui_enter_focus_client(f, ctl_fd);
        else {
            snprintf(f->status_line, sizeof f->status_line,
                     "Cannot focus that agent");
            f->dirty = 1;
        }
        break;
    case 's':
    case 'S':
        request_self_update(f);
        break;
    case ':':
        cmd_mode = 1;
        cmd_len = 0;
        cmd_buf[0] = '\0';
        break;
    case 0x1b: {
        unsigned char seq[2];
        if (read(STDIN_FILENO, seq, 1) == 1 && seq[0] == '[') {
            if (read(STDIN_FILENO, seq + 1, 1) == 1) {
                if (seq[1] == 'A' && f->cursor > 0) {
                    f->cursor--;
                    client_send_cmd_int(ctl_fd, "set_cursor", "n", f->cursor);
                }
                if (seq[1] == 'B' && f->cursor + 1 < vc) {
                    f->cursor++;
                    client_send_cmd_int(ctl_fd, "set_cursor", "n", f->cursor);
                }
            }
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

/* Kept for API compatibility; daemon no longer uses in-process TUI. */
int tui_handle_input(struct fleet *f, unsigned char ch)
{
    return tui_handle_input_client(f, ch, -1);
}

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
#include <time.h>
#include <unistd.h>

static struct termios orig_term;
static int raw_mode;
static int focus_mode;
static int attach_mode;
static char attach_host[NSIMAJ_HOST_LEN];
static long attach_started_ms;
static int attach_spin;
static int cmd_mode;
static char cmd_buf[NSIMAJ_CMD_LEN];
static size_t cmd_len;

static void get_winsize(int *rows, int *cols);

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void term_mouse_off(void)
{
    /* Defensive: focus/tmux may have enabled mouse reporting. */
    write(STDOUT_FILENO,
          "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l", 40);
}

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
    term_mouse_off();
    write(STDOUT_FILENO, "\033[?1049h\033[2J\033[H\033[?25l", 19);
}

void tui_restore(void)
{
    term_mouse_off();
    if (raw_mode)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    write(STDOUT_FILENO, "\033[?1049l\033[?25h\033[0m", 18);
    raw_mode = 0;
    focus_mode = 0;
    attach_mode = 0;
}

int tui_in_focus(void)
{
    return focus_mode;
}

int tui_in_attach(void)
{
    return attach_mode;
}

void tui_begin_attach(struct fleet *f, const char *hostname)
{
    (void)f;
    attach_mode = 1;
    focus_mode = 0;
    attach_spin = 0;
    attach_started_ms = now_ms();
    snprintf(attach_host, sizeof attach_host, "%s",
             hostname && hostname[0] ? hostname : "host");
}

void tui_cancel_attach(struct fleet *f, const char *reason)
{
    attach_mode = 0;
    if (f && reason && reason[0]) {
        snprintf(f->status_line, sizeof f->status_line, "%s", reason);
        f->dirty = 1;
    }
}

void tui_enter_focus_client(struct fleet *f, int ctl_fd)
{
    (void)f;
    attach_mode = 0;
    focus_mode = 1;
    write(STDOUT_FILENO, "\033[2J\033[H\033[?25h", 12);
    const char *hint =
        "\r\n--- nsimajs focus (Ctrl-N to detach) ---\r\n"
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
    attach_mode = 0;
    term_mouse_off();
    write(STDOUT_FILENO, "\033[2J\033[H\033[?25l", 12);
}

void tui_write_focus(const unsigned char *data, size_t len)
{
    if (focus_mode && len)
        write(STDOUT_FILENO, data, len);
}

void tui_on_focus_data(struct fleet *f, int ctl_fd, const unsigned char *data,
                       size_t len)
{
    if (attach_mode)
        tui_enter_focus_client(f, ctl_fd);
    tui_write_focus(data, len);
}

int tui_attach_tick(struct fleet *f, int ctl_fd)
{
    (void)ctl_fd;
    if (!attach_mode)
        return 0;
    long elapsed = now_ms() - attach_started_ms;
    if (elapsed > 15000) {
        tui_cancel_attach(f, "Focus timed out — agent did not attach");
        client_send_cmd(ctl_fd, "focus_detach");
        return 1;
    }
    attach_spin = (int)((elapsed / 200) % 4);
    f->dirty = 1;
    return 0;
}

static void format_last_update(long epoch, char *out, size_t sz)
{
    if (epoch <= 0) {
        snprintf(out, sz, "-");
        return;
    }
    time_t t = (time_t)epoch;
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        snprintf(out, sz, "-");
        return;
    }
    strftime(out, sz, "%m-%d %H:%M", &tm);
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
    int n;

    if (attach_mode) {
        static const char spinch[] = "|/-\\";
        n = snprintf(line, sizeof line,
                     "\r\n\r\n  Attaching to \033[1m%s\033[0m %c\r\n"
                     "  Waiting for tmux session…\r\n"
                     "  (Esc/q to cancel)\r\n",
                     attach_host, spinch[attach_spin & 3]);
        write(STDOUT_FILENO, line, (size_t)n);
        return;
    }

    n = snprintf(line, sizeof line,
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
                 "keys: arrows move  space select  a all  u update  e exec  "
                 "c cancel  f focus  S self-upd  q quit  : cmd\r\n");
    write(STDOUT_FILENO, line, (size_t)n);

    n = snprintf(line, sizeof line,
                 "%-3s %-2s %-16s %-13s %-8s %3s %-9s %-11s %-12s\r\n",
                 "#", "S", "HOST", "IP", "DISTRO", "UPD", "STAGE", "LAST", "ERR");
    write(STDOUT_FILENO, line, (size_t)n);
    write(STDOUT_FILENO, "----------------------------------------------------------------"
                         "--------------------------\r\n",
          92);

    int idxs[NSIMAJ_MAX_AGENTS];
    int vc = fleet_visible_sorted(f, idxs, NSIMAJ_MAX_AGENTS);
    if (f->cursor >= vc)
        f->cursor = vc > 0 ? vc - 1 : 0;
    if (f->cursor < 0)
        f->cursor = 0;

    int row = 0;
    for (int vi = 0; vi < vc; vi++) {
        int i = idxs[vi];
        struct agent *a = &f->agents[i];
        int selected = f->selected[i];
        int is_cur = (row == f->cursor);
        int failed = a->job_stage[0] && strcmp(a->job_stage, STAGE_FAILED) == 0;
        const char *on = a->online ? "on" : "off";
        char stage[NSIMAJ_STAGE_LEN];
        snprintf(stage, sizeof stage, "%s", a->job_stage[0] ? a->job_stage : "-");
        if (a->queue_len > 0 && strcmp(stage, STAGE_QUEUED) == 0)
            snprintf(stage, sizeof stage, "q%d", a->queue_len);
        char last[16];
        format_last_update(a->last_update_at, last, sizeof last);
        const char *err = a->last_error[0] ? a->last_error : "-";
        const char *hi = is_cur ? "\033[7m" : (failed ? "\033[31m" : "");

        n = snprintf(line, sizeof line,
                     "%s%-3d %c%-2s %-16.16s %-13.13s %-8.8s %3d %-9.9s %-11.11s "
                     "%-12.12s\033[0m\r\n",
                     hi, row, selected ? '*' : ' ', on, a->hostname, a->ip,
                     a->distro, a->pending_updates, stage, last, err);
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

static const char *hostname_at_cursor(struct fleet *f, int nth)
{
    int idxs[NSIMAJ_MAX_AGENTS];
    int n = fleet_visible_sorted(f, idxs, NSIMAJ_MAX_AGENTS);
    if (nth < 0 || nth >= n)
        return "";
    return f->agents[idxs[nth]].hostname;
}

static void request_focus(struct fleet *f, int ctl_fd, int nth)
{
    const char *host = hostname_at_cursor(f, nth);
    client_send_cmd_int(ctl_fd, "set_cursor", "n", nth);
    tui_begin_attach(f, host);
    if (client_send_cmd_int(ctl_fd, "focus", "n", nth) != 0) {
        tui_cancel_attach(f, "Cannot focus that agent");
        return;
    }
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
        request_focus(f, ctl_fd, atoi(cmd_buf + 6));
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

/* Drain mouse CSI (SGR / legacy). Returns 1 if ch was ESC and handled. */
static int handle_esc_client(struct fleet *f, int ctl_fd)
{
    unsigned char b;
    ssize_t n = read(STDIN_FILENO, &b, 1);
    if (n != 1) {
        if (attach_mode) {
            tui_cancel_attach(f, "Focus cancelled");
            client_send_cmd(ctl_fd, "focus_detach");
        } else if (focus_mode) {
            unsigned char esc = 0x1b;
            client_send_focus_stdin(ctl_fd, &esc, 1);
        } else if (cmd_mode) {
            cmd_mode = 0;
            cmd_len = 0;
            cmd_buf[0] = '\0';
        } else if (f->confirm_pending) {
            f->confirm_pending = 0;
            snprintf(f->status_line, sizeof f->status_line, "Update cancelled.");
            f->dirty = 1;
        }
        return 1;
    }
    if (b != '[') {
        if (focus_mode) {
            unsigned char esc = 0x1b;
            client_send_focus_stdin(ctl_fd, &esc, 1);
            client_send_focus_stdin(ctl_fd, &b, 1);
        }
        return 1;
    }
    n = read(STDIN_FILENO, &b, 1);
    if (n != 1)
        return 1;
    /* Legacy mouse: ESC [ M Cb Cx Cy */
    if (b == 'M') {
        unsigned char junk[3];
        (void)read(STDIN_FILENO, junk, 3);
        return 1;
    }
    /* SGR mouse: ESC [ < ... M/m */
    if (b == '<') {
        while (read(STDIN_FILENO, &b, 1) == 1)
            if (b == 'M' || b == 'm')
                break;
        return 1;
    }
    if (focus_mode) {
        unsigned char seq[3] = {0x1b, '[', b};
        client_send_focus_stdin(ctl_fd, seq, 3);
        return 1;
    }
    if (cmd_mode || f->confirm_pending)
        return 1;
    int vc = visible_count(f);
    if (b == 'A' && f->cursor > 0) {
        f->cursor--;
        client_send_cmd_int(ctl_fd, "set_cursor", "n", f->cursor);
    } else if (b == 'B' && f->cursor + 1 < vc) {
        f->cursor++;
        client_send_cmd_int(ctl_fd, "set_cursor", "n", f->cursor);
    }
    return 1;
}

int tui_handle_input_client(struct fleet *f, unsigned char ch, int ctl_fd)
{
    if (ch == 0x1b)
        return handle_esc_client(f, ctl_fd);

    if (attach_mode) {
        if (ch == 'q' || ch == 'Q' || ch == 0x03) {
            tui_cancel_attach(f, "Focus cancelled");
            client_send_cmd(ctl_fd, "focus_detach");
        }
        return 0;
    }

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
        } else if (ch == 'n' || ch == 'N' || ch == 0x03) {
            f->confirm_pending = 0;
            snprintf(f->status_line, sizeof f->status_line, "Update cancelled.");
            f->dirty = 1;
        }
        return 0;
    }

    if (cmd_mode) {
        if (ch == 0x03) {
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

    switch (ch) {
    case 'q':
    case 'Q':
    case 0x03:
        f->running = 0;
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
        request_focus(f, ctl_fd, f->cursor);
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

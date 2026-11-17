#define _DEFAULT_SOURCE
#include "tui.h"

#include "../common/json.h"
#include "../common/util.h"

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

void tui_enter_focus(struct fleet *f)
{
    focus_mode = 1;
    write(STDOUT_FILENO, "\033[2J\033[H\033[?25h", 12);
    int rows = 24, cols = 80;
    get_winsize(&rows, &cols);
    fleet_send_focus_resize(f, rows, cols);
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

static int visible_index(struct fleet *f, int nth)
{
    int seen = 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        if (!f->agents[i].used || !f->agents[i].authenticated)
            continue;
        if (seen == nth)
            return i;
        seen++;
    }
    return -1;
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
    int n = snprintf(line, sizeof line,
                     "\033[1mnsimaj\033[0m control  v%s  |  online %d  |  "
                     "port %d  |  secret set\r\n",
                     NSIMAJ_VERSION, fleet_online_count(f), f->control_port);
    write(STDOUT_FILENO, line, (size_t)n);

    n = snprintf(line, sizeof line,
                 "keys: j/k move  space select  a all  u update  e exec  "
                 "c cancel  f focus  q quit  : cmd\r\n");
    write(STDOUT_FILENO, line, (size_t)n);

    n = snprintf(line, sizeof line,
                 "%-3s %-2s %-20s %-15s %-14s %5s %-12s %-10s\r\n",
                 "#", "S", "HOST", "IP", "DISTRO", "UPD", "STAGE", "JOB");
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
        const char *on = a->online ? "on" : "off";
        char stage[NSIMAJ_STAGE_LEN];
        snprintf(stage, sizeof stage, "%s", a->job_stage[0] ? a->job_stage : "-");
        char jobshort[16];
        if (a->job_id[0])
            snprintf(jobshort, sizeof jobshort, "%.10s", a->job_id);
        else
            snprintf(jobshort, sizeof jobshort, "-");

        n = snprintf(line, sizeof line,
                     "%s%-3d %c%-2s %-20.20s %-15.15s %-14.14s %5d %-12.12s %-10.10s\033[0m\r\n",
                     is_cur ? "\033[7m" : "", row, selected ? '*' : ' ', on,
                     a->hostname, a->ip, a->distro, a->pending_updates, stage,
                     jobshort);
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

    /* Status / command line */
    n = snprintf(line, sizeof line, "\r\n\033[1m%s\033[0m\r\n", f->status_line);
    write(STDOUT_FILENO, line, (size_t)n);

    if (cmd_mode) {
        n = snprintf(line, sizeof line, ":%s\033[?25h", cmd_buf);
        write(STDOUT_FILENO, line, (size_t)n);
    } else if (f->confirm_pending) {
        write(STDOUT_FILENO,
              "Confirm update on selected/all hosts? [y/N] ", 44);
    } else {
        write(STDOUT_FILENO, "\033[?25l", 6);
    }
}

static void toggle_select_cursor(struct fleet *f)
{
    int idx = visible_index(f, f->cursor);
    if (idx >= 0)
        f->selected[idx] = !f->selected[idx];
}

static void select_all(struct fleet *f)
{
    int any = fleet_selected_count(f) > 0;
    for (int i = 0; i < NSIMAJ_MAX_AGENTS; i++) {
        if (f->agents[i].used && f->agents[i].authenticated)
            f->selected[i] = any ? 0 : 1;
    }
}

static int run_cmdline(struct fleet *f)
{
    if (strncmp(cmd_buf, "update", 6) == 0) {
        f->confirm_pending = 1;
        fleet_set_status(f, "Confirm update?");
    } else if (strncmp(cmd_buf, "exec ", 5) == 0) {
        fleet_start_job(f, JOB_KIND_EXEC, cmd_buf + 5);
    } else if (strcmp(cmd_buf, "cancel") == 0) {
        fleet_cancel_jobs(f);
    } else if (strncmp(cmd_buf, "focus ", 6) == 0) {
        int n = atoi(cmd_buf + 6);
        int idx = visible_index(f, n);
        if (idx >= 0 && fleet_focus(f, idx) == 0)
            tui_enter_focus(f);
    } else if (strcmp(cmd_buf, "quit") == 0 || strcmp(cmd_buf, "q") == 0) {
        f->running = 0;
    } else {
        fleet_set_status(f, "Unknown command: %s", cmd_buf);
    }
    cmd_mode = 0;
    cmd_len = 0;
    cmd_buf[0] = '\0';
    return 0;
}

int tui_handle_input(struct fleet *f, unsigned char ch)
{
    if (focus_mode) {
        /* Detach: F2 or detect Ctrl-B D sequence simply via F2 */
        if (ch == 0x02) { /* Ctrl-B — wait next; simplified: Ctrl-B then 'd' */
            static int saw_b;
            saw_b = 1;
            (void)saw_b;
            /* Pass through to remote as well for real tmux; also allow F2 */
            fleet_send_focus_stdin(f, &ch, 1);
            return 0;
        }
        if (ch == 0x7f || ch == 0x08) {
            fleet_send_focus_stdin(f, &ch, 1);
            return 0;
        }
        /* F2 = detach (ESC O Q or ESC [ 1 2 ~) — handle bare 0x0e as alt */
        if (ch == 0x0e) { /* Ctrl-N also detach */
            fleet_focus_detach(f);
            tui_leave_focus(f);
            return 0;
        }
        fleet_send_focus_stdin(f, &ch, 1);
        return 0;
    }

    if (f->confirm_pending) {
        if (ch == 'y' || ch == 'Y') {
            fleet_start_job(f, JOB_KIND_UPDATE, NULL);
            f->confirm_pending = 0;
        } else if (ch == 'n' || ch == 'N' || ch == 0x1b || ch == 0x03) {
            f->confirm_pending = 0;
            fleet_set_status(f, "Update cancelled.");
        }
        return 0;
    }

    if (cmd_mode) {
        if (ch == 0x1b || ch == 0x03) {
            cmd_mode = 0;
            cmd_len = 0;
            cmd_buf[0] = '\0';
        } else if (ch == '\r' || ch == '\n') {
            run_cmdline(f);
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
        if (f->cursor + 1 < vc)
            f->cursor++;
        break;
    case 'k':
    case 0x10:
        if (f->cursor > 0)
            f->cursor--;
        break;
    case ' ':
        toggle_select_cursor(f);
        break;
    case 'a':
    case 'A':
        select_all(f);
        break;
    case 'u':
    case 'U':
        f->confirm_pending = 1;
        fleet_set_status(f, "Confirm package update on selected (or all if none)?");
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
        fleet_cancel_jobs(f);
        break;
    case 'f':
    case 'F': {
        int idx = visible_index(f, f->cursor);
        if (idx >= 0 && fleet_focus(f, idx) == 0)
            tui_enter_focus(f);
        else
            fleet_set_status(f, "Cannot focus that agent");
        break;
    }
    case ':':
        cmd_mode = 1;
        cmd_len = 0;
        cmd_buf[0] = '\0';
        break;
    case 0x1b: {
        /* Arrow keys */
        unsigned char seq[2];
        if (read(STDIN_FILENO, seq, 1) == 1 && seq[0] == '[') {
            if (read(STDIN_FILENO, seq + 1, 1) == 1) {
                if (seq[1] == 'A' && f->cursor > 0)
                    f->cursor--;
                if (seq[1] == 'B' && f->cursor + 1 < vc)
                    f->cursor++;
                /* F2 often ESC [ 1 2 ~ — not fully parsed here */
            }
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

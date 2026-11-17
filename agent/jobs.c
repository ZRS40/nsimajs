#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "jobs.h"
#include "state.h"

#include "../common/json.h"
#include "../common/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

const char *agent_tmux_name(const char *job_id, char *buf, size_t sz)
{
    snprintf(buf, sz, "nsimaj-%s", job_id);
    /* tmux session names: sanitize */
    for (char *p = buf; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            *p = '_';
    }
    return buf;
}

static int run_cmd(const char *cmd)
{
    int st = system(cmd);
    if (st < 0)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -1;
}

static int ensure_tmux(const char *family, char *err, size_t err_sz)
{
    if (err && err_sz)
        err[0] = '\0';
    if (run_cmd("command -v tmux >/dev/null 2>&1") == 0)
        return 0;
    int rc = -1;
    if (strcmp(family, "arch") == 0)
        rc = run_cmd("pacman -Sy --noconfirm tmux >/tmp/nsimaj-tmux-install.log 2>&1");
    else if (strcmp(family, "debian") == 0)
        rc = run_cmd("DEBIAN_FRONTEND=noninteractive apt-get install -y tmux "
                     ">/tmp/nsimaj-tmux-install.log 2>&1");
    else {
        if (err && err_sz)
            snprintf(err, err_sz, "unsupported family for tmux install");
        return -1;
    }
    if (rc != 0) {
        if (err && err_sz)
            snprintf(err, err_sz, "tmux install failed (see /tmp/nsimaj-tmux-install.log)");
        return -1;
    }
    if (run_cmd("command -v tmux >/dev/null 2>&1") != 0) {
        if (err && err_sz)
            snprintf(err, err_sz, "tmux still missing after install");
        return -1;
    }
    return 0;
}

static int send_stage(struct agent_state *st, const char *stage, long code,
                      const char *err)
{
    snprintf(st->job_stage, sizeof st->job_stage, "%s", stage);
    return agent_send_event(st, stage, code, err);
}

int agent_job_start(struct agent_state *st, const char *job_id, const char *kind,
                    const char *payload)
{
    char session[128];
    agent_tmux_name(job_id, session, sizeof session);

    snprintf(st->job_id, sizeof st->job_id, "%s", job_id);
    st->job_active = 1;
    send_stage(st, STAGE_QUEUED, -1, NULL);

    /* Kill previous session with same name if any */
    char killcmd[256];
    snprintf(killcmd, sizeof killcmd, "tmux kill-session -t %s 2>/dev/null",
             session);
    run_cmd(killcmd);

    send_stage(st, STAGE_PREPARING, -1, NULL);
    char tmux_err[128];
    int have_tmux = (ensure_tmux(st->family, tmux_err, sizeof tmux_err) == 0);
    if (!have_tmux && tmux_err[0])
        send_stage(st, STAGE_PREPARING, -1, tmux_err);

    char script_path[256];
    char log_path[256];
    char pid_path[256];
    snprintf(script_path, sizeof script_path, "/tmp/nsimaj-job-%s.sh", session);
    snprintf(log_path, sizeof log_path, "/tmp/nsimaj-log-%s", session);
    snprintf(pid_path, sizeof pid_path, "/tmp/nsimaj-pid-%s", session);

    /* Truncate log for this job */
    {
        FILE *lf = fopen(log_path, "w");
        if (lf)
            fclose(lf);
    }

    int interactive_shell =
        (strcmp(kind, JOB_KIND_EXEC) == 0 && payload &&
         (strcmp(payload, "bash") == 0 || strcmp(payload, "sh") == 0));

    FILE *sf = fopen(script_path, "w");
    if (!sf) {
        send_stage(st, STAGE_FAILED, 1, "cannot write job script");
        st->job_active = 0;
        return -1;
    }

    fprintf(sf, "#!/bin/sh\n");
    fprintf(sf, "STAGE_FILE=/tmp/nsimaj-stage-%s\n", session);
    fprintf(sf, "echo preparing > \"$STAGE_FILE\"\n");
    /* Do NOT exec-redirect to a log file — that makes the tmux pane black.
     * Logging is done via tmux pipe-pane after the session starts. */

    if (interactive_shell) {
        /* Real interactive shell so focus shows user@host */
        fprintf(sf, "echo running > \"$STAGE_FILE\"\n");
        fprintf(sf, "export TERM=\"${TERM:-xterm-256color}\"\n");
        fprintf(sf, "exec /bin/bash -l\n");
    } else if (strcmp(kind, JOB_KIND_UPDATE) == 0) {
        fprintf(sf, "echo\n");
        fprintf(sf, "echo '======== nsimaj PACKAGE UPDATE ========'\n");
        fprintf(sf, "echo \"host=$(hostname 2>/dev/null || cat /etc/hostname) "
                    "family=%s\"\n",
                st->family);
        fprintf(sf, "echo '======================================='\n");
        fprintf(sf, "echo\n");
        if (strcmp(st->family, "arch") == 0) {
            fprintf(sf, "echo '>>> [1/3] Refreshing package indexes (pacman -Sy)...'\n");
            fprintf(sf, "echo updating_indexes > \"$STAGE_FILE\"\n");
            fprintf(sf, "pacman -Sy --noconfirm\n");
            fprintf(sf, "echo '>>> [2/3] Upgrading packages (pacman -Su)...'\n");
            fprintf(sf, "echo upgrading > \"$STAGE_FILE\"\n");
            fprintf(sf, "pacman -Su --noconfirm\n");
        } else if (strcmp(st->family, "debian") == 0) {
            fprintf(sf, "echo '>>> [1/3] Refreshing package indexes (apt-get update)...'\n");
            fprintf(sf, "echo updating_indexes > \"$STAGE_FILE\"\n");
            fprintf(sf, "export DEBIAN_FRONTEND=noninteractive\n");
            fprintf(sf, "apt-get update\n");
            fprintf(sf, "echo '>>> [2/3] Upgrading packages (apt-get upgrade)...'\n");
            fprintf(sf, "echo upgrading > \"$STAGE_FILE\"\n");
            fprintf(sf, "apt-get -y upgrade\n");
        } else {
            fprintf(sf, "echo 'unsupported distro family' >&2\nexit 2\n");
        }
        fprintf(sf, "ec_pkg=$?\n");
        fprintf(sf, "echo\n");
        fprintf(sf, "echo \">>> package step finished (exit $ec_pkg)\"\n");
        fprintf(sf, "echo installing_agent > \"$STAGE_FILE\"\n");
        fprintf(sf, "sync\n");
        fprintf(sf, "echo '>>> [3/3] Refreshing nsimaj-agent binary from server...'\n");
        fprintf(sf, "NSIMAJ_SRV='%s'\n", st->server_host);
        fprintf(sf, "NSIMAJ_HTTP='%d'\n",
                st->http_port > 0 ? st->http_port : NSIMAJ_HTTP_PORT);
        fprintf(sf,
                "AGTMP=$(mktemp) && "
                "if curl -fsSL \"http://${NSIMAJ_SRV}:${NSIMAJ_HTTP}/nsimaj-agent\" "
                "-o \"$AGTMP\"; then "
                "install -m 0755 \"$AGTMP\" /usr/local/bin/nsimaj-agent && "
                "curl -fsSL \"http://${NSIMAJ_SRV}:${NSIMAJ_HTTP}/nsimaj-agent.service\" "
                "-o /etc/systemd/system/nsimaj-agent.service 2>/dev/null || true; "
                "systemctl daemon-reload 2>/dev/null || true; "
                "echo '>>> nsimaj-agent binary installed OK'; "
                "else echo '>>> WARNING: could not download new nsimaj-agent' >&2; fi; "
                "rm -f \"$AGTMP\"\n");
        fprintf(sf, "ec=$ec_pkg\n");
        fprintf(sf, "if [ \"$ec\" -eq 0 ]; then echo done > \"$STAGE_FILE\"; "
                    "else echo failed > \"$STAGE_FILE\"; fi\n");
        fprintf(sf, "sync\n");
        fprintf(sf, "echo\n");
        fprintf(sf,
                "if [ \"$ec\" -eq 0 ]; then "
                "echo '======== UPDATE COMPLETE (success) ========'; "
                "else "
                "echo '======== UPDATE FINISHED WITH ERRORS (exit '"
                "\"$ec\") ========'; fi\n");
        fprintf(sf, "echo 'Scheduling agent restart in 3s "
                    "(so nsimajd can receive done first).'\n");
        fprintf(sf, "echo 'Press Ctrl-N in nsimajd to detach if needed.'\n");
        /* Background restart so this process can exit and agent_job_poll
         * can report done/failed before the agent binary is killed. */
        fprintf(sf, "( sleep 3; systemctl restart nsimaj-agent ) &\n");
        fprintf(sf, "exit \"$ec\"\n");
    } else if (strcmp(kind, JOB_KIND_EXEC) == 0) {
        fprintf(sf, "echo running > \"$STAGE_FILE\"\n");
        if (payload && payload[0])
            fprintf(sf, "%s\n", payload);
        else
            fprintf(sf, "exec /bin/bash -l\n");
        fprintf(sf, "ec=$?\n");
        fprintf(sf, "if [ \"$ec\" -eq 0 ]; then echo done > \"$STAGE_FILE\"; "
                    "else echo failed > \"$STAGE_FILE\"; fi\n");
        fprintf(sf, "exit \"$ec\"\n");
    } else {
        fprintf(sf, "echo 'unknown job kind' >&2\nexit 2\n");
    }
    fclose(sf);
    chmod(script_path, 0700);

    if (strcmp(kind, JOB_KIND_UPDATE) == 0)
        send_stage(st, STAGE_UPDATING_INDEXES, -1, NULL);
    else
        send_stage(st, STAGE_RUNNING, -1, NULL);

    if (have_tmux) {
        char tmuxcmd[1024];
        snprintf(tmuxcmd, sizeof tmuxcmd,
                 "TERM=xterm-256color tmux new-session -d -s %s -x 120 -y 40 -- "
                 "/bin/sh %s",
                 session, script_path);
        if (run_cmd(tmuxcmd) != 0) {
            have_tmux = 0;
            send_stage(st, STAGE_PREPARING, -1, "tmux new-session failed");
        } else {
            char check[256];
            snprintf(check, sizeof check, "tmux has-session -t %s 2>/dev/null",
                     session);
            if (run_cmd(check) != 0) {
                have_tmux = 0;
                send_stage(st, STAGE_PREPARING, -1, "tmux session missing after create");
            } else {
                /* Mirror pane output into log for focus fallback */
                char pipe_cmd[512];
                snprintf(pipe_cmd, sizeof pipe_cmd,
                         "tmux pipe-pane -t %s -o 'cat >> %s'", session, log_path);
                run_cmd(pipe_cmd);
            }
        }
    }

    if (!have_tmux) {
        char cmd[2048];
        snprintf(cmd, sizeof cmd,
                 "setsid sh -c '%s > %s 2>&1; echo $? > %s.exit' </dev/null & "
                 "echo $! > %s",
                 script_path, log_path, log_path, pid_path);
        if (run_cmd(cmd) != 0) {
            send_stage(st, STAGE_FAILED, 1, "failed to start job process");
            st->job_active = 0;
            return -1;
        }
        send_stage(st, STAGE_PREPARING, -1, "running without tmux (log only)");
    }
    return 0;
}

int agent_job_cancel(struct agent_state *st, const char *job_id)
{
    char session[128];
    const char *id = job_id && job_id[0] ? job_id : st->job_id;
    agent_tmux_name(id, session, sizeof session);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "tmux kill-session -t %s 2>/dev/null", session);
    run_cmd(cmd);
    char pidbuf[64];
    char pid_path[256];
    snprintf(pid_path, sizeof pid_path, "/tmp/nsimaj-pid-%s", session);
    if (util_read_file(pid_path, pidbuf, sizeof pidbuf) >= 0) {
        util_trim(pidbuf);
        long pid = atol(pidbuf);
        if (pid > 0)
            kill((pid_t)pid, SIGTERM);
    }
    send_stage(st, STAGE_CANCELLED, 130, "cancelled");
    st->job_active = 0;
    return 0;
}

void agent_job_poll(struct agent_state *st)
{
    if (!st->job_active || !st->job_id[0])
        return;

    char session[128];
    agent_tmux_name(st->job_id, session, sizeof session);

    char stage_file[256];
    char pid_path[256];
    snprintf(stage_file, sizeof stage_file, "/tmp/nsimaj-stage-%s", session);
    snprintf(pid_path, sizeof pid_path, "/tmp/nsimaj-pid-%s", session);

    char stage[64] = "";
    if (util_read_file(stage_file, stage, sizeof stage) >= 0) {
        util_trim(stage);
        if (stage[0] && strcmp(stage, st->job_stage) != 0) {
            if (strcmp(stage, "done") == 0) {
                send_stage(st, STAGE_DONE, 0, NULL);
                st->job_active = 0;
            } else if (strcmp(stage, "failed") == 0) {
                send_stage(st, STAGE_FAILED, 1, "job failed");
                st->job_active = 0;
            } else if (strcmp(stage, "upgrading") == 0)
                send_stage(st, STAGE_UPGRADING, -1, NULL);
            else if (strcmp(stage, "updating_indexes") == 0)
                send_stage(st, STAGE_UPDATING_INDEXES, -1, NULL);
            else if (strcmp(stage, "installing_agent") == 0)
                send_stage(st, "installing_agent", -1, NULL);
            else if (strcmp(stage, "running") == 0)
                send_stage(st, STAGE_RUNNING, -1, NULL);
            else
                send_stage(st, stage, -1, NULL);
        }
    }

    if (!st->job_active)
        return;

    int alive = 0;
    char check[256];
    snprintf(check, sizeof check, "tmux has-session -t %s 2>/dev/null", session);
    if (run_cmd(check) == 0)
        alive = 1;
    else {
        char pidbuf[64];
        if (util_read_file(pid_path, pidbuf, sizeof pidbuf) >= 0) {
            util_trim(pidbuf);
            long pid = atol(pidbuf);
            if (pid > 0 && kill((pid_t)pid, 0) == 0)
                alive = 1;
        }
    }

    if (!alive) {
        if (strcmp(st->job_stage, STAGE_DONE) != 0 &&
            strcmp(st->job_stage, STAGE_FAILED) != 0 &&
            strcmp(st->job_stage, STAGE_CANCELLED) != 0) {
            if (strcmp(stage, "done") == 0)
                send_stage(st, STAGE_DONE, 0, NULL);
            else if (strcmp(stage, "failed") == 0)
                send_stage(st, STAGE_FAILED, 1, "job failed");
            else if (stage[0] == '\0')
                send_stage(st, STAGE_FAILED, 1, "session ended early");
            else
                send_stage(st, STAGE_DONE, 0, NULL);
        }
        st->job_active = 0;
    }
}

int agent_focus_start(struct agent_state *st, const char *job_id)
{
    if (st->focus_pty >= 0)
        agent_focus_detach(st);

    char session[128];
    agent_tmux_name(job_id && job_id[0] ? job_id : st->job_id, session,
                    sizeof session);
    char log_path[256];
    snprintf(log_path, sizeof log_path, "/tmp/nsimaj-log-%s", session);

    /* Wait briefly for job_start to create the session */
    int use_tmux = 0;
    if (run_cmd("command -v tmux >/dev/null 2>&1") == 0) {
        char check[256];
        snprintf(check, sizeof check, "tmux has-session -t %s 2>/dev/null",
                 session);
        for (int i = 0; i < 20; i++) {
            if (run_cmd(check) == 0) {
                use_tmux = 1;
                break;
            }
            util_sleep_ms(100);
        }
    }

    int mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (mfd < 0)
        return -1;
    if (grantpt(mfd) < 0 || unlockpt(mfd) < 0) {
        close(mfd);
        return -1;
    }
    char *slave = ptsname(mfd);
    if (!slave) {
        close(mfd);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(mfd);
        return -1;
    }
    if (!pid) {
        setsid();
        int sfd = open(slave, O_RDWR);
        if (sfd < 0)
            _exit(127);
        ioctl(sfd, TIOCSCTTY, 0);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 0);
        struct winsize ws;
        memset(&ws, 0, sizeof ws);
        ws.ws_row = 40;
        ws.ws_col = 120;
        ioctl(sfd, TIOCSWINSZ, &ws);
        dup2(sfd, STDIN_FILENO);
        dup2(sfd, STDOUT_FILENO);
        dup2(sfd, STDERR_FILENO);
        if (sfd > 2)
            close(sfd);
        close(mfd);
        if (use_tmux)
            execlp("tmux", "tmux", "-u", "attach-session", "-t", session,
                   (char *)NULL);
        /* No session: follow log with a clear banner (never leave a blank PTY) */
        execlp("sh", "sh", "-c",
               "export TERM=xterm-256color; "
               "printf '\\r\\n[nsimaj] no tmux session — showing job log "
               "(Ctrl-N to detach)\\r\\n\\r\\n'; "
               "touch \"$1\"; "
               "if [ ! -s \"$1\" ]; then "
               "printf '(log empty so far — waiting for output...)\\r\\n'; fi; "
               "tail -n +1 -f \"$1\"",
               "sh", log_path, (char *)NULL);
        _exit(127);
    }

    st->focus_pty = mfd;
    st->focus_pid = pid;
    set_nonblock(mfd);
    return 0;
}

void agent_focus_detach(struct agent_state *st)
{
    int was_focusing = (st->focus_pty >= 0 || st->focus_pid > 0);
    if (st->focus_pid > 0) {
        kill(st->focus_pid, SIGHUP);
        waitpid(st->focus_pid, NULL, 0);
        st->focus_pid = 0;
    }
    if (st->focus_pty >= 0) {
        close(st->focus_pty);
        st->focus_pty = -1;
    }
    if (!was_focusing || st->fd < 0)
        return;
    struct json_buf b;
    json_buf_init(&b, 128);
    json_buf_printf(&b, "{");
    json_buf_add_str(&b, "type", MSG_FOCUS_END);
    json_buf_printf(&b, "}");
    agent_send_json(st, &b);
    json_buf_free(&b);
}

void agent_focus_stdin(struct agent_state *st, const unsigned char *data,
                       size_t len)
{
    if (st->focus_pty < 0 || !data || !len)
        return;
    write_full(st->focus_pty, data, len);
}

void agent_focus_resize(struct agent_state *st, int rows, int cols)
{
    if (st->focus_pty < 0)
        return;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ioctl(st->focus_pty, TIOCSWINSZ, &ws);
}

void agent_focus_poll(struct agent_state *st)
{
    if (st->focus_pty < 0)
        return;

    unsigned char buf[4096];
    for (;;) {
        ssize_t n = read(st->focus_pty, buf, sizeof buf);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            agent_focus_detach(st);
            break;
        }
        if (n == 0) {
            agent_focus_detach(st);
            break;
        }
        char encoded[NSIMAJ_MAX_FRAME];
        if (!b64_encode(buf, (size_t)n, encoded, sizeof encoded))
            continue;
        struct json_buf b;
        json_buf_init(&b, (size_t)n * 2 + 256);
        json_buf_printf(&b, "{");
        json_buf_add_str(&b, "type", MSG_FOCUS_DATA);
        json_buf_add_str(&b, "data", encoded);
        json_buf_printf(&b, "}");
        if (agent_send_json(st, &b) < 0) {
            json_buf_free(&b);
            break;
        }
        json_buf_free(&b);
    }

    if (st->focus_pid > 0) {
        int status;
        pid_t r = waitpid(st->focus_pid, &status, WNOHANG);
        if (r > 0) {
            st->focus_pid = 0;
            if (st->focus_pty >= 0) {
                close(st->focus_pty);
                st->focus_pty = -1;
            }
            struct json_buf b;
            json_buf_init(&b, 128);
            json_buf_printf(&b, "{");
            json_buf_add_str(&b, "type", MSG_FOCUS_END);
            json_buf_printf(&b, "}");
            agent_send_json(st, &b);
            json_buf_free(&b);
        }
    }
}

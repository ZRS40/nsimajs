#define _DEFAULT_SOURCE
#include "sshcon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define COMMAND_SIZE 1024
#define VALUE_SIZE 256

static const char update_command[] =
    ". /etc/os-release 2>/dev/null || { echo 'cannot detect distribution' >&2; exit 64; }; "
    "case \" $ID $ID_LIKE \" in "
    "*' debian '*|*' ubuntu '*|*' linuxmint '*) "
    "command -v tmux >/dev/null || { apt-get update && apt-get install -y tmux; }; "
    "apt-get update && apt-get -y upgrade;; "
    "*' arch '*|*' manjaro '*|*' endeavouros '*) "
    "command -v tmux >/dev/null || pacman -Sy --noconfirm tmux; "
    "pacman -Syu --noconfirm;; "
    "*) echo \"unsupported distribution: ${ID:-unknown}\" >&2; exit 64;; "
    "esac";

struct config {
    char user[VALUE_SIZE], ssh_password[VALUE_SIZE], sudo_password[VALUE_SIZE];
};

static const struct passwd *config_owner(void)
{
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user) {
        const struct passwd *owner = getpwnam(sudo_user);
        if (owner)
            return owner;
    }
    const char *user = getenv("USER");
    return user ? getpwnam(user) : NULL;
}

static int config_path(char *path, size_t size)
{
    const struct passwd *owner = config_owner();
    const char *home = owner ? owner->pw_dir : getenv("HOME");
    return home && snprintf(path, size, "%s/.config/nsimaj/config", home) < (int)size
        ? 0 : -1;
}

static int log_path(char *path, size_t size, const char *ip)
{
    if (config_path(path, size) < 0)
        return -1;
    char *name = strrchr(path, '/');
    if (!name || (size_t)snprintf(name, size - (size_t)(name - path),
                                  "ssh-%s.log", ip) >=
                     size - (size_t)(name - path))
        return -1;
    return 0;
}

static void set_value(struct config *config, char *line)
{
    char *value = strchr(line, '=');
    if (!value)
        return;
    *value++ = '\0';
    if (!strcmp(line, "ssh_user"))
        snprintf(config->user, sizeof config->user, "%s", value);
    else if (!strcmp(line, "ssh_password"))
        snprintf(config->ssh_password, sizeof config->ssh_password, "%s", value);
    else if (!strcmp(line, "sudo_password"))
        snprintf(config->sudo_password, sizeof config->sudo_password, "%s", value);
}

static void load_config(struct config *config)
{
    char path[PATH_MAX], line[COMMAND_SIZE];
    if (config_path(path, sizeof path) < 0)
        return;
    FILE *file = fopen(path, "r");
    if (!file)
        return;
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = '\0';
        set_value(config, line);
    }
    fclose(file);
}

static int save_config(const struct config *config)
{
    char dir[PATH_MAX], path[PATH_MAX];
    const struct passwd *owner = config_owner();
    const char *home = owner ? owner->pw_dir : getenv("HOME");
    if (!home || snprintf(dir, sizeof dir, "%s/.config/nsimaj", home) >= (int)sizeof dir ||
        config_path(path, sizeof path) < 0)
        return -1;
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0700);
        *slash = '/';
    }
    mkdir(dir, 0700);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    fchmod(fd, 0600);
    if (owner && fchown(fd, owner->pw_uid, owner->pw_gid) < 0)
        perror("fchown");
    FILE *file = fdopen(fd, "w");
    if (!file)
        return close(fd), -1;
    fprintf(file, "ssh_user=%s\nssh_password=%s\nsudo_password=%s\n",
            config->user, config->ssh_password, config->sudo_password);
    return fclose(file);
}

static int ask_value(const char *prompt, char *value, size_t size)
{
    char *input = getpass(prompt);
    if (!input || !*input || strlen(input) >= size)
        return -1;
    snprintf(value, size, "%s", input);
    return 0;
}

static void sudo_command(char *out, size_t size, const char *command)
{
    size_t used = (size_t)snprintf(out, size, "sudo -S -p '' sh -c '");
    for (size_t i = 0; command[i] && used + 5 < size; i++) {
        if (command[i] == '\'')
            used += (size_t)snprintf(out + used, size - used, "'\\''");
        else
            out[used++] = command[i];
    }
    if (used < size)
        snprintf(out + used, size - used, "'");
}

static void shell_quote(char *out, size_t size, const char *value)
{
    size_t used = (size_t)snprintf(out, size, "'");
    for (size_t i = 0; value[i] && used + 5 < size; i++) {
        if (value[i] == '\'')
            used += (size_t)snprintf(out + used, size - used, "'\\''");
        else
            out[used++] = value[i];
    }
    if (used < size)
        snprintf(out + used, size - used, "'");
}

struct job {
    pid_t pid;
    int fd, active;
    int status, started;
    char ip[INET_ADDRSTRLEN];
    FILE *log;
};

static volatile sig_atomic_t focused_pid;
static volatile sig_atomic_t interrupted;

static void interrupt_focused(int signal_number)
{
    (void)signal_number;
    if (focused_pid > 0)
        kill(-focused_pid, SIGINT);
    interrupted = 1;
}

static int start_ssh(const struct config *config, const struct host *host,
                     const char *command, int sudo, struct job *job)
{
    char ip[INET_ADDRSTRLEN], target[VALUE_SIZE + INET_ADDRSTRLEN + 2];
    char remote_command[COMMAND_SIZE * 2], tmux_command[COMMAND_SIZE * 4];
    if (!inet_ntop(AF_INET, &host->ip, ip, sizeof ip))
        return -1;
    snprintf(target, sizeof target, "%s@%s", config->user, ip);

    int output[2];
    if (pipe(output) < 0) {
        perror("pipe");
        return -1;
    }
    int input[2] = {-1, -1};
    if (sudo && pipe(input) < 0) {
        perror("pipe");
        close(output[0]);
        close(output[1]);
        return -1;
    }
    char path[PATH_MAX];
    FILE *log = NULL;
    if (log_path(path, sizeof path, ip) == 0)
        log = fopen(path, "a");
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(output[0]);
        close(output[1]);
        if (sudo)
            close(input[0]), close(input[1]);
        if (log)
            fclose(log);
        return -1;
    }
    if (!pid) {
        setpgid(0, 0);
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        close(output[0]);
        close(output[1]);
        if (sudo) {
            dup2(input[0], STDIN_FILENO);
            close(input[0]);
            close(input[1]);
        }
        setpgid(pid, pid);
        setenv("SSHPASS", config->ssh_password, 1);
        const char *remote = command;
        if (sudo) {
            char quoted[COMMAND_SIZE * 2];
            shell_quote(quoted, sizeof quoted, command);
            snprintf(tmux_command, sizeof tmux_command,
                     ". /etc/os-release 2>/dev/null || exit 64; "
                     "command -v tmux >/dev/null || "
                     "case \" $ID $ID_LIKE \" in "
                     "*' debian '*|*' ubuntu '*|*' linuxmint '*) "
                     "apt-get update && apt-get install -y tmux;; "
                     "*' arch '*|*' manjaro '*|*' endeavouros '*) "
                     "pacman -Sy --noconfirm tmux;; "
                     "*) echo \"unsupported distribution: ${ID:-unknown}\" "
                     ">&2; exit 64;; esac; "
                     "tmux kill-session -t nsimaj-update 2>/dev/null || true; "
                     "tmux new-session -s nsimaj-update -- sh -c %s", quoted);
            sudo_command(remote_command, sizeof remote_command, tmux_command);
            remote = remote_command;
        }
        if (sudo)
            execlp("sshpass", "sshpass", "-e", "ssh", "-tt",
                   "-o", "ConnectTimeout=5",
                   "-o", "StrictHostKeyChecking=accept-new",
                   target, remote, (char *)NULL);
        else
            execlp("sshpass", "sshpass", "-e", "ssh", "-o", "ConnectTimeout=5",
               "-o", "StrictHostKeyChecking=accept-new",
               target, remote, (char *)NULL);
        perror("sshpass");
        _exit(127);
    }
    close(output[1]);
    if (fcntl(output[0], F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl");
        close(output[0]);
        return -1;
    }
    if (sudo) {
        close(input[0]);
        dprintf(input[1], "%s\n", config->sudo_password);
        close(input[1]);
    }
    if (log)
    job->pid = pid;
    job->fd = output[0];
    job->active = 1;
    job->started = 1;
    job->log = log;
    snprintf(job->ip, sizeof job->ip, "%s", ip);
    if (log)
        fprintf(log, "\n[%s] command: %s\n", ip, command);
    return 0;
}

static int job_count(struct job *jobs, size_t count)
{
    int active = 0;
    for (size_t i = 0; i < count; i++)
        active += jobs[i].active;
    return active;
}

static int pump_jobs(struct job *jobs, size_t count, const char *watch,
                     int timeout)
{
    struct pollfd *fds = calloc(count, sizeof *fds);
    size_t *indexes = calloc(count, sizeof *indexes);
    if (!fds || !indexes) {
        free(fds);
        free(indexes);
        return 0;
    }
    int completed = 0;
    for (;;) {
        size_t used = 0;
        for (size_t i = 0; i < count; i++)
            if (jobs[i].active) {
                fds[used].fd = jobs[i].fd;
                fds[used++].events = POLLIN;
                indexes[used - 1] = i;
            }
        if (!used || poll(fds, used, timeout) <= 0)
            break;
        for (size_t n = 0; n < used; n++) {
            if (!(fds[n].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            size_t i = indexes[n];
            char buffer[512];
            ssize_t length = read(jobs[i].fd, buffer, sizeof buffer);
            if (length > 0) {
                if (!strcmp(watch, jobs[i].ip))
                    printf("[%s] %.*s", jobs[i].ip, (int)length, buffer);
                if (jobs[i].log)
                    fwrite(buffer, 1, (size_t)length, jobs[i].log);
                if (jobs[i].log)
                    fflush(jobs[i].log);
                fflush(stdout);
            } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            } else if (!length) {
                int status;
                close(jobs[i].fd);
                while (waitpid(jobs[i].pid, &status, 0) < 0 &&
                       errno == EINTR)
                    ;
                jobs[i].active = 0;
                completed++;
                if (jobs[i].log) {
                    int result = WIFEXITED(status) ? WEXITSTATUS(status) :
                                 128 + WTERMSIG(status);
                    jobs[i].status = result;
                    fprintf(jobs[i].log, "[%s] exit status: %d\n",
                            jobs[i].ip, result);
                    fclose(jobs[i].log);
                }
                int result = WIFEXITED(status) ? WEXITSTATUS(status) :
                             128 + WTERMSIG(status);
                if (interrupted && !strcmp(watch, jobs[i].ip)) {
                    printf("\n[%s] stopped by Ctrl+C\n", jobs[i].ip);
                    interrupted = 0;
                } else {
                    printf("\n[%s] %s (status %d)\n", jobs[i].ip,
                           result ? "failed" : "completed", result);
                }
            }
        }
    }
    free(fds);
    free(indexes);
    return completed;
}

static void start_all(const struct config *config, const struct hosts *hosts,
                      const char *command, int sudo, struct job *jobs)
{
    for (size_t i = 0; i < hosts->count; i++)
        if (start_ssh(config, &hosts->items[i], command, sudo, &jobs[i]) < 0)
            fprintf(stderr, "cannot start SSH for host %zu\n", i + 1);
}

static void start_one(const struct config *config, const struct host *host,
                      const char *command, int sudo, struct job *job)
{


    if (start_ssh(config, host, command, sudo, job) < 0)
        fprintf(stderr, "cannot start SSH for host %s\n", job->ip);
}

static void show_log(const char *ip)
{
    char path[PATH_MAX], buffer[512];
    if (log_path(path, sizeof path, ip) < 0)
        return;
    FILE *log = fopen(path, "r");
    if (!log) {
        printf("[%s] no log yet\n", ip);
        return;
    }
    printf("\n--- existing log: %s ---\n", ip);
    while (fgets(buffer, sizeof buffer, log))
        fputs(buffer, stdout);
    fclose(log);
    printf("--- live output: %s ---\n", ip);
}

static int read_command(char *line, size_t size, char *history)
{
    struct termios original, raw;
    if (tcgetattr(STDIN_FILENO, &original) < 0)
        return fgets(line, (int)size, stdin) ? 0 : -1;
    raw = original;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
        return -1;
    size_t length = 0;
    line[0] = '\0';
    for (;;) {
        unsigned char character;
        if (read(STDIN_FILENO, &character, 1) != 1)
            break;
        if (character == '\n' || character == '\r') {
            putchar('\n');
            break;
        }
        if (character == 127 || character == '\b') {
            if (length) {
                line[--length] = '\0';
                fputs("\b \b", stdout);
            }
        } else if (character == 27) {
            unsigned char sequence[2];
            if (read(STDIN_FILENO, sequence, 2) == 2 &&
                sequence[0] == '[' && sequence[1] == 'A' && *history) {
                while (length)
                    fputs("\b \b", stdout), line[--length] = '\0';
                snprintf(line, size, "%s", history);
                length = strlen(line);
                fputs(line, stdout);
            }
        } else if (length + 1 < size) {
            line[length++] = (char)character;
            line[length] = '\0';
            putchar(character);
        }
        fflush(stdout);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    if (line[0])
        snprintf(history, COMMAND_SIZE, "%s", line);
    return 0;
}

void ssh_console(const struct hosts *hosts)
{
    if (!hosts->count) {
        puts("\nNo SSH hosts discovered.");
        return;
    }
    struct config config = {0};
    char input[COMMAND_SIZE], history[COMMAND_SIZE] = "", user_buffer[64];
    load_config(&config);
    if (!*config.user) {
        int character;
        while ((character = getchar()) != '\n' && character != EOF)
            ;
        printf("\nSSH user: ");
        fflush(stdout);
        if (!fgets(user_buffer, sizeof user_buffer, stdin))
            return;
        user_buffer[strcspn(user_buffer, "\n")] = '\0';
        snprintf(config.user, sizeof config.user, "%s", user_buffer);
    }
    if (!*config.user) {
        fputs("SSH user cannot be empty.\n", stderr);
        return;
    }
    if (!*config.ssh_password &&
        ask_value("SSH password: ", config.ssh_password, sizeof config.ssh_password) < 0)
        return;
    if (save_config(&config) < 0)
        perror("config");

    struct job *jobs = calloc(hosts->count, sizeof *jobs);
    char watch[INET_ADDRSTRLEN] = "";
    int prompt = 1;
    if (!jobs)
        return;
    signal(SIGINT, interrupt_focused);
    puts("\nSSH console: update | exec <command> | focus <ip> | back | jobs | quit");
    for (;;) {
        if (pump_jobs(jobs, hosts->count, watch, 100))
            prompt = 1;
        if (prompt) {
            printf("ssh[%s]> ", watch);
            fflush(stdout);
            prompt = 0;
        }
        struct pollfd input_poll = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&input_poll, 1, 100) <= 0)
            continue;
        if (read_command(input, sizeof input, history) < 0) {
            while (job_count(jobs, hosts->count))
                pump_jobs(jobs, hosts->count, watch, 100);
            break;
        }
        input[strcspn(input, "\n")] = '\0';
        prompt = 1;
        if (!strcmp(input, "quit") || !strcmp(input, "exit")) {
            while (job_count(jobs, hosts->count))
                pump_jobs(jobs, hosts->count, watch, 100);
            break;
        }
        if (!strcmp(input, "help")) {
            puts("update       update all hosts");
            puts("exec <cmd>   execute <cmd> on all hosts");
            puts("jobs         show running SSH jobs");
            puts("focus <ip>   show one host and route Ctrl+C to it");
            puts("back         hide logs and stop focusing a host");
            puts("quit         leave the console");
        } else if (!strcmp(input, "jobs")) {
            for (size_t i = 0; i < hosts->count; i++) {
                if (!jobs[i].started)
                    printf("%s idle\n", jobs[i].ip);
                else if (jobs[i].active)
                    printf("%s running (pid %d)\n", jobs[i].ip, jobs[i].pid);
                else
                    printf("%s %s (status %d)\n", jobs[i].ip,
                           jobs[i].status ? "failed" : "completed",
                           jobs[i].status);
            }
        } else if (!strncmp(input, "focus ", 6) &&
                   (!strcmp(input + 6, "all") ||
                    (strlen(input + 6) < sizeof watch &&
                     inet_pton(AF_INET, input + 6, &(struct in_addr){0})))) {
            if (!strcmp(input + 6, "all"))
                watch[0] = '\0';
            else {
                snprintf(watch, sizeof watch, "%s", input + 6);
                show_log(watch);
            }
            focused_pid = 0;
            for (size_t i = 0; i < hosts->count; i++)
                if (!strcmp(watch, jobs[i].ip) && jobs[i].active)
                    focused_pid = jobs[i].pid;
        } else if (!strcmp(input, "back")) {
            watch[0] = '\0';
            focused_pid = 0;
        } else if (!strncmp(input, "update", 6)) {
            if (!*config.sudo_password &&
                ask_value("Sudo password: ", config.sudo_password,
                          sizeof config.sudo_password) < 0)
                continue;
            save_config(&config);

            if (!strcmp(input + 7, "all") || !strcmp(input, "update")) {
                start_all(&config, hosts, update_command, 1, jobs);
            } else {

                for (size_t i = 0; i < hosts->count; i++)
                    if (!strcmp(input + 7, jobs[i].ip)) {
                        start_one(&config, &hosts->items[i], update_command, 1, &jobs[0]);
                        break;
                    }
            }

        } else if (!strncmp(input, "exec ", 5) && input[5]) {
            start_all(&config, hosts, input + 5, 0, jobs);
        } else if (*input) {
            puts("Unknown command. Type help.");
        }
    }
    free(jobs);
}

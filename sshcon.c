#define _DEFAULT_SOURCE
#include "sshcon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define COMMAND_SIZE 1024
#define VALUE_SIZE 256

static const char update_command[] =
    ". /etc/os-release 2>/dev/null || { echo 'cannot detect distribution' >&2; exit 64; }; "
    "case \" $ID $ID_LIKE \" in "
    "*' debian '*|*' ubuntu '*|*' linuxmint '*) "
    "apt-get update && apt-get -y upgrade;; "
    "*' arch '*|*' manjaro '*|*' endeavouros '*) "
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

static int log_path(char *path, size_t size)
{
    if (config_path(path, size) < 0)
        return -1;
    char *name = strrchr(path, '/');
    if (!name || (size_t)snprintf(name, size - (size_t)(name - path),
                                  "ssh.log") >= size - (size_t)(name - path))
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

static int run_ssh(const struct config *config, const struct host *host,
                   const char *command, int sudo)
{
    char ip[INET_ADDRSTRLEN], target[VALUE_SIZE + INET_ADDRSTRLEN + 2];
    char remote_command[COMMAND_SIZE * 2];
    if (!inet_ntop(AF_INET, &host->ip, ip, sizeof ip))
        return -1;
    snprintf(target, sizeof target, "%s@%s", config->user, ip);

    int errors[2];
    if (pipe(errors) < 0) {
        perror("pipe");
        return -1;
    }
    int input[2] = {-1, -1};
    if (sudo && pipe(input) < 0) {
        perror("pipe");
        close(errors[0]);
        close(errors[1]);
        return -1;
    }
    char path[PATH_MAX];
    FILE *log = NULL;
    if (log_path(path, sizeof path) == 0)
        log = fopen(path, "a");
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(errors[0]);
        close(errors[1]);
        if (sudo)
            close(input[0]), close(input[1]);
        if (log)
            fclose(log);
        return -1;
    }
    if (!pid) {
        dup2(errors[1], STDERR_FILENO);
        close(errors[0]);
        close(errors[1]);
        if (sudo) {
            dup2(input[0], STDIN_FILENO);
            close(input[0]);
            close(input[1]);
        }
        setenv("SSHPASS", config->ssh_password, 1);
        const char *remote = command;
        if (sudo) {
            sudo_command(remote_command, sizeof remote_command, command);
            remote = remote_command;
        }
        execlp("sshpass", "sshpass", "-e", "ssh", "-o", "ConnectTimeout=5",
               "-o", "StrictHostKeyChecking=accept-new",
               target, remote, (char *)NULL);
        perror("sshpass");
        _exit(127);
    }
    close(errors[1]);
    if (sudo) {
        close(input[0]);
        dprintf(input[1], "%s\n", config->sudo_password);
        close(input[1]);
    }
    if (log)
        fprintf(log, "\n[%s] command: %s\n", ip, command);
    char buffer[512];
    ssize_t length;
    while ((length = read(errors[0], buffer, sizeof buffer)) > 0) {
        fwrite(buffer, 1, (size_t)length, stderr);
        if (log)
            fwrite(buffer, 1, (size_t)length, log);
    }
    close(errors[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    if (WIFEXITED(status))
        status = WEXITSTATUS(status);
    else
        status = 128 + WTERMSIG(status);
    if (log) {
        fprintf(log, "[%s] exit status: %d\n", ip, status);
        fclose(log);
    }
    if (status == 6)
        fprintf(stderr, "[%s] sshpass status 6: host key verification failed "
                        "or the host is unknown in known_hosts\n", ip);
    return status;
}

static void run_all(const struct config *config, const struct hosts *hosts,
                    const char *command, int sudo)
{
    for (size_t i = 0; i < hosts->count; i++) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &hosts->items[i].ip, ip, sizeof ip);
        printf("\n--- %s ---\n", ip);
        fflush(stdout);
        int status = run_ssh(config, &hosts->items[i], command, sudo);
        if (status)
            fprintf(stderr, "[%s] ssh failed (status %d)\n", ip, status);
    }
}

void ssh_console(const struct hosts *hosts)
{
    if (!hosts->count) {
        puts("\nNo SSH hosts discovered.");
        return;
    }
    struct config config = {0};
    char input[COMMAND_SIZE], user_buffer[64];
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

    puts("\nSSH console: update | exec <command> | help | quit");
    for (;;) {
        fputs("ssh> ", stdout);
        fflush(stdout);
        if (!fgets(input, sizeof input, stdin))
            break;
        input[strcspn(input, "\n")] = '\0';
        if (!strcmp(input, "quit") || !strcmp(input, "exit"))
            break;
        if (!strcmp(input, "help")) {
            puts("update       update all hosts");
            puts("exec <cmd>   execute <cmd> on all hosts");
            puts("quit         leave the console");
        } else if (!strcmp(input, "update")) {
            if (!*config.sudo_password &&
                ask_value("Sudo password: ", config.sudo_password,
                          sizeof config.sudo_password) < 0)
                continue;
            save_config(&config);
            run_all(&config, hosts, update_command, 1);
        } else if (!strncmp(input, "exec ", 5) && input[5]) {
            run_all(&config, hosts, input + 5, 0);
        } else if (*input) {
            puts("Unknown command. Type help.");
        }
    }
}

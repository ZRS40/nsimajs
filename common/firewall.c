#define _DEFAULT_SOURCE
#include "firewall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int have_ufw(void)
{
    return system("command -v ufw >/dev/null 2>&1") == 0;
}

static int ufw_active(void)
{
    /* Status may need root; treat "active" or unknown-as-try as worth attempting */
    FILE *fp = popen("ufw status 2>/dev/null | head -n1", "r");
    if (!fp)
        return 1;
    char line[256];
    int active = 1;
    if (fgets(line, sizeof line, fp)) {
        if (strstr(line, "inactive") || strstr(line, "Inactive"))
            active = 0;
    }
    pclose(fp);
    return active;
}

static int run_ufw(const char *cmd)
{
    /* Prefer direct ufw when root; otherwise passwordless sudo only
     * (interactive sudo is handled during nsimajd setup prompts). */
    char full[512];
    if (geteuid() == 0)
        snprintf(full, sizeof full, "%s", cmd);
    else
        snprintf(full, sizeof full, "sudo -n %s >/dev/null 2>&1", cmd);
    return system(full) == 0 ? 0 : -1;
}

int firewall_allow_tcp(int port, const char *comment)
{
    if (!have_ufw())
        return 0;
    if (!ufw_active())
        return 0;

    char cmd[384];
    if (comment && comment[0])
        snprintf(cmd, sizeof cmd, "ufw allow %d/tcp comment '%s'", port,
                 comment);
    else
        snprintf(cmd, sizeof cmd, "ufw allow %d/tcp", port);

    if (run_ufw(cmd) == 0) {
        fprintf(stderr, "[nsimaj] ufw: allowed %d/tcp\n", port);
        return 0;
    }
    fprintf(stderr,
            "[nsimaj] warning: could not open ufw port %d "
            "(run: sudo ufw allow %d/tcp)\n",
            port, port);
    return -1;
}

int firewall_allow_out_tcp(const char *host, int port, const char *comment)
{
    if (!have_ufw() || !host || !host[0])
        return 0;
    if (!ufw_active())
        return 0;

    char cmd[512];
    if (comment && comment[0])
        snprintf(cmd, sizeof cmd,
                 "ufw allow out proto tcp to %s port %d comment '%s'", host,
                 port, comment);
    else
        snprintf(cmd, sizeof cmd, "ufw allow out proto tcp to %s port %d", host,
                 port);

    if (run_ufw(cmd) == 0) {
        fprintf(stderr, "[nsimaj] ufw: allowed out to %s:%d\n", host, port);
        return 0;
    }
    fprintf(stderr,
            "[nsimaj] warning: could not add ufw out rule to %s:%d\n", host,
            port);
    return -1;
}

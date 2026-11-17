#define _DEFAULT_SOURCE
#include "selfupdate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void set_msg(char *msg, size_t msgsz, const char *text)
{
    if (msg && msgsz)
        snprintf(msg, msgsz, "%s", text);
}

int nsimaj_self_update(char *msg, size_t msgsz)
{
    /* Prefer systemd oneshot when the unit exists. */
    if (system("systemctl cat nsimajd-update.service >/dev/null 2>&1") == 0) {
        int rc = system("systemctl start nsimajd-update.service");
        if (rc != 0) {
            set_msg(msg, msgsz,
                    "self-update failed (try: sudo nsimajd update)");
            return -1;
        }
        system("systemctl status nsimajd-update.service --no-pager -l 2>/dev/null || true");
        system("journalctl -u nsimajd-update.service -n 20 --no-pager 2>/dev/null || true");
        set_msg(msg, msgsz, "Server self-update finished (see journalctl -u nsimajd-update)");
        return 0;
    }

    const char *root = getenv("NSIMAJ_ROOT");
    if (!root || !root[0])
        root = "/opt/nsimaj";

    char script[512];
    snprintf(script, sizeof script, "%s/bootstrap/nsimajd-update.sh", root);
    if (access(script, X_OK) != 0) {
        set_msg(msg, msgsz, "nsimajd-update not installed (run install-server.sh)");
        return -1;
    }

    char cmd[640];
    snprintf(cmd, sizeof cmd, "NSIMAJ_ROOT='%s' '%s'", root, script);
    int rc = system(cmd);
    if (rc != 0) {
        set_msg(msg, msgsz, "self-update script failed (check /var/log/nsimaj-update.log)");
        return -1;
    }
    set_msg(msg, msgsz, "Server self-update finished");
    return 0;
}

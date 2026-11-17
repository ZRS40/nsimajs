#define _DEFAULT_SOURCE
#include "osinfo.h"

#include "../common/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void osinfo_detect(char *distro, size_t dsz, char *family, size_t fsz)
{
    snprintf(distro, dsz, "unknown");
    snprintf(family, fsz, "unknown");

    char buf[4096];
    if (util_read_file("/etc/os-release", buf, sizeof buf) < 0)
        return;

    char name[128] = "";
    char id[128] = "";
    char like[256] = "";
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            const char *v = line + 12;
            if (*v == '"')
                v++;
            snprintf(name, sizeof name, "%s", v);
            size_t n = strlen(name);
            if (n && name[n - 1] == '"')
                name[n - 1] = '\0';
        } else if (strncmp(line, "ID=", 3) == 0) {
            const char *v = line + 3;
            if (*v == '"')
                v++;
            snprintf(id, sizeof id, "%s", v);
            size_t n = strlen(id);
            if (n && id[n - 1] == '"')
                id[n - 1] = '\0';
        } else if (strncmp(line, "ID_LIKE=", 8) == 0) {
            const char *v = line + 8;
            if (*v == '"')
                v++;
            snprintf(like, sizeof like, "%s", v);
            size_t n = strlen(like);
            if (n && like[n - 1] == '"')
                like[n - 1] = '\0';
        }
        if (!nl)
            break;
        line = nl + 1;
    }

    if (name[0])
        snprintf(distro, dsz, "%s", name);
    else if (id[0])
        snprintf(distro, dsz, "%s", id);

    if (strcmp(id, "arch") == 0 || strstr(like, "arch"))
        snprintf(family, fsz, "arch");
    else if (strcmp(id, "debian") == 0 || strcmp(id, "ubuntu") == 0 ||
             strcmp(id, "linuxmint") == 0 || strstr(like, "debian") ||
             strstr(like, "ubuntu"))
        snprintf(family, fsz, "debian");
}

int osinfo_pending_updates(const char *family)
{
    FILE *fp = NULL;
    if (strcmp(family, "arch") == 0) {
        fp = popen("pacman -Qu 2>/dev/null | wc -l", "r");
    } else if (strcmp(family, "debian") == 0) {
        /* Prefer apt list; fall back to simulated count */
        fp = popen("apt-get -s upgrade 2>/dev/null | "
                   "grep -c '^Inst ' || true",
                   "r");
    } else {
        return -1;
    }
    if (!fp)
        return -1;
    char line[64];
    int n = -1;
    if (fgets(line, sizeof line, fp))
        n = atoi(line);
    pclose(fp);
    return n;
}

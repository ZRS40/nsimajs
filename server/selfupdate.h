#ifndef NSIMAJ_SELFUPDATE_H
#define NSIMAJ_SELFUPDATE_H

#include <stddef.h>

/* Trigger server self-update (git tag + make + restart).
 * Prefers: systemctl start nsimajd-update
 * Fallback: /opt/nsimaj/bootstrap/nsimajd-update.sh (or NSIMAJ_ROOT).
 * Returns 0 on success, -1 on failure. Writes a short message to msg if non-NULL. */
int nsimaj_self_update(char *msg, size_t msgsz);

#endif

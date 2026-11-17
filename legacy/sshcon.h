#ifndef SSHCON_H
#define SSHCON_H

#include "network.h"

/* Keeps an interactive SSH command console until the user quits. */
void ssh_console(const struct hosts *);

#endif

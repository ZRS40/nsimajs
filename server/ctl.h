#ifndef NSIMAJ_CTL_H
#define NSIMAJ_CTL_H

#include "fleet.h"

#include "../common/frame.h"

#include <stddef.h>

#ifndef NSIMAJ_CTL_SOCK
#define NSIMAJ_CTL_SOCK "/run/nsimajd.sock"
#endif

struct ctl_server {
    int listen_fd;
    int client_fd;
    struct frame_reader reader;
    int client_focus; /* client is in focus view */
};

void ctl_init(struct ctl_server *c);
int ctl_listen(struct ctl_server *c);
void ctl_close(struct ctl_server *c);
void ctl_accept(struct ctl_server *c);
void ctl_disconnect_client(struct ctl_server *c);
void ctl_pump(struct ctl_server *c, struct fleet *f);
/* Push fleet snapshot when dirty (clears dirty). */
void ctl_push_state(struct ctl_server *c, struct fleet *f);
void ctl_send_focus_data(struct ctl_server *c, const unsigned char *data,
                         size_t len);
int ctl_has_client(const struct ctl_server *c);
int ctl_client_focusing(const struct ctl_server *c);

#endif

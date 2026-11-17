#ifndef NSIMAJ_AGENT_STATE_H
#define NSIMAJ_AGENT_STATE_H

#include "../common/frame.h"
#include "../common/json.h"
#include "../protocol/protocol.h"

#include <sys/types.h>

struct agent_state {
    int fd;
    int running;
    char server_host[NSIMAJ_HOST_LEN];
    int server_port;
    int http_port; /* bootstrap HTTP for self-update */
    char secret[NSIMAJ_SECRET_LEN];
    char id[NSIMAJ_ID_LEN];
    char hostname[NSIMAJ_HOST_LEN];
    char ip[NSIMAJ_IP_LEN];
    char distro[NSIMAJ_DISTRO_LEN];
    char family[32];
    int pending_updates;
    char job_id[NSIMAJ_ID_LEN];
    char job_stage[NSIMAJ_STAGE_LEN];
    int job_active;
    int focus_pty; /* master fd or -1 */
    pid_t focus_pid;
    struct frame_reader reader;
};

int agent_send_event(struct agent_state *st, const char *stage, long exit_code,
                     const char *error);
int agent_send_json(struct agent_state *st, struct json_buf *b);

#endif

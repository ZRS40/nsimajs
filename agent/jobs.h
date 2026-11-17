#ifndef NSIMAJ_AGENT_JOBS_H
#define NSIMAJ_AGENT_JOBS_H

#include <stddef.h>

struct agent_state;

int agent_job_start(struct agent_state *st, const char *job_id, const char *kind,
                    const char *payload);
int agent_job_cancel(struct agent_state *st, const char *job_id);
void agent_job_poll(struct agent_state *st);

int agent_focus_start(struct agent_state *st, const char *job_id);
void agent_focus_detach(struct agent_state *st);
void agent_focus_stdin(struct agent_state *st, const unsigned char *data,
                       size_t len);
void agent_focus_resize(struct agent_state *st, int rows, int cols);
void agent_focus_poll(struct agent_state *st);

const char *agent_tmux_name(const char *job_id, char *buf, size_t sz);

#endif

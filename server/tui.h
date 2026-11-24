#ifndef NSIMAJ_TUI_H
#define NSIMAJ_TUI_H

#include "fleet.h"

void tui_init(void);
void tui_restore(void);
void tui_draw(struct fleet *f);
/* Legacy in-process TUI (unused when client attaches to daemon). */
int tui_handle_input(struct fleet *f, unsigned char ch);
/* TUI client: local UI state + commands over ctl fd. */
int tui_handle_input_client(struct fleet *f, unsigned char ch, int ctl_fd);
void tui_enter_focus_client(struct fleet *f, int ctl_fd);
void tui_leave_focus(struct fleet *f);
int tui_in_focus(void);
void tui_write_focus(const unsigned char *data, size_t len);

/* Attaching / loading screen before first focus_data. */
void tui_begin_attach(struct fleet *f, const char *hostname);
void tui_cancel_attach(struct fleet *f, const char *reason);
int tui_in_attach(void);
/* Call when first focus_data arrives; enters focus if attaching. */
void tui_on_focus_data(struct fleet *f, int ctl_fd, const unsigned char *data,
                       size_t len);
/* Tick attaching spinner / timeout. Returns 1 if timed out. */
int tui_attach_tick(struct fleet *f, int ctl_fd);

#endif

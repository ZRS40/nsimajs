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

#endif

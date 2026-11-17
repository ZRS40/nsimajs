#ifndef NSIMAJ_HTTP_H
#define NSIMAJ_HTTP_H

#include "fleet.h"

int http_listen(struct fleet *f);
void http_close(struct fleet *f);
int http_accept_once(struct fleet *f);

#endif

#ifndef NSIMAJ_OSINFO_H
#define NSIMAJ_OSINFO_H

#include <stddef.h>

void osinfo_detect(char *distro, size_t dsz, char *family, size_t fsz);
int osinfo_pending_updates(const char *family);

#endif

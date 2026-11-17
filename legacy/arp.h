#ifndef ARP_H
#define ARP_H

#include "network.h"

/* Sends ARP requests on every host address and returns unique replies. */
int arp_scan(int, const struct network *, struct hosts *);

#endif

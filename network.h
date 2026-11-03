#ifndef NETWORK_H
#define NETWORK_H

#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>

/* Selected interface data shared with the ARP scanner. */
struct network {
    char name[IFNAMSIZ];
    struct in_addr ip, mask;
    uint8_t mac[6];
    unsigned ifindex;
};

struct host {
    struct in_addr ip;
    uint8_t mac[6];
};

struct hosts {
    struct host *items;
    size_t count;
};

/* Lists usable IPv4 interfaces, selects one, and reads its MAC/index. */
int network_select(struct network *);

#endif

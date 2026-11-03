#define _DEFAULT_SOURCE
#include "arp.h"

#include <arpa/inet.h>
#include <linux/if_arp.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define ARP_TIMEOUT 2000

struct arp_packet {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t oper;
    uint8_t sha[6], spa[4], tha[6], tpa[4];
} __attribute__((packed));

static int send_request(int fd, const struct network *net, uint32_t target)
{
    uint8_t buffer[sizeof(struct ether_header) + sizeof(struct arp_packet)] = {0};
    struct ether_header *eth = (struct ether_header *)buffer;
    struct arp_packet *arp =
        (struct arp_packet *)(buffer + sizeof(struct ether_header));
    memset(eth->ether_dhost, 0xff, 6);
    memcpy(eth->ether_shost, net->mac, 6);
    eth->ether_type = htons(ETH_P_ARP);
    arp->htype = htons(ARPHRD_ETHER);
    arp->ptype = htons(ETH_P_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARPOP_REQUEST);
    memcpy(arp->sha, net->mac, 6);
    memcpy(arp->spa, &net->ip.s_addr, 4);
    memcpy(arp->tpa, &target, 4);
    struct sockaddr_ll addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(ETH_P_ARP),
        .sll_ifindex = (int)net->ifindex,
        .sll_halen = 6
    };
    memset(addr.sll_addr, 0xff, 6);
    return sendto(fd, buffer, sizeof buffer, 0, (struct sockaddr *)&addr,
                  sizeof addr) < 0 ? -1 : 0;
}

int arp_scan(int fd, const struct network *net, struct hosts *hosts)
{
    uint32_t ip = ntohl(net->ip.s_addr), mask = ntohl(net->mask.s_addr);
    uint32_t network = ip & mask, broadcast = network | ~mask;
    uint32_t total = broadcast - network - 1, count = 0;
    char text[INET_ADDRSTRLEN];

    printf("\nInterface : %s\n", net->name);
    inet_ntop(AF_INET, &net->ip, text, sizeof text);
    printf("Local IP  : %s\n", text);
    inet_ntop(AF_INET, &net->mask, text, sizeof text);
    printf("Netmask   : %s\n", text);
    printf("\nScanning %u possible addresses...\n\n", total);
    uint8_t *seen = total ? calloc(total, 1) : NULL;
    hosts->items = total ? calloc(total, sizeof(*hosts->items)) : NULL;
    if (!seen || !hosts->items) {
        perror("calloc");
        free(seen);
        free(hosts->items);
        return -1;
    }
    struct timespec delay = {0, 1000000};
    for (uint32_t i = network + 1; i < broadcast; i++) {
        if (i != ip && send_request(fd, net, htonl(i)) < 0)
            perror("sendto");
        nanosleep(&delay, NULL);
    }

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    puts("Waiting for responses...");
    while (poll(&pfd, 1, ARP_TIMEOUT) > 0) {
        uint8_t buffer[2048];
        ssize_t len = recv(fd, buffer, sizeof buffer, 0);
        if (len < (ssize_t)(sizeof(struct ether_header) +
                            sizeof(struct arp_packet)))
            continue;
        struct ether_header *eth = (struct ether_header *)buffer;
        struct arp_packet *arp =
            (struct arp_packet *)(buffer + sizeof(struct ether_header));
        if (ntohs(eth->ether_type) != ETH_P_ARP ||
            ntohs(arp->oper) != ARPOP_REPLY)
            continue;
        struct in_addr sender_ip;
        memcpy(&sender_ip.s_addr, arp->spa, 4);
        uint32_t sender = ntohl(sender_ip.s_addr);
        if (sender <= network || sender >= broadcast ||
            seen[sender - network - 1])
            continue;
        seen[sender - network - 1] = 1;
        hosts->items[count].ip = sender_ip;
        memcpy(hosts->items[count].mac, arp->sha, 6);
        char mac[18];
        snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                 arp->sha[0], arp->sha[1], arp->sha[2],
                 arp->sha[3], arp->sha[4], arp->sha[5]);
        inet_ntop(AF_INET, &sender_ip, text, sizeof text);
        printf("[+] %-16s %s\n", text, mac);
        count++;
    }
    printf("\nFound %u devices.\n", count);
    hosts->count = count;
    free(seen);
    return 0;
}

#define _DEFAULT_SOURCE
#include <net/route.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <linux/if_arp.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <time.h>

#define ARP_TIMEOUT 2000

struct arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} __attribute__((packed));

struct device {
    struct in_addr ip;
    uint8_t mac[6];
};

static char interface_name[IFNAMSIZ];
static struct in_addr local_ip;
static struct in_addr netmask;
static uint8_t local_mac[6];

int get_network_info(void)
{
    struct ifaddrs *interfaces, *ifa;

    if (getifaddrs(&interfaces) == -1) {
        perror("getifaddrs");
        return -1;
    }

    struct {
        char name[IFNAMSIZ];
        struct in_addr ip;
        struct in_addr mask;
    } available[32];

    int count = 0;

    printf("\nAvailable network interfaces:\n\n");

    for (ifa = interfaces; ifa; ifa = ifa->ifa_next) {

        if (!ifa->ifa_addr || !ifa->ifa_netmask)
            continue;

        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;

        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;

        // Avoid duplicate interfaces
        int duplicate = 0;

        for (int i = 0; i < count; i++) {
            if (strcmp(available[i].name, ifa->ifa_name) == 0) {
                duplicate = 1;
                break;
            }
        }

        if (duplicate || count >= 32)
            continue;

        strncpy(available[count].name, ifa->ifa_name, IFNAMSIZ - 1);

        available[count].ip =
            ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;

        available[count].mask =
            ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr;

        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &available[count].ip, ip, sizeof(ip));

        printf(" [%d] %-15s %s\n", count + 1,
               available[count].name, ip);

        count++;
    }

    freeifaddrs(interfaces);

    if (count == 0) {
        fprintf(stderr, "No active network interface found.\n");
        return -1;
    }

    int choice = 0;

    if (count == 1) {
        choice = 1;
        printf("\nAutomatically selected: %s\n",
               available[0].name);
    } else {
        printf("\nSelect an interface (1-%d): ", count);

        if (scanf("%d", &choice) != 1 ||
            choice < 1 || choice > count) {
            fprintf(stderr, "Invalid interface selection.\n");
            return -1;
        }
    }

    choice--;

    strncpy(interface_name, available[choice].name,
            IFNAMSIZ - 1);

    local_ip = available[choice].ip;
    netmask = available[choice].mask;

    // Retrieve MAC address
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct ifreq ifr = {0};

    strncpy(ifr.ifr_name, interface_name, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("ioctl SIOCGIFHWADDR");
        close(fd);
        return -1;
    }

    memcpy(local_mac, ifr.ifr_hwaddr.sa_data, 6);

    close(fd);

    return 0;
}

int send_arp_request(int fd, int ifindex, uint32_t target_ip)
{
    uint8_t buffer[sizeof(struct ether_header) +
                   sizeof(struct arp_packet)] = {0};

    struct ether_header *eth = (struct ether_header *)buffer;
    struct arp_packet *arp =
        (struct arp_packet *)(buffer + sizeof(struct ether_header));

    memset(eth->ether_dhost, 0xff, 6);
    memcpy(eth->ether_shost, local_mac, 6);
    eth->ether_type = htons(ETH_P_ARP);

    arp->htype = htons(ARPHRD_ETHER);
    arp->ptype = htons(ETH_P_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARPOP_REQUEST);

    memcpy(arp->sha, local_mac, 6);
    memcpy(arp->spa, &local_ip.s_addr, 4);

    memset(arp->tha, 0, 6);
    memcpy(arp->tpa, &target_ip, 4);

    struct sockaddr_ll addr = {0};

    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ARP);
    addr.sll_ifindex = ifindex;
    addr.sll_halen = 6;

    memset(addr.sll_addr, 0xff, 6);

    return sendto(fd, buffer, sizeof(buffer), 0,
                  (struct sockaddr *)&addr, sizeof(addr)) < 0 ? -1 : 0;
}

void scan_network(int fd, int ifindex)
{
    uint32_t ip = ntohl(local_ip.s_addr);
    uint32_t mask = ntohl(netmask.s_addr);

    uint32_t network = ip & mask;
    uint32_t broadcast = network | ~mask;

    uint32_t total = broadcast - network - 1;

    printf("\nInterface : %s\n", interface_name);

    char ip_string[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &local_ip, ip_string, sizeof(ip_string));
    printf("Local IP  : %s\n", ip_string);

    inet_ntop(AF_INET, &netmask, ip_string, sizeof(ip_string));
    printf("Netmask   : %s\n", ip_string);

    printf("\nScanning %u possible addresses...\n\n", total);

    struct device *devices = calloc(total, sizeof(struct device));

    if (!devices) {
        perror("calloc");
        return;
    }

    uint32_t count = 0;

    // Send ARP requests
    for (uint32_t i = network + 1; i < broadcast; i++) {

        if (i == ip)
            continue;

        if (send_arp_request(fd, ifindex, htonl(i)) < 0)
            perror("sendto");

        // Small delay to avoid overwhelming the network
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 1000000
        };

        nanosleep(&delay, NULL);
    }

    // Receive ARP replies
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN
    };

    printf("Waiting for responses...\n");

    while (poll(&pfd, 1, ARP_TIMEOUT) > 0) {

        uint8_t buffer[2048];

        ssize_t len = recv(fd, buffer, sizeof(buffer), 0);

        if (len < (ssize_t)(sizeof(struct ether_header) +
                            sizeof(struct arp_packet)))
            continue;

        struct ether_header *eth =
            (struct ether_header *)buffer;

        if (ntohs(eth->ether_type) != ETH_P_ARP)
            continue;

        struct arp_packet *arp =
            (struct arp_packet *)(buffer + sizeof(struct ether_header));

        if (ntohs(arp->oper) != ARPOP_REPLY)
            continue;

        struct in_addr sender_ip;
        memcpy(&sender_ip.s_addr, arp->spa, 4);

        uint32_t sender = ntohl(sender_ip.s_addr);

        if (sender <= network || sender >= broadcast)
            continue;

        // Avoid duplicates
        int duplicate = 0;

        for (uint32_t i = 0; i < count; i++) {
            if (devices[i].ip.s_addr == sender_ip.s_addr) {
                duplicate = 1;
                break;
            }
        }

        if (duplicate)
            continue;

        devices[count].ip = sender_ip;
        memcpy(devices[count].mac, arp->sha, 6);
        count++;

        char mac[18];

        snprintf(mac, sizeof(mac),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 arp->sha[0], arp->sha[1],
                 arp->sha[2], arp->sha[3],
                 arp->sha[4], arp->sha[5]);

        inet_ntop(AF_INET, &sender_ip, ip_string, sizeof(ip_string));

        printf("[+] %-16s %s\n", ip_string, mac);
    }

    printf("\nFound %u devices.\n", count);

    free(devices);
}

int main(void)
{
    if (get_network_info() < 0)
        return 1;

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP));

    if (fd < 0) {
        perror("socket AF_PACKET");
        return 1;
    }

    int ifindex = if_nametoindex(interface_name);

    if (!ifindex) {
        perror("if_nametoindex");
        close(fd);
        return 1;
    }

    struct sockaddr_ll addr = {0};

    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ARP);
    addr.sll_ifindex = ifindex;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    scan_network(fd, ifindex);

    close(fd);

    return 0;
}
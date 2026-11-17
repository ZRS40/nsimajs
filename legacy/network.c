#define _DEFAULT_SOURCE
#include "network.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_INTERFACES 32

int network_select(struct network *selected)
{
    struct ifaddrs *list, *ifa;
    struct network available[MAX_INTERFACES];
    int count = 0;

    if (getifaddrs(&list) < 0) {
        perror("getifaddrs");
        return -1;
    }
    puts("\nAvailable network interfaces:\n");
    for (ifa = list; ifa && count < MAX_INTERFACES; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask ||
            ifa->ifa_addr->sa_family != AF_INET ||
            !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        int duplicate = 0;
        for (int i = 0; i < count; i++)
            if (!strcmp(available[i].name, ifa->ifa_name)) {
                duplicate = 1;
                break;
            }
        if (duplicate)
            continue;
        snprintf(available[count].name, IFNAMSIZ, "%s", ifa->ifa_name);
        available[count].ip = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
        available[count].mask =
            ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &available[count].ip, ip, sizeof ip);
        printf(" [%d] %-15s %s\n", count + 1, available[count].name, ip);
        count++;
    }
    freeifaddrs(list);
    if (!count) {
        fputs("No active network interface found.\n", stderr);
        return -1;
    }

    int choice = 1;
    if (count == 1)
        printf("\nAutomatically selected: %s\n", available[0].name);
    else {
        printf("\nSelect an interface (1-%d): ", count);
        if (scanf("%d", &choice) != 1 || choice < 1 || choice > count) {
            fputs("Invalid interface selection.\n", stderr);
            return -1;
        }
    }
    *selected = available[choice - 1];
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", selected->name);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("ioctl SIOCGIFHWADDR");
        close(fd);
        return -1;
    }
    memcpy(selected->mac, ifr.ifr_hwaddr.sa_data, sizeof selected->mac);
    close(fd);
    selected->ifindex = if_nametoindex(selected->name);
    if (!selected->ifindex) {
        perror("if_nametoindex");
        return -1;
    }
    return 0;
}

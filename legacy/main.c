#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arp.h"
#include "sshcon.h"
#include "network.h"

static int become_root(const char *program)
{
    char *password = getpass("[sudo] Password: ");
    if (!password)
        return -1;
    int input[2];
    if (pipe(input) < 0) {
        perror("pipe");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(input[0]);
        close(input[1]);
        return -1;
    }
    if (!pid) {
        dup2(input[0], STDIN_FILENO);
        close(input[0]);
        close(input[1]);
        execlp("sudo", "sudo", "-S", "-p", "", "-v", (char *)NULL);
        perror("sudo");
        _exit(127);
    }
    close(input[0]);
    dprintf(input[1], "%s\n", password);
    close(input[1]);
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        return -1;
    execlp("sudo", "sudo", program, (char *)NULL);
    perror("sudo");
    return -1;
}

int main(int argc, char **argv)
{
    if (geteuid() && become_root(argc ? argv[0] : "./app") < 0)
        return 1;
    struct network net;
    if (network_select(&net) < 0)
        return 1;

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP));
    if (fd < 0) {
        perror("socket AF_PACKET");
        return 1;
    }
    struct sockaddr_ll addr = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(ETH_P_ARP),
        .sll_ifindex = (int)net.ifindex
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }
    struct hosts hosts = {0};
    if (arp_scan(fd, &net, &hosts) < 0) {
        close(fd);
        return 1;
    }
    close(fd);
    ssh_console(&hosts);
    free(hosts.items);
    return 0;
}

#ifndef NSIMAJ_FIREWALL_H
#define NSIMAJ_FIREWALL_H

/* Open a TCP port with ufw when available/active. comment is for ufw rule. */
int firewall_allow_tcp(int port, const char *comment);

/* Allow outbound TCP to host:port (agent side). */
int firewall_allow_out_tcp(const char *host, int port, const char *comment);

#endif

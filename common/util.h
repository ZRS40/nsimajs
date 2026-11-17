#ifndef NSIMAJ_UTIL_H
#define NSIMAJ_UTIL_H

#include <stddef.h>

void util_trim(char *s);
int util_read_file(const char *path, char *buf, size_t sz);
int util_write_file(const char *path, const char *data, int mode);
int util_mkdir_p(const char *path, int mode);
const char *util_home_dir(void);
int util_config_dir(char *out, size_t sz);
void util_hostname(char *out, size_t sz);
/* Best-effort primary IPv4 used to reach the LAN (empty string on failure). */
void util_primary_ipv4(char *out, size_t sz);
void util_gen_id(char *out, size_t sz);
long util_now(void);
int util_sleep_ms(int ms);
/* Prompt on a TTY. hide=1 disables echo (for secrets). Returns 0 on success. */
int util_prompt(const char *prompt, char *out, size_t sz, int hide);

#endif

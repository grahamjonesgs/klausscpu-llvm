/* sshkeys.h — SSH host key management for KlaussCPU (Zephyr). */
#ifndef SSHKEYS_H
#define SSHKEYS_H

#include <stdint.h>
#include <stddef.h>

#define SSH_KEY_FILENAME "ssh_host_key.bin"

int sshkeys_init(void);
int sshkeys_persist(void);
const uint8_t *sshkeys_get_private(void);
size_t sshkeys_get_private_len(void);

#endif /* SSHKEYS_H */

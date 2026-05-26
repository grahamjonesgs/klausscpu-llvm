/* ssh_server.h — SSH server for KlaussCPU Zephyr. */
#ifndef SSH_SERVER_H
#define SSH_SERVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSH_PORT        22
#define SSH_MAX_CONNS   2
#define SSH_STACK_SIZE  8192
#define SSH_PRIO        5

typedef int (*ssh_auth_cb_t)(const char *user, const char *password,
			     const uint8_t *pubkey, size_t pubkey_len);

int ssh_server_start(void);
void ssh_server_set_auth_cb(ssh_auth_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* SSH_SERVER_H */

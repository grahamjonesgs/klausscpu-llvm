/* ssh_server.h — SSH server task for KlaussCPU FreeRTOS console.
 *
 * Starts a wolfSSH server on TCP port 22 that exposes the same interactive
 * shell as the telnet server (telnet/telnet.c), but over an encrypted
 * SSH connection using hardware-accelerated AES-GCM / AES-CTR.
 *
 * Prerequisites:
 *   wolfSSL + wolfSSH built (run build-wolfssl.sh and build-wolfssh.sh).
 *   wolfssl_hw_init() called before ssh_server_start().
 *   sshkeys_init() called before ssh_server_start().
 *   TCP/IP stack initialized (tcpip_init done, DHCP up).
 *
 * Cipher suite negotiated (in preference order):
 *   KEX:    curve25519-sha256
 *   Cipher: aes128-gcm@openssh.com  (hardware AES-GCM)
 *           aes128-ctr              (hardware AES-CTR fallback)
 *   MAC:    hmac-sha2-256           (wolfCrypt software)
 *   HostKey: ssh-ed25519
 */
#ifndef SSH_SERVER_H
#define SSH_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#define SSH_PORT       22
#define SSH_MAX_CONNS  2        /* simultaneous SSH sessions */
#define SSH_TASK_STACK 4096     /* words (32 KB on KlaussCPU) */
#define SSH_TASK_PRIO  2

/* Start the SSH listener task.  Returns 0 on success. */
int ssh_server_start(void);

/* Authentication callback type — return 0 to allow, non-zero to reject. */
typedef int (*ssh_auth_cb_t)(const char *user, const char *password,
                             const uint8_t *pubkey, size_t pubkey_len);

/* Override the default (password "klausscpu" for user "admin").
 * Must be called before ssh_server_start(). */
void ssh_server_set_auth_cb(ssh_auth_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* SSH_SERVER_H */

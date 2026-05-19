/* sshkeys.h — SSH host key management for KlaussCPU.
 *
 * Generates an ECDSA P-256 host key pair on first call (wolfSSH does not
 * support Ed25519 host keys in this version).  Key is stored in SEC1 DER
 * format.  Optionally loads/saves from SD card for host identity persistence.
 */
#ifndef SSHKEYS_H
#define SSHKEYS_H

#include <stdint.h>
#include <stddef.h>

#define SSH_KEY_PATH "0:ssh_host_key.bin"

/* Initialize SSH host key.
 * Tries to load an existing key from SD card; if unavailable (SD not mounted
 * pre-scheduler), generates a new ECDSA P-256 key pair from TRNG.
 * Returns 0 on success, negative on failure. */
int sshkeys_init(void);

/* Persist a freshly-generated key to SD card.  No-op if the key was loaded
 * from SD.  Must be called from a task context after FatFs is ready.
 * Returns 0 on success or if no persistence is needed. */
int sshkeys_persist(void);

/* Return the SEC1 DER-encoded ECDSA P-256 private key (≈121 bytes).
 * Valid after sshkeys_init(). */
const uint8_t *sshkeys_get_private(void);
size_t         sshkeys_get_private_len(void);

const uint8_t *sshkeys_get_public(void);
size_t         sshkeys_get_public_len(void);

#endif /* SSHKEYS_H */

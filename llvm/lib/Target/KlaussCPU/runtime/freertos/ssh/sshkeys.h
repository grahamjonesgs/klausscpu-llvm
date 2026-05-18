/* sshkeys.h — SSH host key management for KlaussCPU.
 *
 * Generates an Ed25519 host key pair using wolfCrypt + TRNG on first call,
 * and caches it in RAM.  Optionally stores/loads from SD card for persistent
 * host identity (avoids "host key changed" warnings across reboots).
 */
#ifndef SSHKEYS_H
#define SSHKEYS_H

#include <stdint.h>

#define SSH_KEY_PATH "0:ssh_host_key.bin"  /* FatFs path: SD card, partition 0 */

/* Initialize SSH host key.
 * If SD card is mounted and SSH_KEY_PATH exists, load the stored key.
 * Otherwise generate a new Ed25519 key pair from TRNG and (optionally) save it.
 * Returns 0 on success, negative on failure. */
int sshkeys_init(int save_to_sd);

/* Return the DER-encoded Ed25519 private key (64 bytes: priv‖pub).
 * Valid after sshkeys_init(). */
const uint8_t *sshkeys_get_private(void);
size_t         sshkeys_get_private_len(void);

/* Return the DER-encoded Ed25519 public key (32 bytes).
 * Valid after sshkeys_init(). */
const uint8_t *sshkeys_get_public(void);
size_t         sshkeys_get_public_len(void);

#endif /* SSHKEYS_H */

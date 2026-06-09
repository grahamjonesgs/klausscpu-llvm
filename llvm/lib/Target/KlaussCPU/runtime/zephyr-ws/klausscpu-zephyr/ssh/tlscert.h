/* tlscert.h — TLS server certificate + key management for KlaussCPU (Zephyr).
 *
 * Self-signed ECDSA P-256 cert/key, generated on first boot and persisted to
 * the SD card (mirrors sshkeys.c, which does the same for the SSH host key).
 */
#ifndef TLSCERT_H
#define TLSCERT_H

#include <stdint.h>
#include <stddef.h>

#define TLS_KEY_FILENAME  "tls_key.bin"   /* EC private key (DER)         */
#define TLS_CERT_FILENAME "tls_cert.bin"  /* self-signed certificate (DER) */

/* Load the cert+key from SD, or generate a fresh self-signed pair and persist
 * it.  Call once before wolfSSL_CTX_use_*_buffer().  Returns 0 on success. */
int tlscert_init(void);

const uint8_t *tlscert_get_cert(void);
size_t         tlscert_get_cert_len(void);
const uint8_t *tlscert_get_key(void);
size_t         tlscert_get_key_len(void);

#endif /* TLSCERT_H */

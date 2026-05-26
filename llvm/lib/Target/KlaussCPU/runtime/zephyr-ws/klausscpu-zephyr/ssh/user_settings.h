/* user_settings.h — wolfSSL configuration for KlaussCPU Zephyr SSH server.
 *
 * Adapted from the FreeRTOS version (freertos/wolfssl/user_settings.h).
 * Key change: WOLFSSL_ZEPHYR replaces FREERTOS for memory/time primitives.
 */
#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

/* ── Target / OS ─────────────────────────────────────────────────────────── */

#define WOLFSSL_ZEPHYR             /* Zephyr memory + time primitives */
#define NO_FILESYSTEM              /* no fopen/fclose in wolfSSL core */
#define WOLFSSL_NO_SOCK            /* don't compile default socket layer */
#define WOLFSSL_USER_IO            /* suppress EmbedReceive/EmbedSend */

/* ── C type widths (KlaussCPU: long = 64 bits) ───────────────────────────── */

#define SIZEOF_LONG      8
#define SIZEOF_LONG_LONG 8

/* ── Footprint ───────────────────────────────────────────────────────────── */

#define WOLFSSL_NO_CLIENT          /* server-side TLS code only */

/* ── Enabled algorithms ──────────────────────────────────────────────────── */

#define HAVE_AES_CBC
#define HAVE_AES_CTR
#define WOLFSSL_AES_COUNTER
#define HAVE_AESGCM
#define HAVE_AES_ECB

#define WOLFSSL_SHA256
#define WOLFSSL_SHA512
#define NO_SHA
#define NO_MD5
#define NO_OLD_TLS

#define HAVE_HMAC

#define HAVE_ECC
#define HAVE_CURVE25519
#define HAVE_ED25519
#define HAVE_ED25519_SIGN
#define HAVE_ED25519_VERIFY
#define HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_EXPORT

/* ── RNG ─────────────────────────────────────────────────────────────────── */

#define HAVE_HASHDRBG

int klausscpu_trng_seed(unsigned char *output, unsigned int sz);
#define CUSTOM_RAND_GENERATE_SEED klausscpu_trng_seed

/* ── Hardware crypto callbacks ───────────────────────────────────────────── */

#define WOLF_CRYPTO_CB
#define KLAUSSCPU_CRYPTO_DEV_ID  1

/* ── ASN.1 parser ────────────────────────────────────────────────────────── */

#define WOLFSSL_ASN_ORIGINAL

/* ── Disabled features ───────────────────────────────────────────────────── */

#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_RC4
#define NO_DES3
#define NO_PSK
#define NO_WOLFSSL_CLIENT
#define NO_FIPS
#define WC_NO_HARDEN

/* ── Performance ─────────────────────────────────────────────────────────── */

#define WOLFSSL_SMALL_STACK
#define WOLFSSL_SMALL_STACK_CACHE
#define SP_WORD_SIZE 32

#endif /* WOLFSSL_USER_SETTINGS_H */

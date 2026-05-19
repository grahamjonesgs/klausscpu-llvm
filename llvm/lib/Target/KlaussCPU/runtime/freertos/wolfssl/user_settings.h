/* user_settings.h — wolfSSL configuration for KlaussCPU FreeRTOS SSH server.
 *
 * Used by both wolfSSL (crypto) and wolfSSH (SSH protocol).
 * Set WOLFSSL_USER_SETTINGS before including any wolfSSL/wolfSSH header.
 */
#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

/* ── Target / OS ─────────────────────────────────────────────────────────── */

#define FREERTOS               /* FreeRTOS memory + time primitives */
#define WOLFSSL_FREERTOS
#define NO_FILESYSTEM          /* no fopen/fclose in wolfSSL core    */
#define WOLFSSL_NO_SOCK        /* don't compile default socket layer  */
#define WOLFSSL_USER_IO        /* suppress EmbedReceive/EmbedSend in internal.c */

/* ── C type widths (KlaussCPU: long = 64 bits) ───────────────────────────── */

#define SIZEOF_LONG      8
#define SIZEOF_LONG_LONG 8

/* ── Footprint ───────────────────────────────────────────────────────────── */
/* Do NOT define WOLFCRYPT_ONLY: wolfSSH calls wolfSSL_Init() from ssl.c. */
#define WOLFSSL_NO_CLIENT      /* server-side TLS code only */

/* ── Enabled algorithms ──────────────────────────────────────────────────── */

/* AES */
#define HAVE_AES_CBC
#define HAVE_AES_CTR
#define WOLFSSL_AES_COUNTER    /* internal guard for cryptocb.h CTR sub-struct */
#define HAVE_AESGCM
#define HAVE_AES_ECB

/* Hashes */
#define WOLFSSL_SHA256
#define WOLFSSL_SHA512         /* required by Ed25519 (hashes with SHA-512) */
#define NO_SHA                 /* disable SHA-1 */
#define NO_MD5
#define NO_OLD_TLS             /* old TLS ciphersuites require SHA-1 + MD5 */

/* HMAC */
#define HAVE_HMAC

/* ECC / EdDSA / Curve25519 (pure software) */
#define HAVE_ECC
#define HAVE_CURVE25519
#define HAVE_ED25519
#define HAVE_ED25519_SIGN
#define HAVE_ED25519_VERIFY
#define HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_EXPORT

/* ── RNG ─────────────────────────────────────────────────────────────────── */

#define HAVE_HASHDRBG          /* HMAC-DRBG seeded from TRNG */

/* Override wolfSSL's wc_GenerateSeed with our TRNG-backed implementation.
 * wolfSSL 5.7 calls CUSTOM_RAND_GENERATE_SEED(output, sz) — two args, no OS_Seed.
 * The forward declaration is needed because random.c includes this header before
 * seeing the definition in wolfssl_hw.c. */
int klausscpu_trng_seed(unsigned char *output, unsigned int sz);
#define CUSTOM_RAND_GENERATE_SEED klausscpu_trng_seed

/* ── Memory ──────────────────────────────────────────────────────────────── */

/* FREERTOS define above already maps XMALLOC/XFREE to pvPortMalloc/vPortFree
 * in wolfSSL's wc_port.h.  Do NOT define XMALLOC_USER (that would require us
 * to provide external XMALLOC/XFREE symbols which causes linker errors). */

/* ── Hardware crypto callbacks ───────────────────────────────────────────── */

#define WOLF_CRYPTO_CB
#define KLAUSSCPU_CRYPTO_DEV_ID  1

/* ── ASN.1 parser ────────────────────────────────────────────────────────── */

/* Force the old-style (non-template) ASN.1 decoder.
 * wolfSSL 5.7.2 settings.h auto-enables WOLFSSL_ASN_TEMPLATE which routes
 * through a template engine whose GetASN_Items / ToTraditionalInline_ex path
 * fails with ASN_PARSE_E on all ECC key formats (SEC1 and PKCS#8) on KlaussCPU.
 * The old decoder (wc_EccPrivateKeyDecode #ifndef WOLFSSL_ASN_TEMPLATE branch)
 * works correctly.  WOLFSSL_ASN_ORIGINAL suppresses the auto-define. */
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
#define WC_NO_HARDEN           /* suppress timing/side-channel #warning */

/* ── Performance ─────────────────────────────────────────────────────────── */

#define WOLFSSL_SMALL_STACK
#define WOLFSSL_SMALL_STACK_CACHE

/* Force 32-bit SP math limbs.
 * wolfSSL's 64-bit SP math path uses __uint128_t for intermediate products and
 * allocates much larger per-operation buffers during ECC key generation.  This
 * causes heap corruption on KlaussCPU (buffer overrun in sp_256 scalar mult).
 * 32-bit limbs produce correct results and were the verified working config. */
#define SP_WORD_SIZE 32
/* Note: CURVED25519_SMALL and ED25519_SMALL removed — they redirect to
 * ge_low_mem.c which cmake may not compile, leaving ge_ and fe_ symbols undefined. */

#endif /* WOLFSSL_USER_SETTINGS_H */

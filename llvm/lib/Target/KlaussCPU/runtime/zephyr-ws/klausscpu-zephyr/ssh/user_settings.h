/* user_settings.h — wolfSSL configuration for KlaussCPU Zephyr SSH server.
 *
 * Adapted from the FreeRTOS version (freertos/wolfssl/user_settings.h).
 * Key change: WOLFSSL_ZEPHYR replaces FREERTOS for memory/time primitives.
 */
#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

/* ── Target / OS ─────────────────────────────────────────────────────────── */

#define SINGLE_THREADED            /* no pthreads needed; single-core CPU */
/* NO_FILESYSTEM intentionally NOT defined: wolfSSH SFTP needs a filesystem.
 * With it defined, wolfSSH port.h takes the NO_FILESYSTEM branch (which wins
 * over the WOLFSSH_ZEPHYR branch) and stubs out all SFTP file ops.  Instead we
 * use the Zephyr FS port (wolfSSL auto-defines WOLFSSL_ZEPHYR -> z_fs_open/etc.
 * mapped onto Zephyr fs_*).  The server still loads its host key from memory
 * (sshkeys.c), so no file I/O happens for auth. */
#define WOLFSSL_NO_SOCK            /* don't compile default socket layer */
#define WOLFSSL_USER_IO            /* suppress EmbedReceive/EmbedSend */

/* Select the Zephyr filesystem/RTOS port in wolfSSL+wolfSSH headers (port.h
 * would otherwise fall through to the POSIX branch -> <unistd.h> not found).
 * The wolfSSL/wolfSSH module CMake sets these as -D only for the `app` target;
 * our ssh sources live in a separate zephyr_library, so define them here too
 * (user_settings.h reaches every wolf TU).  Guarded to avoid -D redefinition. */
#ifndef WOLFSSL_ZEPHYR
#define WOLFSSL_ZEPHYR
#endif
#ifndef WOLFSSH_ZEPHYR
#define WOLFSSH_ZEPHYR
#endif

/* Memory: use Zephyr's k_malloc/k_free */
#include <zephyr/kernel.h>
#define XMALLOC(s, h, t)     k_malloc(s)
#define XFREE(p, h, t)       k_free(p)
#define XREALLOC(p, n, h, t) k_realloc(p, n)
#define XMALLOC_USER

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

/* ── wolfSSH SFTP / SCP (file transfer over the SSH transport) ────────────── */

#define WOLFSSH_SFTP               /* compile wolfsftp.c + the sftp subsystem */
/* WOLFSSH_SCP left off: wolfscp.c needs a complete struct timeval, which this
 * minimal-libc config doesn't provide (SFTP doesn't need it).  SFTP covers the
 * file-transfer use case (Cyberduck). */
#define NO_WOLFSSL_DIR             /* use wolfSSH's dir handling, not wolfSSL's */
#define WOLFSSH_MAX_SFTP_RW 8192   /* max SFTP read/write chunk                */
#define DEFAULT_WINDOW_SZ (128 * 128)  /* 16 KiB SSH window — SFTP throughput  */

#endif /* WOLFSSL_USER_SETTINGS_H */

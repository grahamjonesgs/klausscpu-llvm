/* sshkeys.c — SSH host key management (ECDSA P-256).
 *
 * wolfSSH in this version does NOT support Ed25519 host keys — IdentifyAsn1Key()
 * only recognises RSA and ECDSA.  We therefore use an ECDSA P-256 host key,
 * stored in SEC1 DER format (output of wc_EccKeyToDer).
 *
 * Storage on SD card: the raw SEC1 DER bytes (≈121 bytes for P-256).
 * In-memory: same DER buffer passed directly to wolfSSH_CTX_UsePrivateKey_buffer.
 *
 * sshkeys_init() is called before vTaskStartScheduler, so the SD card
 * (FatFs) may not be mounted yet.  Generation always succeeds; call
 * sshkeys_persist() from a task context to save the key to SD.
 */

#define WOLFSSL_USER_SETTINGS
#include "../wolfssl/user_settings.h"

#include <stdio.h>
#include <string.h>

#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/asn_public.h>

#include "ff.h"
#include "sshkeys.h"

/* SEC1 DER for a P-256 key is ≈121 bytes. */
#define PRIV_DER_MAX  256

static uint8_t s_priv_der[PRIV_DER_MAX];
static int     s_priv_der_len;
static int     s_initialized;
static int     s_needs_persist;   /* 1 if key was generated (not loaded from SD) */

/* Single FATFS object shared by load and save — FatFs only allows one
 * registered object per drive at a time. */
static FATFS   s_fatfs;
static int     s_fatfs_mounted;

static int sd_ensure_mounted(void) {
    if (s_fatfs_mounted) return 0;
    FRESULT fr = f_mount(&s_fatfs, "0:", 1);
    if (fr != FR_OK) {
        printf("sshkeys: SD mount failed (fr=%d)\n", (int)fr);
        return -1;
    }
    s_fatfs_mounted = 1;
    return 0;
}

static int generate_keypair(void) {
    ecc_key key;
    WC_RNG  rng;

    int rc = wc_InitRng(&rng);
    if (rc != 0) return rc;

    rc = wc_ecc_init(&key);
    if (rc != 0) { wc_FreeRng(&rng); return rc; }

    rc = wc_ecc_make_key(&rng, 32, &key);  /* P-256 */
    if (rc != 0) { wc_ecc_free(&key); wc_FreeRng(&rng); return rc; }

    rc = wc_EccKeyToDer(&key, s_priv_der, (word32)sizeof(s_priv_der));
    wc_ecc_free(&key);
    wc_FreeRng(&rng);
    if (rc < 0) return rc;
    s_priv_der_len = rc;
    return 0;
}

static int load_from_sd(void) {
    if (sd_ensure_mounted() != 0) return -1;
    FIL f;
    FRESULT fr = f_open(&f, SSH_KEY_PATH, FA_READ);
    if (fr != FR_OK) return -1;

    UINT br;
    fr = f_read(&f, s_priv_der, PRIV_DER_MAX, &br);
    f_close(&f);
    if (fr != FR_OK || br < 16 || s_priv_der[0] != 0x30) return -1;

    s_priv_der_len = (int)br;
    return 0;
}

static int save_to_sd_file(void) {
    FIL f;
    FRESULT fr = f_open(&f, SSH_KEY_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("sshkeys: f_open write failed (fr=%d)\n", (int)fr);
        return -1;
    }
    UINT bw;
    fr = f_write(&f, s_priv_der, (UINT)s_priv_der_len, &bw);
    f_close(&f);
    if (fr != FR_OK || (int)bw != s_priv_der_len) {
        printf("sshkeys: f_write failed (fr=%d bw=%u)\n", (int)fr, (unsigned)bw);
        return -1;
    }
    return 0;
}

int sshkeys_init(void) {
    if (s_initialized) return 0;

    if (load_from_sd() == 0) {
        printf("sshkeys: loaded host key from SD (%d bytes)\n", s_priv_der_len);
        s_initialized = 1;
        return 0;
    }

    int rc = generate_keypair();
    if (rc != 0) {
        printf("sshkeys: key generation failed (%d)\n", rc);
        return rc;
    }
    printf("sshkeys: generated new ECDSA P-256 host key (%d bytes)\n",
           s_priv_der_len);
    s_needs_persist = 1;
    s_initialized = 1;
    return 0;
}

int sshkeys_persist(void) {
    if (!s_initialized || !s_needs_persist) return 0;

    if (sd_ensure_mounted() != 0) return -1;

    if (save_to_sd_file() == 0) {
        printf("sshkeys: host key saved to SD\n");
        s_needs_persist = 0;
        return 0;
    }

    printf("sshkeys: warning: could not save host key to SD\n");
    return -1;
}

const uint8_t *sshkeys_get_private(void)    { return s_priv_der; }
size_t         sshkeys_get_private_len(void) { return (size_t)s_priv_der_len; }
const uint8_t *sshkeys_get_public(void)     { return NULL; }
size_t         sshkeys_get_public_len(void)  { return 0; }

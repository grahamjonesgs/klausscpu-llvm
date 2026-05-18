/* sshkeys.c — SSH host key management. */

#define WOLFSSL_USER_SETTINGS
#include "../wolfssl/user_settings.h"

#include <stdio.h>
#include <string.h>

#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include "ff.h"          /* FatFs */
#include "sshkeys.h"
#include "../../mmio.h"  /* TRNG */

/* Ed25519: private key = 64 bytes (scalar ‖ public), public key = 32 bytes. */
#define PRIV_LEN 64
#define PUB_LEN  32

static uint8_t s_priv[PRIV_LEN];
static uint8_t s_pub[PUB_LEN];
static int     s_initialized;

static int generate_keypair(void) {
    ed25519_key key;
    WC_RNG      rng;

    int rc = wc_InitRng(&rng);
    if (rc) return rc;

    rc = wc_ed25519_init_ex(&key, NULL, KLAUSSCPU_CRYPTO_DEV_ID);
    if (rc) { wc_FreeRng(&rng); return rc; }

    rc = wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key);
    if (rc) { wc_ed25519_free(&key); wc_FreeRng(&rng); return rc; }

    /* Export raw private scalar (32 bytes) + public key (32 bytes) = 64 bytes. */
    word32 priv_sz = PRIV_LEN;
    rc = wc_ed25519_export_private(&key, s_priv, &priv_sz);
    if (rc) { wc_ed25519_free(&key); wc_FreeRng(&rng); return rc; }

    word32 pub_sz = PUB_LEN;
    rc = wc_ed25519_export_public(&key, s_pub, &pub_sz);

    wc_ed25519_free(&key);
    wc_FreeRng(&rng);
    return rc;
}

static int load_from_sd(void) {
    FIL f;
    FRESULT fr = f_open(&f, SSH_KEY_PATH, FA_READ);
    if (fr != FR_OK) return -1;

    UINT br;
    fr = f_read(&f, s_priv, PRIV_LEN, &br);
    f_close(&f);
    if (fr != FR_OK || br != PRIV_LEN) return -1;

    /* Re-derive public key from private. */
    ed25519_key key;
    int rc = wc_ed25519_init(&key);
    if (rc) return rc;
    word32 priv_sz = PRIV_LEN;
    rc = wc_ed25519_import_private_only(s_priv, priv_sz, &key);
    if (!rc) {
        word32 pub_sz = PUB_LEN;
        rc = wc_ed25519_export_public(&key, s_pub, &pub_sz);
    }
    wc_ed25519_free(&key);
    return rc;
}

static int save_to_sd_file(void) {
    FIL f;
    FRESULT fr = f_open(&f, SSH_KEY_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) return -1;
    UINT bw;
    fr = f_write(&f, s_priv, PRIV_LEN, &bw);
    f_close(&f);
    return (fr == FR_OK && bw == PRIV_LEN) ? 0 : -1;
}

int sshkeys_init(int save_to_sd) {
    if (s_initialized) return 0;

    /* Try to load existing key from SD card. */
    if (load_from_sd() == 0) {
        printf("ssh: loaded host key from SD\n");
        s_initialized = 1;
        return 0;
    }

    /* Generate a fresh key pair. */
    int rc = generate_keypair();
    if (rc) {
        printf("ssh: key generation failed (%d)\n", rc);
        return rc;
    }
    printf("ssh: generated new Ed25519 host key\n");

    if (save_to_sd) {
        if (save_to_sd_file() == 0)
            printf("ssh: host key saved to SD card\n");
        else
            printf("ssh: warning: could not save host key to SD\n");
    }

    s_initialized = 1;
    return 0;
}

const uint8_t *sshkeys_get_private(void)    { return s_priv; }
size_t         sshkeys_get_private_len(void) { return PRIV_LEN; }
const uint8_t *sshkeys_get_public(void)     { return s_pub; }
size_t         sshkeys_get_public_len(void)  { return PUB_LEN; }

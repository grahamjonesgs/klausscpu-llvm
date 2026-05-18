/* wolfssl_hw.c — wolfSSL hardware crypto callbacks for KlaussCPU.
 *
 * Implements AES-CTR and AES-GCM hardware acceleration via the KlaussCPU
 * MMIO crypto block.  SHA-256 / HMAC fall through to wolfCrypt software.
 *
 * Include order: FreeRTOS headers before wolfSSL to avoid macro conflicts.
 */

#define WOLFSSL_USER_SETTINGS
#include "wolfssl/user_settings.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* wolfCrypt headers. */
#include <wolfssl/wolfcrypt/cryptocb.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include "../crypto_hw.h"
#include "../mmio.h"
#include "wolfssl_hw.h"

/* ── Serialization mutex for the hardware AES/GCM block ─────────────────── */

static SemaphoreHandle_t s_aes_mtx;

static inline void aes_lock(void)   { xSemaphoreTake(s_aes_mtx, portMAX_DELAY); }
static inline void aes_unlock(void) { xSemaphoreGive(s_aes_mtx); }

/* ── TRNG seed function ───────────────────────────────────────────────────── */

/* wolfSSL calls this via the CUSTOM_RAND_GENERATE_SEED macro defined in
 * user_settings.h.  That macro prevents wolfSSL from compiling its own
 * wc_GenerateSeed (which tries /dev/urandom), so there is no duplicate symbol. */
int klausscpu_trng_seed(unsigned char *output, unsigned int sz) {
    if (!trng_health_ok()) return -1; /* RNG_FAILURE_E = -129 in wolfSSL */
    trng_read(output, (size_t)sz);
    return 0;
}

/* ── Hardware crypto callback ────────────────────────────────────────────── */

/* Extract the 128-bit AES key from a wolfCrypt Aes struct into two uint64_t.
 * wolfCrypt stores the key as little-endian 32-bit words in aes->key[]. */
static int extract_key128(const Aes *aes, uint64_t *lo, uint64_t *hi) {
    if (!aes || aes->keylen != 16) return -1;
    uint8_t k[16];
    for (int i = 0; i < 4; i++) {
        uint32_t w = aes->key[i];
        k[i*4+0] = (uint8_t)(w      );
        k[i*4+1] = (uint8_t)(w >>  8);
        k[i*4+2] = (uint8_t)(w >> 16);
        k[i*4+3] = (uint8_t)(w >> 24);
    }
    memcpy(lo, k,     8);
    memcpy(hi, k + 8, 8);
    return 0;
}

static int hw_crypto_cb(int devId, wc_CryptoInfo *info, void *ctx) {
    (void)devId; (void)ctx;

    if (info->algo_type != WC_ALGO_TYPE_CIPHER)
        return CRYPTOCB_UNAVAILABLE;

#ifdef WOLFSSL_AES_COUNTER
    if (info->cipher.type == WC_CIPHER_AES_CTR) {
        Aes           *aes = info->cipher.aesctr.aes;
        const uint8_t *in  = (const uint8_t *)info->cipher.aesctr.in;
        uint8_t       *out = info->cipher.aesctr.out;
        word32         sz  = info->cipher.aesctr.sz;

        uint64_t key_lo, key_hi;
        if (extract_key128(aes, &key_lo, &key_hi) < 0)
            return CRYPTOCB_UNAVAILABLE;

        /* wolfSSL stores the CTR block big-endian in aes->reg[]. */
        uint8_t ctr[16];
        for (int i = 0; i < 4; i++) {
            uint32_t w = aes->reg[i];
            ctr[i*4+0] = (uint8_t)(w >> 24);
            ctr[i*4+1] = (uint8_t)(w >> 16);
            ctr[i*4+2] = (uint8_t)(w >>  8);
            ctr[i*4+3] = (uint8_t)(w      );
        }
        uint64_t nonce_lo, nonce_hi;
        memcpy(&nonce_lo, ctr, 8); memcpy(&nonce_hi, ctr + 8, 8);

        aes_lock();
        aes_hw_set_key(key_lo, key_hi);
        aes_ctr(nonce_lo, nonce_hi, in, out, sz);
        aes_unlock();

        /* Advance aes->reg[] by the number of blocks consumed. */
        uint32_t blocks = (sz + 15) / 16;
        uint64_t clo, chi;
        memcpy(&clo, ctr, 8); memcpy(&chi, ctr + 8, 8);
        uint64_t old_chi = chi;
        chi += blocks;
        if (chi < old_chi) clo++;
        uint8_t nc[16];
        memcpy(nc, &clo, 8); memcpy(nc + 8, &chi, 8);
        for (int i = 0; i < 4; i++) {
            aes->reg[i] = ((uint32_t)nc[i*4]   << 24) |
                          ((uint32_t)nc[i*4+1]  << 16) |
                          ((uint32_t)nc[i*4+2]  <<  8) |
                           (uint32_t)nc[i*4+3];
        }
        return 0;
    }
#endif /* WOLFSSL_AES_COUNTER */

#ifdef HAVE_AESGCM
    if (info->cipher.type == WC_CIPHER_AES_GCM) {
        if (info->cipher.enc) {
            /* Encrypt */
            Aes           *aes      = info->cipher.aesgcm_enc.aes;
            const uint8_t *in       = (const uint8_t *)info->cipher.aesgcm_enc.in;
            uint8_t       *out      = info->cipher.aesgcm_enc.out;
            word32         sz       = info->cipher.aesgcm_enc.sz;
            const uint8_t *iv       = info->cipher.aesgcm_enc.iv;
            word32         iv_sz    = info->cipher.aesgcm_enc.ivSz;
            uint8_t       *authTag  = info->cipher.aesgcm_enc.authTag;
            word32         tagSz    = info->cipher.aesgcm_enc.authTagSz;
            const uint8_t *authIn   = (const uint8_t *)info->cipher.aesgcm_enc.authIn;
            word32         authInSz = info->cipher.aesgcm_enc.authInSz;

            if (iv_sz != 12 || tagSz != 16) return CRYPTOCB_UNAVAILABLE;
            uint64_t key_lo, key_hi;
            if (extract_key128(aes, &key_lo, &key_hi) < 0)
                return CRYPTOCB_UNAVAILABLE;

            aes_lock();
            aes_hw_set_key(key_lo, key_hi);
            aes_gcm_encrypt(iv, authIn, authInSz, in, out, sz, authTag);
            aes_unlock();
            return 0;
        } else {
            /* Decrypt */
            Aes           *aes      = info->cipher.aesgcm_dec.aes;
            const uint8_t *in       = (const uint8_t *)info->cipher.aesgcm_dec.in;
            uint8_t       *out      = info->cipher.aesgcm_dec.out;
            word32         sz       = info->cipher.aesgcm_dec.sz;
            const uint8_t *iv       = info->cipher.aesgcm_dec.iv;
            word32         iv_sz    = info->cipher.aesgcm_dec.ivSz;
            const uint8_t *authTag  = (const uint8_t *)info->cipher.aesgcm_dec.authTag;
            word32         tagSz    = info->cipher.aesgcm_dec.authTagSz;
            const uint8_t *authIn   = (const uint8_t *)info->cipher.aesgcm_dec.authIn;
            word32         authInSz = info->cipher.aesgcm_dec.authInSz;

            if (iv_sz != 12 || tagSz != 16) return CRYPTOCB_UNAVAILABLE;
            uint64_t key_lo, key_hi;
            if (extract_key128(aes, &key_lo, &key_hi) < 0)
                return CRYPTOCB_UNAVAILABLE;

            aes_lock();
            aes_hw_set_key(key_lo, key_hi);
            int rc = aes_gcm_decrypt(iv, authIn, authInSz, in, out, sz, authTag);
            aes_unlock();
            return (rc == 0) ? 0 : AES_GCM_AUTH_E;
        }
    }
#endif /* HAVE_AESGCM */

    if (info->algo_type == WC_ALGO_TYPE_SEED) {
        if (!trng_health_ok()) return RNG_FAILURE_E;
        trng_read(info->seed.seed, (size_t)info->seed.sz);
        return 0;
    }

    return CRYPTOCB_UNAVAILABLE;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int wolfssl_hw_init(void) {
    s_aes_mtx = xSemaphoreCreateMutex();
    if (!s_aes_mtx) return -1;

    /* Ensure TRNG is running. */
    trng_init();

    /* Register hardware device with wolfCrypt. */
    int rc = wc_CryptoCb_RegisterDevice(KLAUSSCPU_CRYPTO_DEV_ID,
                                        hw_crypto_cb, NULL);
    return rc;
}

void wolfssl_hw_cleanup(void) {
    wc_CryptoCb_UnRegisterDevice(KLAUSSCPU_CRYPTO_DEV_ID);
    if (s_aes_mtx) {
        vSemaphoreDelete(s_aes_mtx);
        s_aes_mtx = NULL;
    }
}

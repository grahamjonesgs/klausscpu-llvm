/* wolfssl_hw.c — wolfSSL hardware crypto callbacks for KlaussCPU (Zephyr).
 *
 * Adapted from freertos/wolfssl_hw.c.  Uses Zephyr k_mutex instead of
 * FreeRTOS semaphores.  Hardware AES/GCM/TRNG access is identical.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <string.h>

#include <wolfssl/wolfcrypt/cryptocb.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include "wolfssl_hw.h"

/* ── MMIO register definitions (crypto blocks) ──────────────────────────── */

#define MMIO_BASE    0xF0000000u
#define AES_BASE     (MMIO_BASE + 0x000A0000u)
#define TRNG_BASE    (MMIO_BASE + 0x000C0000u)

#define REG(a)       (*(volatile uint32_t *)(unsigned long)(a))
#define REG64(a)     (*(volatile uint64_t *)(unsigned long)(a))

#define REG_AES_CTRL     REG(AES_BASE + 0x000u)
#define REG_AES_STATUS   REG(AES_BASE + 0x008u)
#define REG_AES_KEY0     REG64(AES_BASE + 0x010u)
#define REG_AES_KEY1     REG64(AES_BASE + 0x018u)
#define REG_AES_IN0      REG64(AES_BASE + 0x040u)
#define REG_AES_IN1      REG64(AES_BASE + 0x048u)
#define REG_AES_OUT0     REG64(AES_BASE + 0x050u)
#define REG_AES_OUT1     REG64(AES_BASE + 0x058u)

#define AES_CTRL_GO        (1u << 0)
#define AES_CTRL_ENC       (1u << 1)
#define AES_CTRL_KEY_LOAD  (1u << 2)
#define AES_STATUS_BUSY    (1u << 0)

#define REG_GCM_CTRL     REG(AES_BASE + 0x080u)
#define REG_GCM_STATUS   REG(AES_BASE + 0x088u)
#define REG_GCM_H0       REG64(AES_BASE + 0x090u)
#define REG_GCM_H1       REG64(AES_BASE + 0x098u)
#define REG_GCM_X0       REG64(AES_BASE + 0x0A0u)
#define REG_GCM_X1       REG64(AES_BASE + 0x0A8u)
#define REG_GCM_TAG0     REG64(AES_BASE + 0x0B0u)
#define REG_GCM_TAG1     REG64(AES_BASE + 0x0B8u)

#define GCM_CTRL_GO      (1u << 0)
#define GCM_CTRL_RESET   (1u << 1)
#define GCM_STATUS_BUSY  (1u << 0)

#define REG_TRNG_CTRL    REG(TRNG_BASE + 0x000u)
#define REG_TRNG_STATUS  REG(TRNG_BASE + 0x008u)
#define REG_TRNG_DATA    REG64(TRNG_BASE + 0x010u)

#define TRNG_CTRL_ENABLE   (1u << 0)
#define TRNG_STATUS_READY      (1u << 0)
#define TRNG_STATUS_HEALTH_OK  (1u << 1)

/* ── Hardware helpers ────────────────────────────────────────────────────── */

static inline void aes_wait(void) { while (REG_AES_STATUS & AES_STATUS_BUSY) {} }
static inline void gcm_wait(void) { while (REG_GCM_STATUS & GCM_STATUS_BUSY) {} }

static void aes_hw_set_key(uint64_t lo, uint64_t hi)
{
	REG_AES_KEY0 = lo;
	REG_AES_KEY1 = hi;
	REG_AES_CTRL = AES_CTRL_KEY_LOAD;
	aes_wait();
}

static void aes_hw_encrypt(uint64_t in_lo, uint64_t in_hi,
			    uint64_t *out_lo, uint64_t *out_hi)
{
	REG_AES_IN0 = in_lo;
	REG_AES_IN1 = in_hi;
	REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
	aes_wait();
	*out_lo = REG_AES_OUT0;
	*out_hi = REG_AES_OUT1;
}

static void trng_init_hw(void)
{
	REG_TRNG_CTRL = TRNG_CTRL_ENABLE;
	while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
}

static int trng_health_ok(void)
{
	return (REG_TRNG_STATUS & TRNG_STATUS_HEALTH_OK) != 0;
}

static void trng_read(uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
		uint64_t w = REG_TRNG_DATA;
		size_t chunk = len - done;

		if (chunk > 8) {
			chunk = 8;
		}
		memcpy(buf + done, &w, chunk);
		done += chunk;
	}
}

/* ── AES-CTR (software loop + hardware ECB) ──────────────────────────────── */

static void aes_ctr(uint64_t nonce_lo, uint64_t nonce_hi,
		    const uint8_t *in, uint8_t *out, size_t len)
{
	size_t done = 0;

	while (done < len) {
		uint64_t ks_lo, ks_hi;

		aes_hw_encrypt(nonce_lo, nonce_hi, &ks_lo, &ks_hi);

		uint8_t ks[16];

		memcpy(ks, &ks_lo, 8);
		memcpy(ks + 8, &ks_hi, 8);

		size_t chunk = len - done;

		if (chunk > 16) {
			chunk = 16;
		}
		for (size_t i = 0; i < chunk; i++) {
			out[done + i] = in[done + i] ^ ks[i];
		}
		done += chunk;

		nonce_hi++;
		if (nonce_hi == 0) {
			nonce_lo++;
		}
	}
}

/* ── AES-GCM ─────────────────────────────────────────────────────────────── */

static void gcm_ghash_block(uint64_t lo, uint64_t hi)
{
	REG_GCM_X0 = lo;
	REG_GCM_X1 = hi;
	REG_GCM_CTRL = GCM_CTRL_GO;
	gcm_wait();
}

static void aes_gcm_encrypt(const uint8_t *iv,
			     const uint8_t *aad, size_t aad_len,
			     const uint8_t *pt, uint8_t *ct, size_t len,
			     uint8_t tag_out[16])
{
	uint64_t h_lo, h_hi;
	uint64_t zero = 0;

	aes_hw_encrypt(zero, zero, &h_lo, &h_hi);

	REG_GCM_H0 = h_lo;
	REG_GCM_H1 = h_hi;
	REG_GCM_CTRL = GCM_CTRL_RESET;
	gcm_wait();

	/* AAD */
	size_t i;

	for (i = 0; i + 16 <= aad_len; i += 16) {
		uint64_t lo, hi;

		memcpy(&lo, aad + i, 8);
		memcpy(&hi, aad + i + 8, 8);
		gcm_ghash_block(lo, hi);
	}
	if (i < aad_len) {
		uint8_t pad[16] = {0};

		memcpy(pad, aad + i, aad_len - i);
		uint64_t lo, hi;

		memcpy(&lo, pad, 8);
		memcpy(&hi, pad + 8, 8);
		gcm_ghash_block(lo, hi);
	}

	/* Build J0 from 12-byte IV */
	uint64_t j0_lo, j0_hi;
	uint8_t j0[16] = {0};

	memcpy(j0, iv, 12);
	j0[15] = 1;
	memcpy(&j0_lo, j0, 8);
	memcpy(&j0_hi, j0 + 8, 8);

	/* CTR encrypt starting from J0+1 */
	uint64_t ctr_hi = j0_hi + 1;
	uint64_t ctr_lo = j0_lo;

	aes_ctr(ctr_lo, ctr_hi, pt, ct, len);

	/* GHASH ciphertext */
	for (i = 0; i + 16 <= len; i += 16) {
		uint64_t lo, hi;

		memcpy(&lo, ct + i, 8);
		memcpy(&hi, ct + i + 8, 8);
		gcm_ghash_block(lo, hi);
	}
	if (i < len) {
		uint8_t pad[16] = {0};

		memcpy(pad, ct + i, len - i);
		uint64_t lo, hi;

		memcpy(&lo, pad, 8);
		memcpy(&hi, pad + 8, 8);
		gcm_ghash_block(lo, hi);
	}

	/* Length block: aad_len*8 || len*8 as big-endian 64-bit */
	uint64_t aad_bits = aad_len * 8;
	uint64_t ct_bits = len * 8;
	uint8_t lblock[16];

	for (int b = 7; b >= 0; b--) {
		lblock[b] = (uint8_t)(aad_bits & 0xFF);
		aad_bits >>= 8;
	}
	for (int b = 15; b >= 8; b--) {
		lblock[b] = (uint8_t)(ct_bits & 0xFF);
		ct_bits >>= 8;
	}
	uint64_t lb_lo, lb_hi;

	memcpy(&lb_lo, lblock, 8);
	memcpy(&lb_hi, lblock + 8, 8);
	gcm_ghash_block(lb_lo, lb_hi);

	/* Tag = GHASH_tag XOR E(K, J0) */
	uint64_t tag_lo = REG_GCM_TAG0;
	uint64_t tag_hi = REG_GCM_TAG1;
	uint64_t ej0_lo, ej0_hi;

	aes_hw_encrypt(j0_lo, j0_hi, &ej0_lo, &ej0_hi);
	tag_lo ^= ej0_lo;
	tag_hi ^= ej0_hi;
	memcpy(tag_out, &tag_lo, 8);
	memcpy(tag_out + 8, &tag_hi, 8);
}

static int aes_gcm_decrypt(const uint8_t *iv,
			    const uint8_t *aad, size_t aad_len,
			    const uint8_t *ct_in, uint8_t *pt, size_t len,
			    const uint8_t tag_in[16])
{
	uint8_t computed_tag[16];

	aes_gcm_encrypt(iv, aad, aad_len, ct_in, pt, len, computed_tag);

	uint8_t diff = 0;

	for (int i = 0; i < 16; i++) {
		diff |= computed_tag[i] ^ tag_in[i];
	}
	return (diff == 0) ? 0 : -1;
}

/* ── Mutex for hardware AES block ────────────────────────────────────────── */

static struct k_mutex aes_mtx;

static inline void aes_lock(void)   { k_mutex_lock(&aes_mtx, K_FOREVER); }
static inline void aes_unlock(void) { k_mutex_unlock(&aes_mtx); }

/* ── TRNG seed function ──────────────────────────────────────────────────── */

int klausscpu_trng_seed(unsigned char *output, unsigned int sz)
{
	if (!trng_health_ok()) {
		return -1;
	}
	trng_read(output, (size_t)sz);
	return 0;
}

/* ── Extract 128-bit AES key from wolfCrypt Aes struct ───────────────────── */

static int extract_key128(const Aes *aes, uint64_t *lo, uint64_t *hi)
{
	if (!aes || aes->keylen != 16) {
		return -1;
	}
	uint8_t k[16];

	for (int i = 0; i < 4; i++) {
		uint32_t w = aes->key[i];

		k[i * 4 + 0] = (uint8_t)(w);
		k[i * 4 + 1] = (uint8_t)(w >> 8);
		k[i * 4 + 2] = (uint8_t)(w >> 16);
		k[i * 4 + 3] = (uint8_t)(w >> 24);
	}
	memcpy(lo, k, 8);
	memcpy(hi, k + 8, 8);
	return 0;
}

/* ── wolfCrypt crypto callback ───────────────────────────────────────────── */

static int hw_crypto_cb(int devId, wc_CryptoInfo *info, void *ctx)
{
	(void)devId;
	(void)ctx;

	if (info->algo_type != WC_ALGO_TYPE_CIPHER) {
		return CRYPTOCB_UNAVAILABLE;
	}

#ifdef WOLFSSL_AES_COUNTER
	if (info->cipher.type == WC_CIPHER_AES_CTR) {
		Aes *aes = info->cipher.aesctr.aes;
		const uint8_t *in = (const uint8_t *)info->cipher.aesctr.in;
		uint8_t *out_buf = info->cipher.aesctr.out;
		word32 sz = info->cipher.aesctr.sz;
		uint64_t key_lo, key_hi;

		if (extract_key128(aes, &key_lo, &key_hi) < 0) {
			return CRYPTOCB_UNAVAILABLE;
		}

		uint8_t ctr[16];

		for (int i = 0; i < 4; i++) {
			uint32_t w = aes->reg[i];

			ctr[i * 4 + 0] = (uint8_t)(w >> 24);
			ctr[i * 4 + 1] = (uint8_t)(w >> 16);
			ctr[i * 4 + 2] = (uint8_t)(w >> 8);
			ctr[i * 4 + 3] = (uint8_t)(w);
		}
		uint64_t nonce_lo, nonce_hi;

		memcpy(&nonce_lo, ctr, 8);
		memcpy(&nonce_hi, ctr + 8, 8);

		aes_lock();
		aes_hw_set_key(key_lo, key_hi);
		aes_ctr(nonce_lo, nonce_hi, in, out_buf, sz);
		aes_unlock();

		uint32_t blocks = (sz + 15) / 16;
		uint64_t clo, chi;

		memcpy(&clo, ctr, 8);
		memcpy(&chi, ctr + 8, 8);
		uint64_t old_chi = chi;

		chi += blocks;
		if (chi < old_chi) {
			clo++;
		}
		uint8_t nc[16];

		memcpy(nc, &clo, 8);
		memcpy(nc + 8, &chi, 8);
		for (int i = 0; i < 4; i++) {
			aes->reg[i] = ((uint32_t)nc[i * 4] << 24) |
				       ((uint32_t)nc[i * 4 + 1] << 16) |
				       ((uint32_t)nc[i * 4 + 2] << 8) |
				       (uint32_t)nc[i * 4 + 3];
		}
		return 0;
	}
#endif

#ifdef HAVE_AESGCM
	if (info->cipher.type == WC_CIPHER_AES_GCM) {
		if (info->cipher.enc) {
			Aes *aes = info->cipher.aesgcm_enc.aes;
			const uint8_t *in = (const uint8_t *)info->cipher.aesgcm_enc.in;
			uint8_t *out_buf = info->cipher.aesgcm_enc.out;
			word32 sz = info->cipher.aesgcm_enc.sz;
			const uint8_t *iv = info->cipher.aesgcm_enc.iv;
			word32 iv_sz = info->cipher.aesgcm_enc.ivSz;
			uint8_t *authTag = info->cipher.aesgcm_enc.authTag;
			word32 tagSz = info->cipher.aesgcm_enc.authTagSz;
			const uint8_t *authIn =
				(const uint8_t *)info->cipher.aesgcm_enc.authIn;
			word32 authInSz = info->cipher.aesgcm_enc.authInSz;

			if (iv_sz != 12 || tagSz != 16) {
				return CRYPTOCB_UNAVAILABLE;
			}
			uint64_t key_lo, key_hi;

			if (extract_key128(aes, &key_lo, &key_hi) < 0) {
				return CRYPTOCB_UNAVAILABLE;
			}

			aes_lock();
			aes_hw_set_key(key_lo, key_hi);
			aes_gcm_encrypt(iv, authIn, authInSz, in, out_buf, sz,
					authTag);
			aes_unlock();
			return 0;
		} else {
			Aes *aes = info->cipher.aesgcm_dec.aes;
			const uint8_t *in = (const uint8_t *)info->cipher.aesgcm_dec.in;
			uint8_t *out_buf = info->cipher.aesgcm_dec.out;
			word32 sz = info->cipher.aesgcm_dec.sz;
			const uint8_t *iv = info->cipher.aesgcm_dec.iv;
			word32 iv_sz = info->cipher.aesgcm_dec.ivSz;
			const uint8_t *authTag =
				(const uint8_t *)info->cipher.aesgcm_dec.authTag;
			word32 tagSz = info->cipher.aesgcm_dec.authTagSz;
			const uint8_t *authIn =
				(const uint8_t *)info->cipher.aesgcm_dec.authIn;
			word32 authInSz = info->cipher.aesgcm_dec.authInSz;

			if (iv_sz != 12 || tagSz != 16) {
				return CRYPTOCB_UNAVAILABLE;
			}
			uint64_t key_lo, key_hi;

			if (extract_key128(aes, &key_lo, &key_hi) < 0) {
				return CRYPTOCB_UNAVAILABLE;
			}

			aes_lock();
			aes_hw_set_key(key_lo, key_hi);
			int rc = aes_gcm_decrypt(iv, authIn, authInSz, in,
						 out_buf, sz, authTag);
			aes_unlock();
			return (rc == 0) ? 0 : AES_GCM_AUTH_E;
		}
	}
#endif

	if (info->algo_type == WC_ALGO_TYPE_SEED) {
		if (!trng_health_ok()) {
			return RNG_FAILURE_E;
		}
		trng_read(info->seed.seed, (size_t)info->seed.sz);
		return 0;
	}

	return CRYPTOCB_UNAVAILABLE;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int wolfssl_hw_init(void)
{
	k_mutex_init(&aes_mtx);
	trng_init_hw();
	return wc_CryptoCb_RegisterDevice(KLAUSSCPU_CRYPTO_DEV_ID,
					   hw_crypto_cb, NULL);
}

void wolfssl_hw_cleanup(void)
{
	wc_CryptoCb_UnRegisterDevice(KLAUSSCPU_CRYPTO_DEV_ID);
}

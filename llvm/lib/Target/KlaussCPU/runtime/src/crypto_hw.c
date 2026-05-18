/* crypto_hw.c — KlaussCPU hardware crypto MMIO drivers.
 *
 * Implements the API declared in crypto_hw.h.  All functions poll BUSY
 * (no IRQ-driven operation).  Callers must serialize concurrent access
 * with a mutex in multi-threaded environments.
 */

#include "../crypto_hw.h"
#include "../mmio.h"
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static inline void aes_wait(void) {
    while (REG_AES_STATUS & AES_STATUS_BUSY) {}
}

static inline void sha_wait(void) {
    while (!(REG_SHA_STATUS & SHA_STATUS_DONE)) {}
}

static inline void hmac_wait(void) {
    while (REG_HMAC_STATUS & HMAC_STATUS_BUSY) {}
}

/* Write 64 bytes (8 × uint64_t) to SHA_BLOCK0..7. */
static void sha_write_block(const uint8_t *data) {
    volatile uint64_t *blk = (volatile uint64_t *)(SHA_BASE + 0x010u);
    for (int i = 0; i < 8; i++) {
        uint64_t w;
        memcpy(&w, data + i * 8, 8);
        blk[i] = w;
    }
}

/* Read 32-byte SHA digest from SHA_DIGEST0..3 into a byte array. */
static void sha_read_digest(uint8_t out[32]) {
    volatile uint64_t *dig = (volatile uint64_t *)(SHA_BASE + 0x050u);
    for (int i = 0; i < 4; i++) {
        uint64_t w = dig[i];
        memcpy(out + i * 8, &w, 8);
    }
}

/* ── Internal SHA-256 block processing (used by sha256_hw_*) ─────────────── */

/* Process any number of complete 64-byte blocks already staged in hardware
 * (assumes SHA hardware is already INIT'd). */
static void sha_process_full_blocks(const uint8_t *data, size_t n_blocks) {
    for (size_t i = 0; i < n_blocks; i++) {
        sha_write_block(data + i * 64);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
    }
}

/* Finalize a SHA-256 computation.
 * partial: the remaining bytes (< 64) of the message.
 * partial_len: number of bytes in partial.
 * total_bits: total message length in bits.
 * out: 32-byte digest output.
 *
 * SHA-256 padding: append 0x80, zeros, then 64-bit BE bit count in last 8 bytes.
 * If partial_len >= 56 we need two padding blocks (the 0x80 and zeros don't
 * leave room for the 8-byte length field in the same block). */
static void sha_finalize(const uint8_t *partial, size_t partial_len,
                         uint64_t total_bits, uint8_t out[32]) {
    uint8_t pad[128];
    memset(pad, 0, sizeof(pad));
    if (partial_len) memcpy(pad, partial, partial_len);
    pad[partial_len] = 0x80;

    /* Write 64-bit BE bit length into last 8 bytes of the last 64-byte pad block. */
    size_t padblocks = (partial_len < 56) ? 1 : 2;
    size_t lpos = padblocks * 64 - 8;
    pad[lpos]   = (uint8_t)(total_bits >> 56);
    pad[lpos+1] = (uint8_t)(total_bits >> 48);
    pad[lpos+2] = (uint8_t)(total_bits >> 40);
    pad[lpos+3] = (uint8_t)(total_bits >> 32);
    pad[lpos+4] = (uint8_t)(total_bits >> 24);
    pad[lpos+5] = (uint8_t)(total_bits >> 16);
    pad[lpos+6] = (uint8_t)(total_bits >>  8);
    pad[lpos+7] = (uint8_t)(total_bits      );

    if (padblocks == 2) {
        sha_write_block(pad);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
    }
    /* Final block — software has already written the length into pad[], so
     * SHA_CTRL_START alone completes the hash. No LAST bit or BITLEN register. */
    sha_write_block(pad + (padblocks - 1) * 64);
    REG_SHA_CTRL = SHA_CTRL_START;
    sha_wait();

    sha_read_digest(out);
}

/* ── AES-128 ECB ─────────────────────────────────────────────────────────── */

void aes_hw_set_key(uint64_t key_lo, uint64_t key_hi) {
    REG_AES_KEY0 = key_lo;
    REG_AES_KEY1 = key_hi;
    REG_AES_CTRL = AES_CTRL_KEY_LOAD;
    aes_wait();
}

void aes_hw_encrypt(uint64_t in_lo, uint64_t in_hi,
                    uint64_t *out_lo, uint64_t *out_hi) {
    REG_AES_IN0 = in_lo;
    REG_AES_IN1 = in_hi;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait();
    *out_lo = REG_AES_OUT0;
    *out_hi = REG_AES_OUT1;
}

void aes_hw_decrypt(uint64_t in_lo, uint64_t in_hi,
                    uint64_t *out_lo, uint64_t *out_hi) {
    REG_AES_IN0 = in_lo;
    REG_AES_IN1 = in_hi;
    REG_AES_CTRL = AES_CTRL_GO; /* ENC=0 → decrypt */
    aes_wait();
    *out_lo = REG_AES_OUT0;
    *out_hi = REG_AES_OUT1;
}

/* ── AES-128 CTR ─────────────────────────────────────────────────────────── */

void aes_ctr(uint64_t nonce_lo, uint64_t nonce_hi,
             const uint8_t *in, uint8_t *out, size_t len) {
    uint64_t ctr_lo = nonce_lo, ctr_hi = nonce_hi;

    /* Full 16-byte blocks. */
    while (len >= 16) {
        uint64_t ks_lo, ks_hi;
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);

        uint64_t a, b;
        memcpy(&a, in,      8);
        memcpy(&b, in + 8,  8);
        a ^= ks_lo;
        b ^= ks_hi;
        memcpy(out,     &a, 8);
        memcpy(out + 8, &b, 8);

        /* 128-bit counter increment. */
        ctr_lo++;
        if (ctr_lo == 0) ctr_hi++;
        in  += 16; out += 16; len -= 16;
    }

    /* Tail: generate one keystream block, XOR remaining bytes. */
    if (len > 0) {
        uint64_t ks_lo, ks_hi;
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);
        uint8_t ks_bytes[16];
        memcpy(ks_bytes,     &ks_lo, 8);
        memcpy(ks_bytes + 8, &ks_hi, 8);
        for (size_t i = 0; i < len; i++)
            out[i] = in[i] ^ ks_bytes[i];
    }
}

void aes_ctr_full(uint64_t key_lo, uint64_t key_hi,
                  uint64_t nonce_lo, uint64_t nonce_hi,
                  const uint8_t *in, uint8_t *out, size_t len) {
    aes_hw_set_key(key_lo, key_hi);
    aes_ctr(nonce_lo, nonce_hi, in, out, len);
}

/* ── AES-GCM (raw GHASH interface per MMIO_MAP.md) ───────────────────────── */

static uint64_t bswap64(uint64_t v) {
    return ((v & 0xFF00000000000000ULL) >> 56) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x000000FF00000000ULL) >>  8) |
           ((v & 0x00000000FF000000ULL) <<  8) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x00000000000000FFULL) << 56);
}

/* Increment a 128-bit counter block; last 4 bytes = 32-bit big-endian counter. */
static void ctr_inc(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--)
        if (++ctr[i]) break;
}

/* GHASH one (zero-padded) block of up to 16 bytes. */
static void ghash_block(const uint8_t *data, size_t len) {
    uint8_t blk[16] = {0};
    if (len) memcpy(blk, data, len > 16 ? 16 : len);
    uint64_t lo, hi;
    memcpy(&lo, blk,     8);
    memcpy(&hi, blk + 8, 8);
    gcm_ghash_block(lo, hi);
}

void aes_gcm_encrypt(const uint8_t *iv,
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, uint8_t *ct, size_t len,
                     uint8_t tag_out[16]) {
    /* 1. Compute H = AES_K(0^128) and load into GHASH unit. */
    REG_AES_IN0 = 0; REG_AES_IN1 = 0;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait();
    REG_GCM_H0 = REG_AES_OUT0;
    REG_GCM_H1 = REG_AES_OUT1;

    /* 2. Build J0 = IV ‖ 0x00000001; encrypt J0 for tag mask. */
    uint8_t j0[16] = {0};
    memcpy(j0, iv, 12); j0[15] = 0x01;
    uint64_t j0_lo, j0_hi;
    memcpy(&j0_lo, j0, 8); memcpy(&j0_hi, j0 + 8, 8);
    aes_hw_encrypt(j0_lo, j0_hi, &j0_lo, &j0_hi); /* j0_lo/hi now = keystream mask */

    /* 3. Reset GHASH accumulator. */
    REG_GCM_CTRL = GCM_CTRL_RESET;

    /* 4. GHASH over AAD (zero-padded to block boundary). */
    size_t off = 0;
    while (off < aad_len) {
        size_t n = (aad_len - off >= 16) ? 16 : (aad_len - off);
        ghash_block(aad + off, n);
        off += n;
    }

    /* 5. CTR encryption + GHASH of ciphertext. Counter starts at J0+1. */
    uint8_t ctr[16] = {0};
    memcpy(ctr, iv, 12); ctr[15] = 0x01;
    off = 0;
    while (off < len) {
        ctr_inc(ctr);
        uint64_t ctr_lo, ctr_hi, ks_lo, ks_hi;
        memcpy(&ctr_lo, ctr, 8); memcpy(&ctr_hi, ctr + 8, 8);
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);

        size_t n = (len - off >= 16) ? 16 : (len - off);
        uint8_t blk[16] = {0};
        memcpy(blk, pt + off, n);
        uint64_t p0, p1;
        memcpy(&p0, blk, 8); memcpy(&p1, blk + 8, 8);
        uint64_t c0 = p0 ^ ks_lo, c1 = p1 ^ ks_hi;
        uint8_t cblk[16];
        memcpy(cblk, &c0, 8); memcpy(cblk + 8, &c1, 8);
        memcpy(ct + off, cblk, n);
        ghash_block(cblk, n); /* GHASH over ciphertext bytes */
        off += n;
    }

    /* 6. GHASH length block: (lenA_bits || lenC_bits), each 64-bit BE.
     * Per MMIO_MAP: pass bswap64(lenC_bits) as lo, bswap64(lenA_bits) as hi. */
    uint64_t len_a_bits = (uint64_t)aad_len * 8;
    uint64_t len_c_bits = (uint64_t)len     * 8;
    gcm_ghash_block(bswap64(len_c_bits), bswap64(len_a_bits));

    /* 7. tag = GHASH ^ AES_K(J0). */
    uint64_t t0 = REG_GCM_TAG0 ^ j0_lo;
    uint64_t t1 = REG_GCM_TAG1 ^ j0_hi;
    memcpy(tag_out,     &t0, 8);
    memcpy(tag_out + 8, &t1, 8);
}

int aes_gcm_decrypt(const uint8_t *iv,
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, uint8_t *pt, size_t len,
                    const uint8_t tag_in[16]) {
    uint8_t computed_tag[16];
    /* GCM decrypt: GHASH over CT, then CTR-decrypt CT→PT. */
    /* 1. H and J0. */
    REG_AES_IN0 = 0; REG_AES_IN1 = 0;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait();
    REG_GCM_H0 = REG_AES_OUT0;
    REG_GCM_H1 = REG_AES_OUT1;

    uint8_t j0[16] = {0};
    memcpy(j0, iv, 12); j0[15] = 0x01;
    uint64_t j0_lo, j0_hi;
    memcpy(&j0_lo, j0, 8); memcpy(&j0_hi, j0 + 8, 8);
    aes_hw_encrypt(j0_lo, j0_hi, &j0_lo, &j0_hi);

    REG_GCM_CTRL = GCM_CTRL_RESET;

    /* 2. GHASH AAD. */
    size_t off = 0;
    while (off < aad_len) {
        size_t n = (aad_len - off >= 16) ? 16 : (aad_len - off);
        ghash_block(aad + off, n);
        off += n;
    }

    /* 3. GHASH CT. */
    off = 0;
    while (off < len) {
        size_t n = (len - off >= 16) ? 16 : (len - off);
        ghash_block(ct + off, n);
        off += n;
    }

    /* 4. Length block. */
    gcm_ghash_block(bswap64((uint64_t)len * 8), bswap64((uint64_t)aad_len * 8));

    /* 5. Tag. */
    uint64_t t0 = REG_GCM_TAG0 ^ j0_lo;
    uint64_t t1 = REG_GCM_TAG1 ^ j0_hi;
    memcpy(computed_tag,     &t0, 8);
    memcpy(computed_tag + 8, &t1, 8);

    /* Constant-time compare. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= computed_tag[i] ^ tag_in[i];
    if (diff) { memset(pt, 0, len); return -1; }

    /* 6. CTR decrypt. */
    uint8_t ctr[16] = {0};
    memcpy(ctr, iv, 12); ctr[15] = 0x01;
    off = 0;
    while (off < len) {
        ctr_inc(ctr);
        uint64_t ctr_lo, ctr_hi, ks_lo, ks_hi;
        memcpy(&ctr_lo, ctr, 8); memcpy(&ctr_hi, ctr + 8, 8);
        aes_hw_encrypt(ctr_lo, ctr_hi, &ks_lo, &ks_hi);
        size_t n = (len - off >= 16) ? 16 : (len - off);
        uint8_t blk[16] = {0};
        memcpy(blk, ct + off, n);
        uint64_t c0, c1;
        memcpy(&c0, blk, 8); memcpy(&c1, blk + 8, 8);
        c0 ^= ks_lo; c1 ^= ks_hi;
        memcpy(blk, &c0, 8); memcpy(blk + 8, &c1, 8);
        memcpy(pt + off, blk, n);
        off += n;
    }
    return 0;
}

/* ── SHA-256 (one-shot) ──────────────────────────────────────────────────── */

void sha256_hw(const uint8_t *msg, size_t len, uint8_t out[32]) {
    REG_SHA_CTRL = SHA_CTRL_INIT;
    sha_wait();

    /* Full blocks. */
    size_t off = 0;
    while (off + 64 <= len) {
        sha_write_block(msg + off);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
        off += 64;
    }

    sha_finalize(msg + off, len - off, (uint64_t)len * 8, out);
}

/* ── SHA-256 (streaming) ─────────────────────────────────────────────────── */

void sha256_hw_init(sha256_ctx_t *ctx) {
    ctx->total_bytes = 0;
    ctx->buf_len     = 0;
    ctx->initialized = 1;
    REG_SHA_CTRL = SHA_CTRL_INIT;
    sha_wait();
}

void sha256_hw_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    ctx->total_bytes += len;

    /* If there are bytes in the partial block buffer, try to fill it first. */
    if (ctx->buf_len > 0) {
        size_t need = 64 - ctx->buf_len;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += (uint32_t)take;
        data += take; len -= take;
        if (ctx->buf_len == 64) {
            sha_write_block(ctx->buf);
            REG_SHA_CTRL = SHA_CTRL_START;
            sha_wait();
            ctx->buf_len = 0;
        }
    }

    /* Process complete blocks directly from input. */
    while (len >= 64) {
        sha_write_block(data);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
        data += 64; len -= 64;
    }

    /* Buffer any remaining bytes. */
    if (len > 0) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = (uint32_t)len;
    }
}

void sha256_hw_final(sha256_ctx_t *ctx, uint8_t out[32]) {
    sha_finalize(ctx->buf, ctx->buf_len,
                 ctx->total_bytes * 8, out);
    ctx->initialized = 0;
}

/* ── HMAC-SHA-256 (hardware HMAC wrapper) ────────────────────────────────── */

void hmac_sha256_hw(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]) {
    /* Key must be ≤ 32 bytes. Longer keys should be pre-hashed by caller. */
    uint8_t key_buf[32];
    memset(key_buf, 0, 32);
    if (key_len > 32) key_len = 32;
    memcpy(key_buf, key, key_len);

    /* Load key into HMAC_KEY0..3 (256-bit = 4 × 64-bit). */
    volatile uint64_t *hkey = (volatile uint64_t *)(SHA_BASE + 0x090u);
    for (int i = 0; i < 4; i++) {
        uint64_t w;
        memcpy(&w, key_buf + i * 8, 8);
        hkey[i] = w;
    }
    REG_HMAC_CTRL = HMAC_CTRL_KEY_LOAD;
    hmac_wait();

    /* Start HMAC operation. */
    REG_HMAC_CTRL = HMAC_CTRL_START;
    hmac_wait();

    /* Stream message through SHA_BLOCK registers. */
    size_t off = 0;
    while (off + 64 <= msg_len) {
        sha_write_block(msg + off);
        REG_SHA_CTRL = SHA_CTRL_START;
        sha_wait();
        off += 64;
    }

    /* Final block with HMAC finalization.
     * Inner message is ipad (64 bytes, handled by hardware) + msg, so the
     * SHA-256 length field encodes (512 + msg_len*8) bits, big-endian. */
    uint64_t inner_bits = 512ULL + (uint64_t)msg_len * 8ULL;
    uint8_t fin[64];
    memset(fin, 0, 64);
    memcpy(fin, msg + off, msg_len - off);
    fin[msg_len - off] = 0x80;
    for (int i = 0; i < 8; i++)
        fin[63 - i] = (uint8_t)(inner_bits >> (i * 8));
    sha_write_block(fin);
    REG_HMAC_CTRL = HMAC_CTRL_FINAL;
    hmac_wait();

    sha_read_digest(out);
}

/* ── HMAC-SHA-256 (software HMAC using hardware SHA-256) ────────────────── */

void hmac_sha256_sw(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];

    /* If key > 64 bytes, hash it. */
    memset(k, 0, 64);
    if (key_len > 64) {
        sha256_hw(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36u;
        opad[i] = k[i] ^ 0x5Cu;
    }

    /* Inner hash: SHA256(ipad ‖ msg). */
    sha256_ctx_t ctx;
    sha256_hw_init(&ctx);
    sha256_hw_update(&ctx, ipad, 64);
    sha256_hw_update(&ctx, msg, msg_len);
    sha256_hw_final(&ctx, inner);

    /* Outer hash: SHA256(opad ‖ inner). */
    sha256_hw_init(&ctx);
    sha256_hw_update(&ctx, opad, 64);
    sha256_hw_update(&ctx, inner, 32);
    sha256_hw_final(&ctx, out);
}

/* ── TRNG ────────────────────────────────────────────────────────────────── */

void trng_init(void) {
    REG_TRNG_CTRL = TRNG_CTRL_ENABLE;

    /* Drain any stale FIFO entries from before init. */
    uint32_t drain = 32;
    while ((REG_TRNG_STATUS & TRNG_STATUS_READY) && drain--)
        (void)REG_TRNG_DATA;

    /* Reseed: self-clearing bit; ring oscillators refill the FIFO with
     * fresh entropy. */
    REG_TRNG_CTRL = TRNG_CTRL_ENABLE | TRNG_CTRL_RESEED;

    /* Wait for the first fresh conditioned word to appear. */
    while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
}

int trng_ready(void) {
    return (REG_TRNG_STATUS & TRNG_STATUS_READY) != 0;
}

int trng_health_ok(void) {
    return (REG_TRNG_STATUS & TRNG_STATUS_HEALTH_OK) != 0;
}

void trng_read(uint8_t *buf, size_t len) {
    while (len >= 8) {
        while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
        uint64_t w = REG_TRNG_DATA;
        memcpy(buf, &w, 8);
        buf += 8; len -= 8;
    }
    if (len > 0) {
        while (!(REG_TRNG_STATUS & TRNG_STATUS_READY)) {}
        uint64_t w = REG_TRNG_DATA;
        memcpy(buf, &w, len);
    }
}

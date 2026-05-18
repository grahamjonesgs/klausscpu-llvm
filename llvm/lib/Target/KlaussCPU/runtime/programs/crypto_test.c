/* crypto_test.c — on-FPGA self-test for the AES/SHA-256/HMAC/GHASH/TRNG
 * crypto blocks (CRYPTO_PLAN.md items 1–5).
 *
 * Style follows eth_test.c — uses REG_SEG_ALL on the 7-segment display for
 * pass/fail reporting.  Each test stage paints two codes:
 *
 *   PRE  = 0x000N_0000          before the test runs (N = test number)
 *   POST = 0x000N_0000          after PASS
 *        | 0x000N_EEEE          after FAIL
 *
 * After all tests:
 *   0x00990000u  = ALL PASS
 *   0x00FFEEEEu  = ANY FAIL
 *
 * Stages:
 *   1. AES-128 encrypt + round-trip decrypt   (FIPS-197 §B vector)
 *   2. SHA-256 of "abc"                       (FIPS-180-4 §B.1)
 *   3. HMAC-SHA-256 of "Hi There" key=0x0b×20 (RFC 4231 TC1)
 *   4. GHASH NIST GCM TC2                     (chained 2-step multiply)
 *   5. TRNG: enable, read 4 words, check HEALTH_OK and non-zero
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../mmio.h"   /* REG_AES_*, REG_SHA_*, REG_GCM_*, REG_HMAC_*, REG_TRNG_*,
                       aes_wait_done, sha_wait_done, hmac_wait_keyload,
                       gcm_wait_done, trng_read64, SHA_BLOCK_PTR, SHA_DIGEST_PTR */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline void show(uint32_t code) { REG_SEG_ALL = code; }

/* Compare two 16-byte / 32-byte buffers; return 0 on match, else 1. */
static int bcmp16(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 1;
    return 0;
}
static int bcmp32(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 32; i++) if (a[i] != b[i]) return 1;
    return 0;
}

static void print_hex(const char *label, const uint8_t *buf, int len) {
    printf("  %s: ", label);
    for (int i = 0; i < len; i++) {
        if (i > 0 && (i & 3) == 0) printf(" ");
        printf("%02X", (unsigned)buf[i]);
    }
    printf("\n");
}

/* ── AES helpers ─────────────────────────────────────────────────────────── */

static void aes_set_key(const uint8_t key[16]) {
    uint64_t lo, hi;
    memcpy(&lo, key,     8);
    memcpy(&hi, key + 8, 8);
    REG_AES_KEY0 = lo;
    REG_AES_KEY1 = hi;
    REG_AES_CTRL = AES_CTRL_KEY_LOAD;
    aes_wait_done();
}

static void aes_encrypt_block(const uint8_t pt[16], uint8_t ct[16]) {
    uint64_t lo, hi;
    memcpy(&lo, pt,     8);
    memcpy(&hi, pt + 8, 8);
    REG_AES_IN0 = lo;
    REG_AES_IN1 = hi;
    REG_AES_CTRL = AES_CTRL_GO | AES_CTRL_ENC;
    aes_wait_done();
    lo = REG_AES_OUT0; hi = REG_AES_OUT1;
    memcpy(ct,     &lo, 8);
    memcpy(ct + 8, &hi, 8);
}

static void aes_decrypt_block(const uint8_t ct[16], uint8_t pt[16]) {
    uint64_t lo, hi;
    memcpy(&lo, ct,     8);
    memcpy(&hi, ct + 8, 8);
    REG_AES_IN0 = lo;
    REG_AES_IN1 = hi;
    REG_AES_CTRL = AES_CTRL_GO;   /* ENC bit = 0 → decrypt */
    aes_wait_done();
    lo = REG_AES_OUT0; hi = REG_AES_OUT1;
    memcpy(pt,     &lo, 8);
    memcpy(pt + 8, &hi, 8);
}

/* ── SHA-256 helper (single ≤55-byte message) ────────────────────────────── */

static void sha256_one_block(const uint8_t *msg, uint32_t len, uint8_t out[32]) {
    uint8_t block[64];
    memset(block, 0, 64);
    memcpy(block, msg, len);
    block[len] = 0x80;
    /* 64-bit big-endian bit length in bytes 56..63 */
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        block[63 - i] = (uint8_t)(bitlen >> (8 * i));

    REG_SHA_CTRL = SHA_CTRL_INIT;
    for (int i = 0; i < 8; i++) {
        uint64_t w;
        memcpy(&w, block + i * 8, 8);
        SHA_BLOCK_PTR[i] = w;
    }
    REG_SHA_CTRL = SHA_CTRL_START;
    sha_wait_done();
    for (int i = 0; i < 4; i++) {
        uint64_t w = SHA_DIGEST_PTR[i];
        memcpy(out + i * 8, &w, 8);
    }
}

/* ── HMAC-SHA-256 helper (key ≤32 bytes, message ≤55 bytes) ─────────────── */

static void hmac_sha256_hw(const uint8_t *key, uint32_t klen,
                           const uint8_t *msg,  uint32_t mlen,
                           uint8_t tag[32]) {
    uint8_t k32[32];
    memset(k32, 0, 32);
    if (klen > 32) klen = 32;
    memcpy(k32, key, klen);

    uint64_t w;
    memcpy(&w, k32,      8); REG_HMAC_KEY0 = w;
    memcpy(&w, k32 +  8, 8); REG_HMAC_KEY1 = w;
    memcpy(&w, k32 + 16, 8); REG_HMAC_KEY2 = w;
    memcpy(&w, k32 + 24, 8); REG_HMAC_KEY3 = w;

    REG_HMAC_CTRL = HMAC_CTRL_KEY_LOAD;
    hmac_wait_keyload();

    /* Inner pass */
    REG_HMAC_CTRL = HMAC_CTRL_START;

    uint8_t blk[64];
    memset(blk, 0, 64);
    memcpy(blk, msg, mlen);
    blk[mlen] = 0x80;
    uint64_t inner_bits = 512ULL + (uint64_t)mlen * 8ULL;
    for (int i = 0; i < 8; i++)
        blk[63 - i] = (uint8_t)(inner_bits >> (8 * i));
    for (int i = 0; i < 8; i++) {
        memcpy(&w, blk + i * 8, 8);
        SHA_BLOCK_PTR[i] = w;
    }
    REG_SHA_CTRL = SHA_CTRL_START;
    sha_wait_done();

    uint8_t inner_digest[32];
    for (int i = 0; i < 4; i++) {
        w = SHA_DIGEST_PTR[i];
        memcpy(inner_digest + i * 8, &w, 8);
    }

    /* Outer pass */
    REG_HMAC_CTRL = HMAC_CTRL_FINAL;

    memset(blk, 0, 64);
    memcpy(blk, inner_digest, 32);
    blk[32] = 0x80;
    uint64_t outer_bits = 768ULL;
    for (int i = 0; i < 8; i++)
        blk[63 - i] = (uint8_t)(outer_bits >> (8 * i));
    for (int i = 0; i < 8; i++) {
        memcpy(&w, blk + i * 8, 8);
        SHA_BLOCK_PTR[i] = w;
    }
    REG_SHA_CTRL = SHA_CTRL_START;
    sha_wait_done();

    for (int i = 0; i < 4; i++) {
        w = SHA_DIGEST_PTR[i];
        memcpy(tag + i * 8, &w, 8);
    }
}

/* ── Main self-test ──────────────────────────────────────────────────────── */

int main(void) {
    int any_fail = 0;
    uint8_t buf[32];

    printf("=== KlaussCPU crypto hardware self-test ===\n");

    /* ===== Stage 1: AES-128 FIPS-197 §B vector ===== */
    show(0x00010000u);
    printf("\n[1] AES-128 ECB (FIPS-197 Appendix B)\n");
    delay_ms(500);
    {
        static const uint8_t key[16] = {
            0x2b,0x7e,0x15,0x16, 0x28,0xae,0xd2,0xa6,
            0xab,0xf7,0x15,0x88, 0x09,0xcf,0x4f,0x3c
        };
        static const uint8_t pt[16] = {
            0x32,0x43,0xf6,0xa8, 0x88,0x5a,0x30,0x8d,
            0x31,0x31,0x98,0xa2, 0xe0,0x37,0x07,0x34
        };
        static const uint8_t expected_ct[16] = {
            0x39,0x25,0x84,0x1d, 0x02,0xdc,0x09,0xfb,
            0xdc,0x11,0x85,0x97, 0x19,0x6a,0x0b,0x32
        };
        uint8_t ct[16], pt2[16];

        aes_set_key(key);
        aes_encrypt_block(pt, ct);
        aes_decrypt_block(expected_ct, pt2);

        printf("  Encrypt:\n");
        print_hex("expected CT", expected_ct, 16);
        print_hex("actual   CT", ct, 16);
        printf("  Decrypt (round-trip):\n");
        print_hex("expected PT", pt, 16);
        print_hex("actual   PT", pt2, 16);

        int enc_ok = (bcmp16(ct, expected_ct) == 0);
        int dec_ok = (bcmp16(pt2, pt) == 0);
        printf("  encrypt=%s  decrypt=%s  => %s\n",
               enc_ok ? "PASS" : "FAIL",
               dec_ok ? "PASS" : "FAIL",
               (enc_ok && dec_ok) ? "PASS" : "FAIL");

        if (enc_ok && dec_ok)
            show(0x00010000u);
        else { show(0x0001EEEEu); any_fail = 1; }
    }
    delay_ms(1500);

    /* ===== Stage 2: SHA-256("abc") FIPS-180-4 §B.1 ===== */
    show(0x00020000u);
    printf("\n[2] SHA-256(\"abc\") (FIPS-180-4 B.1)\n");
    delay_ms(500);
    {
        static const uint8_t msg[3] = { 'a', 'b', 'c' };
        /* SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2ec7
         *                   3b338c0f 4b7e9073 e37ffe02 1c64f80b */
        static const uint8_t expected[32] = {
           0xba,0x78,0x16,0xbf, 0x8f,0x01,0xcf,0xea,
            0x41,0x41,0x40,0xde, 0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3, 0x96,0x17,0x7a,0x9c,
            0xb4,0x10,0xff,0x61, 0xf2,0x00,0x15,0xad
        };
        sha256_one_block(msg, 3, buf);

        print_hex("expected", expected, 32);
        print_hex("actual  ", buf,      32);
        int ok = (bcmp32(buf, expected) == 0);
        printf("  => %s\n", ok ? "PASS" : "FAIL");

        if (ok)
            show(0x00020000u);
        else { show(0x0002EEEEu); any_fail = 1; }
    }
    delay_ms(1500);

    /* ===== Stage 3: HMAC-SHA-256 RFC 4231 TC1 ===== */
    show(0x00030000u);
    printf("\n[3] HMAC-SHA-256 (RFC 4231 TC1, key=0x0b*20, data=\"Hi There\")\n");
    delay_ms(500);
    {
        uint8_t key[20];
        memset(key, 0x0b, 20);
        static const uint8_t data[8] = { 'H','i',' ','T','h','e','r','e' };
        /* Expected: b0344c61 d8db3853 5ca8afce af0bf12b
         *           881dc200 c9833da7 26e9376c 2e32cff7 */
        static const uint8_t expected[32] = {
            0xb0,0x34,0x4c,0x61, 0xd8,0xdb,0x38,0x53,
            0x5c,0xa8,0xaf,0xce, 0xaf,0x0b,0xf1,0x2b,
            0x88,0x1d,0xc2,0x00, 0xc9,0x83,0x3d,0xa7,
            0x26,0xe9,0x37,0x6c, 0x2e,0x32,0xcf,0xf7
        };
        hmac_sha256_hw(key, 20, data, 8, buf);

        print_hex("expected", expected, 32);
        print_hex("actual  ", buf,      32);
        int ok = (bcmp32(buf, expected) == 0);
        printf("  => %s\n", ok ? "PASS" : "FAIL");

        if (ok)
            show(0x00030000u);
        else { show(0x0003EEEEu); any_fail = 1; }
    }
    delay_ms(1500);

    /* ===== Stage 4: GHASH — NIST GCM TC2 two-step multiply ===== */
    show(0x00040000u);
    printf("\n[4] GHASH/GCM (NIST GCM TC2, K=0, 1-block CT)\n");
    delay_ms(500);
    {
        /* H = AES_K=0(0^128) */
        uint8_t zero16[16];
        memset(zero16, 0, 16);
        aes_set_key(zero16);
        uint8_t H[16];
        aes_encrypt_block(zero16, H);
        printf("  H (AES_K=0(0)): ");
        for (int i = 0; i < 16; i++) printf("%02X", (unsigned)H[i]);
        printf("\n");

        uint64_t h0, h1;
        memcpy(&h0, H,     8); REG_GCM_H0 = h0;
        memcpy(&h1, H + 8, 8); REG_GCM_H1 = h1;

        REG_GCM_CTRL = GCM_CTRL_RESET;

        /* Step 1: tag = (0 XOR CT) * H */
        static const uint8_t ct[16] = {
            0x03,0x88,0xda,0xce, 0x60,0xb6,0xa3,0x92,
            0xf3,0x28,0xc2,0xb9, 0x71,0xb2,0xfe,0x78
        };
        uint64_t x0, x1;
        memcpy(&x0, ct,     8); REG_GCM_X0 = x0;
        memcpy(&x1, ct + 8, 8); REG_GCM_X1 = x1;
        REG_GCM_CTRL = GCM_CTRL_GO;
        gcm_wait_done();

        /* Step 2: tag = (tag XOR lengths) * H; lengths = 0^64 || 128 (bits) */
        static const uint8_t lengths[16] = {
            0,0,0,0, 0,0,0,0,
            0,0,0,0, 0,0,0,0x80
        };
        memcpy(&x0, lengths,     8); REG_GCM_X0 = x0;
        memcpy(&x1, lengths + 8, 8); REG_GCM_X1 = x1;
        REG_GCM_CTRL = GCM_CTRL_GO;
        gcm_wait_done();

        /* Final tag = GHASH XOR AES_K(J0); J0 = IV(96 zeros) || 0x00000001 */
        uint8_t j0[16];
        memset(j0, 0, 16); j0[15] = 0x01;
        uint8_t j0_ks[16];
        aes_encrypt_block(j0, j0_ks);

        uint8_t tag[16];
        uint64_t t0 = REG_GCM_TAG0, t1 = REG_GCM_TAG1;
        uint64_t k0, k1;
        memcpy(&k0, j0_ks,     8);
        memcpy(&k1, j0_ks + 8, 8);
        t0 ^= k0; t1 ^= k1;
        memcpy(tag,     &t0, 8);
        memcpy(tag + 8, &t1, 8);

        /* NIST GCM TC2 T = ab6e47d4 2cec13bd f53a67b2 1257bddf */
        static const uint8_t expected[16] = {
            0xab,0x6e,0x47,0xd4, 0x2c,0xec,0x13,0xbd,
            0xf5,0x3a,0x67,0xb2, 0x12,0x57,0xbd,0xdf
        };

        print_hex("expected tag", expected, 16);
        print_hex("actual   tag", tag,      16);
        int ok = (bcmp16(tag, expected) == 0);
        printf("  => %s\n", ok ? "PASS" : "FAIL");

        if (ok)
            show(0x00040000u);
        else { show(0x0004EEEEu); any_fail = 1; }
    }
    delay_ms(1500);

    /* ===== Stage 5: TRNG ===== */
    show(0x00050000u);
    printf("\n[5] TRNG (4 x 64-bit words)\n");
    delay_ms(500);
    {
        /* Enable, drain any stale FIFO entries, then reseed for fresh data. */
        REG_TRNG_CTRL = TRNG_CTRL_ENABLE;
        delay_ms(5);

        /* Drain: read and discard whatever is already in the FIFO. */
        {
            uint64_t dummy;
            uint32_t drain = 32;   /* more than max FIFO depth */
            while ((REG_TRNG_STATUS & TRNG_STATUS_READY) && drain--) {
                dummy = REG_TRNG_DATA;
                (void)dummy;
            }
        }

        /* Reseed: RESEED bit is self-clearing; ring oscillators re-fill FIFO. */
        REG_TRNG_CTRL = TRNG_CTRL_ENABLE | TRNG_CTRL_RESEED;
        delay_ms(10);   /* wait for reseed + FIFO to fill */

        uint64_t words[4];
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            /* Poll with timeout (~10 ms each). */
            uint32_t timeout = 200000u;
            while (!trng_read64(&words[i])) {
                if (--timeout == 0) { ok = 0; break; }
            }
            if (!ok) break;
        }

        uint32_t status = REG_TRNG_STATUS;
        int health_ok = (status & TRNG_STATUS_HEALTH_OK) != 0;
        uint64_t nonzero = words[0] | words[1] | words[2] | words[3];

        for (int i = 0; i < 4; i++) {
            uint32_t hi = (uint32_t)(words[i] >> 32);
            uint32_t lo = (uint32_t)(words[i]);
            printf("  word[%d]: %08lX%08lX\n", i,
                   (unsigned long)hi, (unsigned long)lo);
        }
        printf("  HEALTH_OK=%d  non-zero=%d  read_ok=%d\n",
               health_ok, (nonzero != 0), ok);
        int pass = ok && (nonzero != 0) && health_ok;
        printf("  => %s\n", pass ? "PASS" : "FAIL");

        if (pass)
            show(0x00050000u);
        else { show(0x0005EEEEu); any_fail = 1; }
    }
    delay_ms(1500);

    /* ===== Summary ===== */
    printf("\n=== SUMMARY: %s ===\n", any_fail ? "FAIL" : "ALL PASS");
    show(any_fail ? 0x00FFEEEEu : 0x00990000u);
    for (;;) delay_ms(1000);
}

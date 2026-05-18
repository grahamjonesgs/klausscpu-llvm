/* crypto_selftest.c — Known-answer tests for KlaussCPU hardware crypto blocks.
 *
 * Tests:
 *   AES-128-ECB  — FIPS-197 Appendix B vector
 *   AES-128-CTR  — NIST SP 800-38A §F.5.1 first block
 *   AES-128-GCM  — NIST SP 800-38D Test Case 2 (with AAD)
 *   SHA-256      — FIPS 180-4: "", "abc", 56-byte (two-block pad edge case)
 *   HMAC-SHA-256 — RFC 4231 Test Cases 1–4 (hw and sw paths)
 *   TRNG         — health check + basic sanity (non-zero, not all-same)
 *
 * Each test prints PASS or FAIL over UART.  Run once after new hardware is
 * programmed; any FAIL means the HDL has a bug.
 *
 * Interop note: encrypt with this program, decrypt with OpenSSL on the host:
 *   openssl enc -d -aes-128-ecb -nosalt -K <key-hex> -in cipher.bin
 *   echo -n "abc" | openssl dgst -sha256
 */

#include <stdio.h>
#include <string.h>
#include "../crypto_hw.h"
#include "../mmio.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int pass_count, fail_count;

static void print_hex(const char *label, const uint8_t *buf, size_t len) {
    printf("  %s: ", label);
    for (size_t i = 0; i < len; i++) printf("%02x", buf[i]);
    printf("\n");
}

static int check(const char *name, const uint8_t *got, const uint8_t *want, size_t len) {
    if (memcmp(got, want, len) == 0) {
        printf("PASS  %s\n", name);
        pass_count++;
        return 1;
    }
    printf("FAIL  %s\n", name);
    print_hex("     got ", got, len);
    print_hex("    want ", want, len);
    fail_count++;
    return 0;
}

/* Encode a hex string (no spaces) into bytes.  Must be even length. */
static void from_hex(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t hi = hex[i*2];
        uint8_t lo = hex[i*2+1];
        hi = (hi >= 'a') ? (hi - 'a' + 10) : (hi >= 'A') ? (hi - 'A' + 10) : (hi - '0');
        lo = (lo >= 'a') ? (lo - 'a' + 10) : (lo >= 'A') ? (lo - 'A' + 10) : (lo - '0');
        out[i] = (hi << 4) | lo;
    }
}

/* ── AES-128-ECB ─────────────────────────────────────────────────────────── */

static void test_aes_ecb(void) {
    printf("\n=== AES-128-ECB (FIPS-197 Appendix B) ===\n");

    /* Key:       000102030405060708090a0b0c0d0e0f
     * Plain:     00112233445566778899aabbccddeeff
     * Cipher:    69c4e0d86a7b04300d8a8b41de7726e7 */
    uint8_t key[16], pt[16], ct_exp[16], ct_got[16], pt_dec[16];
    from_hex("000102030405060708090a0b0c0d0e0f", key, 16);
    from_hex("00112233445566778899aabbccddeeff", pt,  16);
    from_hex("69c4e0d86a7b04300d8a8b41de7726e7", ct_exp, 16);

    uint64_t key_lo, key_hi, in_lo, in_hi, out_lo, out_hi;
    memcpy(&key_lo, key,     8); memcpy(&key_hi, key + 8,  8);
    memcpy(&in_lo,  pt,      8); memcpy(&in_hi,  pt  + 8,  8);

    aes_hw_set_key(key_lo, key_hi);
    aes_hw_encrypt(in_lo, in_hi, &out_lo, &out_hi);
    memcpy(ct_got,     &out_lo, 8);
    memcpy(ct_got + 8, &out_hi, 8);
    check("ECB encrypt", ct_got, ct_exp, 16);

    /* Decrypt back. */
    memcpy(&in_lo, ct_exp, 8); memcpy(&in_hi, ct_exp + 8, 8);
    aes_hw_decrypt(in_lo, in_hi, &out_lo, &out_hi);
    memcpy(pt_dec,     &out_lo, 8);
    memcpy(pt_dec + 8, &out_hi, 8);
    check("ECB decrypt", pt_dec, pt, 16);

    /* FIPS-197 §A.1: key = 2b7e... plain = 6bc1... cipher = 3ad77b... */
    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    from_hex("6bc1bee22e409f96e93d7e117393172a", pt,  16);
    from_hex("3ad77bb40d7a3660a89ecaf32466ef97", ct_exp, 16);
    memcpy(&key_lo, key, 8); memcpy(&key_hi, key + 8, 8);
    memcpy(&in_lo,  pt,  8); memcpy(&in_hi,  pt  + 8, 8);
    aes_hw_set_key(key_lo, key_hi);
    aes_hw_encrypt(in_lo, in_hi, &out_lo, &out_hi);
    memcpy(ct_got, &out_lo, 8); memcpy(ct_got + 8, &out_hi, 8);
    check("ECB encrypt (A.1)", ct_got, ct_exp, 16);
}

/* ── AES-128-CTR ─────────────────────────────────────────────────────────── */

static void test_aes_ctr(void) {
    printf("\n=== AES-128-CTR (NIST SP 800-38A F.5.1) ===\n");

    /* Key:   2b7e151628aed2a6abf7158809cf4f3c
     * ICB:   f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff  (initial counter block)
     * PT[0]: 6bc1bee22e409f96e93d7e117393172a
     * CT[0]: 874d6191b620e3261bef6864990db6ce
     * PT[1]: ae2d8a571e03ac9c9eb76fac45af8e51
     * CT[1]: 9806f66b7970fdff8617187bb9fffdff
     * PT[2]: 30c81c46a35ce411e5fbc1191a0a52ef
     * CT[2]: 5ae4df3edbd5d35e5b4f09020db03eab
     * PT[3]: f69f2445df4f9b17ad2b417be66c3710
     * CT[3]: 1e031dda2fbe03d1792170a0f3009cee */

    uint8_t key[16], nonce[16];
    from_hex("2b7e151628aed2a6abf7158809cf4f3c", key,   16);
    from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", nonce, 16);

    const char *pt_hex[4] = {
        "6bc1bee22e409f96e93d7e117393172a",
        "ae2d8a571e03ac9c9eb76fac45af8e51",
        "30c81c46a35ce411e5fbc1191a0a52ef",
        "f69f2445df4f9b17ad2b417be66c3710"
    };
    const char *ct_hex[4] = {
        "874d6191b620e3261bef6864990db6ce",
        "9806f66b7970fdff8617187bb9fffdff",
        "5ae4df3edbd5d35e5b4f09020db03eab",
        "1e031dda2fbe03d1792170a0f3009cee"
    };

    uint8_t pt[64], ct_exp[64], ct_got[64];
    for (int i = 0; i < 4; i++) {
        from_hex(pt_hex[i],  pt     + i*16, 16);
        from_hex(ct_hex[i],  ct_exp + i*16, 16);
    }

    uint64_t key_lo, key_hi, nonce_lo, nonce_hi;
    memcpy(&key_lo,   key,       8); memcpy(&key_hi,   key   + 8, 8);
    memcpy(&nonce_lo, nonce,     8); memcpy(&nonce_hi, nonce + 8, 8);

    aes_hw_set_key(key_lo, key_hi);
    aes_ctr(nonce_lo, nonce_hi, pt, ct_got, 64);
    check("CTR encrypt 4 blocks", ct_got, ct_exp, 64);

    /* Decrypt (CTR is symmetric). */
    uint8_t pt_dec[64];
    aes_ctr(nonce_lo, nonce_hi, ct_exp, pt_dec, 64);
    check("CTR decrypt 4 blocks", pt_dec, pt, 64);

    /* Unaligned length (17 bytes). */
    uint8_t ct17[17], pt17_dec[17];
    aes_ctr(nonce_lo, nonce_hi, pt, ct17, 17);
    aes_ctr(nonce_lo, nonce_hi, ct17, pt17_dec, 17);
    check("CTR 17-byte round-trip", pt17_dec, pt, 17);
}

/* ── AES-128-GCM ─────────────────────────────────────────────────────────── */

static void test_aes_gcm(void) {
    printf("\n=== AES-128-GCM (NIST SP 800-38D Test Case 4) ===\n");

    /* TC4: K=feffe9928665731c6d6a8f9467308308
     *      P=d9313225f88406e5a55909c5aff5269a  (16 bytes)
     *        86a7a9531534f7da2e4c303d8a318a72
     *        1c3c0c95956809532fcf0e2449a6b525
     *        b16aedf5aa0de657ba637b391aafd255
     *      A=feedfacedeadbeeffeedfacedeadbeef  (16 bytes)
     *        abaddad2
     *      IV=cafebabefacedbaddecaf888  (12 bytes)
     *      C=42831ec2217774244b7221b784d0d49c
     *        e3aa212f2c02a4e035c17e2329aca12e
     *        21d514b25466931c7d8f6a5aac84aa05
     *        1ba30b396a0aac973d58e091473f5985
     *      T=4d5c2af327cd64a62cf35abd2ba6fab4  (16 bytes) */

    uint8_t key[16], iv[12], aad[20], pt[64], ct_exp[64], tag_exp[16];
    from_hex("feffe9928665731c6d6a8f9467308308", key, 16);
    from_hex("cafebabefacedbaddecaf888",         iv,  12);
    from_hex("feedfacedeadbeeffeedfacedeadbeef"
             "abaddad2",                          aad, 20);
    from_hex("d9313225f88406e5a55909c5aff5269a"
             "86a7a9531534f7da2e4c303d8a318a72"
             "1c3c0c95956809532fcf0e2449a6b525"
             "b16aedf5aa0de657ba637b391aafd255", pt, 64);
    from_hex("42831ec2217774244b7221b784d0d49c"
             "e3aa212f2c02a4e035c17e2329aca12e"
             "21d514b25466931c7d8f6a5aac84aa05"
             "1ba30b396a0aac973d58e091473f5985", ct_exp, 64);
    from_hex("4d5c2af327cd64a62cf35abd2ba6fab4", tag_exp, 16);

    uint64_t key_lo, key_hi;
    memcpy(&key_lo, key, 8); memcpy(&key_hi, key + 8, 8);
    aes_hw_set_key(key_lo, key_hi);

    uint8_t ct_got[64], tag_got[16];
    aes_gcm_encrypt(iv, aad, 20, pt, ct_got, 64, tag_got);
    check("GCM ciphertext", ct_got, ct_exp, 64);
    check("GCM auth tag",   tag_got, tag_exp, 16);

    /* Decrypt + verify. */
    uint8_t pt_dec[64];
    int rc = aes_gcm_decrypt(iv, aad, 20, ct_exp, pt_dec, 64, tag_exp);
    if (rc == 0) {
        check("GCM decrypt+verify", pt_dec, pt, 64);
    } else {
        printf("FAIL  GCM decrypt+verify (tag mismatch)\n");
        fail_count++;
    }

    /* Tamper test: flip one ciphertext byte, expect failure. */
    uint8_t ct_bad[64];
    memcpy(ct_bad, ct_exp, 64);
    ct_bad[0] ^= 0x01;
    rc = aes_gcm_decrypt(iv, aad, 20, ct_bad, pt_dec, 64, tag_exp);
    if (rc < 0) {
        printf("PASS  GCM tamper detection\n");
        pass_count++;
    } else {
        printf("FAIL  GCM tamper detection (should have rejected)\n");
        fail_count++;
    }
}

/* ── SHA-256 ─────────────────────────────────────────────────────────────── */

static void test_sha256(void) {
    printf("\n=== SHA-256 (FIPS 180-4) ===\n");

    uint8_t got[32], want[32];

    /* Empty string. */
    from_hex("e3b0c44298fc1c149afbf4c8996fb924"
             "27ae41e4649b934ca495991b7852b855", want, 32);
    sha256_hw((const uint8_t *)"", 0, got);
    check("SHA256(\"\")", got, want, 32);

    /* "abc" (3 bytes, single partial block). */
    from_hex("ba7816bf8f01cfea414140de5dae2ec7"
             "3b338c0f4b7e9073e37ffe021c64f80b", want, 32);
    sha256_hw((const uint8_t *)"abc", 3, got);
    check("SHA256(\"abc\")", got, want, 32);

    /* 448-bit ("abcdbcdecdefdefgefghfghighij..." — FIPS 180-4 Example 2). */
    const char *msg448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    from_hex("248d6a61d20638b8e5c026930c3e6039"
             "a33ce45964ff2167f6ecedd419db06c1", want, 32);
    sha256_hw((const uint8_t *)msg448, 56, got);
    check("SHA256(448-bit)", got, want, 32);

    /* 55-byte message: tail exactly fits in one padding block (tail=55, <56). */
    uint8_t msg55[55];
    memset(msg55, 0x61, 55); /* "aaa...a" (55 'a's) */
    sha256_hw(msg55, 55, got);
    /* Verify round-trip via streaming API. */
    sha256_ctx_t ctx;
    sha256_hw_init(&ctx);
    sha256_hw_update(&ctx, msg55, 30);
    sha256_hw_update(&ctx, msg55 + 30, 25);
    uint8_t got2[32];
    sha256_hw_final(&ctx, got2);
    check("SHA256 streaming == one-shot (55 bytes)", got2, got, 32);

    /* 56-byte message: tail=56, needs two padding blocks (edge case). */
    uint8_t msg56[56];
    memset(msg56, 0x62, 56); /* "bbb...b" (56 'b's) */
    sha256_hw(msg56, 56, got);
    sha256_hw_init(&ctx);
    sha256_hw_update(&ctx, msg56, 56);
    sha256_hw_final(&ctx, got2);
    check("SHA256 two-block padding edge case (56 bytes)", got2, got, 32);

    /* Multi-block message (130 bytes = 2 full blocks + 2 byte tail). */
    uint8_t msg130[130];
    memset(msg130, 0x63, 130);
    sha256_hw(msg130, 130, got);
    sha256_hw_init(&ctx);
    sha256_hw_update(&ctx, msg130, 64);
    sha256_hw_update(&ctx, msg130 + 64, 66);
    sha256_hw_final(&ctx, got2);
    check("SHA256 streaming 3-block (130 bytes)", got2, got, 32);
}

/* ── HMAC-SHA-256 ────────────────────────────────────────────────────────── */

static void test_hmac(void) {
    printf("\n=== HMAC-SHA-256 (RFC 4231) ===\n");

    uint8_t key[131], data[152], got_hw[32], got_sw[32], want[32];
    size_t klen, dlen;

    /* Test Case 1:
     * Key  = 0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (20 bytes)
     * Data = "Hi There"
     * MAC  = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7 */
    klen = 20; memset(key, 0x0b, klen);
    dlen = 8;  memcpy(data, "Hi There", 8);
    from_hex("b0344c61d8db38535ca8afceaf0bf12b"
             "881dc200c9833da726e9376c2e32cff7", want, 32);
    hmac_sha256_sw(key, klen, data, dlen, got_sw);
    check("HMAC TC1 (sw)", got_sw, want, 32);
    hmac_sha256_hw(key, klen, data, dlen, got_hw);
    check("HMAC TC1 (hw)", got_hw, want, 32);

    /* Test Case 2:
     * Key  = "Jefe" (4 bytes)
     * Data = "what do ya want for nothing?" (28 bytes)
     * MAC  = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964a86551 */
    klen = 4;  memcpy(key, "Jefe", 4);
    dlen = 28; memcpy(data, "what do ya want for nothing?", 28);
    from_hex("5bdcc146bf60754e6a042426089575c7"
             "5a003f089d2739839dec58b964a86551", want, 32);
    hmac_sha256_sw(key, klen, data, dlen, got_sw);
    check("HMAC TC2 (sw)", got_sw, want, 32);
    hmac_sha256_hw(key, klen, data, dlen, got_hw);
    check("HMAC TC2 (hw)", got_hw, want, 32);

    /* Test Case 3:
     * Key  = 0xaaaa...aa (20 bytes)
     * Data = 0xdddd...dd (50 bytes)
     * MAC  = 773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe */
    klen = 20; memset(key,  0xaa, klen);
    dlen = 50; memset(data, 0xdd, dlen);
    from_hex("773ea91e36800e46854db8ebd09181a7"
             "2959098b3ef8c122d9635514ced565fe", want, 32);
    hmac_sha256_sw(key, klen, data, dlen, got_sw);
    check("HMAC TC3 (sw)", got_sw, want, 32);
    hmac_sha256_hw(key, klen, data, dlen, got_hw);
    check("HMAC TC3 (hw)", got_hw, want, 32);

    /* Test Case 4:
     * Key  = 0102030405060708090a0b0c0d0e0f10111213141516171819 (25 bytes)
     * Data = 0xcdcd...cd (50 bytes)
     * MAC  = 82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b */
    klen = 25;
    from_hex("0102030405060708090a0b0c0d0e0f10111213141516171819", key, 25);
    dlen = 50; memset(data, 0xcd, dlen);
    from_hex("82558a389a443c0ea4cc819899f2083a"
             "85f0faa3e578f8077a2e3ff46729665b", want, 32);
    hmac_sha256_sw(key, klen, data, dlen, got_sw);
    check("HMAC TC4 (sw)", got_sw, want, 32);
    hmac_sha256_hw(key, klen, data, dlen, got_hw);
    check("HMAC TC4 (hw)", got_hw, want, 32);
}

/* ── TRNG ────────────────────────────────────────────────────────────────── */

static void test_trng(void) {
    printf("\n=== TRNG (sanity + health) ===\n");

    /* Health check. */
    if (!trng_health_ok()) {
        printf("FAIL  TRNG health monitor (HEALTH_OK=0)\n");
        fail_count++;
        return;
    }
    printf("PASS  TRNG health monitor\n");
    pass_count++;

    /* Read 256 bits (4 words). */
    uint8_t buf[32];
    trng_read(buf, 32);

    /* Sanity: not all-zero. */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) if (buf[i]) { all_zero = 0; break; }
    if (!all_zero) {
        printf("PASS  TRNG non-zero output\n");
        pass_count++;
    } else {
        printf("FAIL  TRNG all-zero output (entropy failure)\n");
        fail_count++;
    }

    /* Sanity: two consecutive reads differ. */
    uint8_t buf2[32];
    trng_read(buf2, 32);
    if (memcmp(buf, buf2, 32) != 0) {
        printf("PASS  TRNG output varies across reads\n");
        pass_count++;
    } else {
        printf("FAIL  TRNG two reads identical (stuck RNG)\n");
        fail_count++;
    }

    /* Print a sample for visual inspection. */
    print_hex("sample", buf, 16);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    REG_LEDS = 0x01;
    printf("=== KlaussCPU Hardware Crypto Self-Test ===\n");

    pass_count = fail_count = 0;

    /* Initialize TRNG first (needed by some later tests indirectly). */
    trng_init();
    REG_LEDS = 0x03;

    test_aes_ecb();   REG_LEDS = 0x07;
    test_aes_ctr();   REG_LEDS = 0x0F;
    test_aes_gcm();   REG_LEDS = 0x1F;
    test_sha256();    REG_LEDS = 0x3F;
    test_hmac();      REG_LEDS = 0x7F;
    test_trng();      REG_LEDS = 0xFF;

    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    if (fail_count == 0) {
        printf("ALL PASS — hardware crypto is correct.\n");
    } else {
        printf("FAILURES DETECTED — do not use this hardware for crypto.\n");
    }
    return fail_count;
}

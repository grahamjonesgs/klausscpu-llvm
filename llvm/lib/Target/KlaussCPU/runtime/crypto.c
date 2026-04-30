/* crypto.c — CRC32, SHA-256, and Base64 round-trip tests for KlaussCPU.
 *
 * Compile via Makefile:  make crypto.elf  &&  make crypto.bin
 *
 * Exercises:
 *   - 32-bit modular arithmetic with wrap-around (post int=32 migration)
 *   - Bit manipulation (rotates, shifts, XOR)
 *   - Byte-level memory access (LDIDX8 / MEMSET8)
 *   - 32-bit reads of byte buffers via shift-and-OR (no aliasing tricks)
 *   - Static rodata tables (the SHA-256 K[] and the Base64 alphabet)
 *
 * Test vectors are taken from FIPS 180-4 (SHA-256) and RFC 4648 (Base64);
 * CRC32 values match the IEEE 802.3 / zlib variant.
 */

extern void  print_str(char *s);
extern void  print_int(long long n);
extern void  print_hex_prefix(long long val);
extern void  newline(void);
extern int   strlen(char *s);
extern int   strcmp(char *a, char *b);
extern void *memset(void *p, int val, int n);
extern void *memcpy(void *dst, void *src, int n);

typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned char      uint8_t;

static int g_pass;
static int g_fail;

static void check(char *name, int ok) {
    print_str(name);
    if (ok) { print_str("PASS"); g_pass++; }
    else    { print_str("FAIL"); g_fail++; }
    newline();
}

static void check_hex(char *name, uint32_t got, uint32_t expected) {
    print_str(name);
    if (got == expected) {
        print_str("PASS");
        g_pass++;
    } else {
        print_str("FAIL  got=");
        print_hex_prefix((long long)(uint64_t)got);
        print_str(" exp=");
        print_hex_prefix((long long)(uint64_t)expected);
        g_fail++;
    }
    newline();
}

/* ── CRC32 (IEEE 802.3 / zlib) ────────────────────────────────────────────── */

static uint32_t crc32(const uint8_t *data, int len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────────────── */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t H[8], const uint8_t blk[64]) {
    uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)blk[i*4    ] << 24)
             | ((uint32_t)blk[i*4 + 1] << 16)
             | ((uint32_t)blk[i*4 + 2] <<  8)
             | ((uint32_t)blk[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(W[i-15],  7) ^ rotr32(W[i-15], 18) ^ (W[i-15] >>  3);
        uint32_t s1 = rotr32(W[i- 2], 17) ^ rotr32(W[i- 2], 19) ^ (W[i- 2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
    uint32_t e = H[4], f = H[5], g = H[6], h = H[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + W[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d;
    H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

/* sha256 of a < 256-byte message into 32-byte big-endian digest. */
static void sha256(const uint8_t *msg, int len, uint8_t out[32]) {
    uint32_t H[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    uint8_t pad[256];
    int padlen = ((len + 9 + 63) / 64) * 64;
    for (int i = 0; i < len; i++)        pad[i] = msg[i];
    pad[len] = 0x80;
    for (int i = len + 1; i < padlen - 8; i++) pad[i] = 0;
    uint64_t bitlen = (uint64_t)len * 8;
    pad[padlen - 8] = (uint8_t)(bitlen >> 56);
    pad[padlen - 7] = (uint8_t)(bitlen >> 48);
    pad[padlen - 6] = (uint8_t)(bitlen >> 40);
    pad[padlen - 5] = (uint8_t)(bitlen >> 32);
    pad[padlen - 4] = (uint8_t)(bitlen >> 24);
    pad[padlen - 3] = (uint8_t)(bitlen >> 16);
    pad[padlen - 2] = (uint8_t)(bitlen >>  8);
    pad[padlen - 1] = (uint8_t)(bitlen      );

    for (int blk = 0; blk < padlen; blk += 64)
        sha256_block(H, pad + blk);

    for (int i = 0; i < 8; i++) {
        out[i*4    ] = (uint8_t)(H[i] >> 24);
        out[i*4 + 1] = (uint8_t)(H[i] >> 16);
        out[i*4 + 2] = (uint8_t)(H[i] >>  8);
        out[i*4 + 3] = (uint8_t)(H[i]      );
    }
}

static int hexcmp(const uint8_t digest[32], const char *expected_hex) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        if (expected_hex[i*2    ] != hexd[(digest[i] >> 4) & 0xF]) return 0;
        if (expected_hex[i*2 + 1] != hexd[ digest[i]       & 0xF]) return 0;
    }
    return 1;
}

/* ── Base64 (RFC 4648) ────────────────────────────────────────────────────── */

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, int len, char *out, int cap) {
    int oi = 0;
    for (int i = 0; i < len; i += 3) {
        int n = len - i;
        if (n > 3) n = 3;
        uint32_t v = 0;
        for (int j = 0; j < n; j++)
            v |= (uint32_t)in[i+j] << ((2 - j) * 8);
        if (oi + 4 > cap) return -1;
        out[oi++] = b64_chars[(v >> 18) & 0x3F];
        out[oi++] = b64_chars[(v >> 12) & 0x3F];
        out[oi++] = (n >= 2) ? b64_chars[(v >>  6) & 0x3F] : '=';
        out[oi++] = (n >= 3) ? b64_chars[ v        & 0x3F] : '=';
    }
    if (oi < cap) out[oi] = '\0';
    return oi;
}

static int b64_lookup(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *in, uint8_t *out, int cap) {
    int oi = 0;
    uint32_t v = 0;
    int bits = 0;
    for (int i = 0; in[i]; i++) {
        int c = (int)(unsigned char)in[i];
        if (c == '=') break;
        int d = b64_lookup(c);
        if (d < 0) continue;            /* skip whitespace etc. */
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (oi >= cap) return -1;
            out[oi++] = (uint8_t)((v >> bits) & 0xFF);
        }
    }
    return oi;
}

/* ── Test ─────────────────────────────────────────────────────────────────── */

int main(void) {
    g_pass = 0;
    g_fail = 0;

    print_str("=== Crypto / Hash ===");
    newline();

    /* CRC32 vectors (zlib / IEEE 802.3) */
    {
        static const uint8_t empty[] = "";
        static const uint8_t a[]     = "a";
        static const uint8_t abc[]   = "abc";
        static const uint8_t s9[]    = "123456789";
        static const uint8_t lazy[]  = "The quick brown fox jumps over the lazy dog";

        check_hex("CRC32 empty:      ", crc32(empty, 0),  0x00000000u);
        check_hex("CRC32 'a':        ", crc32(a,     1),  0xE8B7BE43u);
        check_hex("CRC32 'abc':      ", crc32(abc,   3),  0x352441C2u);
        check_hex("CRC32 '12...9':   ", crc32(s9,    9),  0xCBF43926u);
        check_hex("CRC32 'fox...':   ", crc32(lazy, 43),  0x414FA339u);
    }

    /* SHA-256 vectors (FIPS 180-4) */
    {
        uint8_t digest[32];

        sha256((const uint8_t *)"", 0, digest);
        check("SHA256 empty:     ",
              hexcmp(digest,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

        sha256((const uint8_t *)"abc", 3, digest);
        check("SHA256 'abc':     ",
              hexcmp(digest,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

        sha256((const uint8_t *)"a", 1, digest);
        check("SHA256 'a':       ",
              hexcmp(digest,
                "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"));

        /* 56-byte message — exactly one block (no extra padding-block). */
        sha256((const uint8_t *)
               "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
               digest);
        check("SHA256 56-byte:   ",
              hexcmp(digest,
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }

    /* Base64 (RFC 4648 §10) */
    {
        char enc[64];
        uint8_t dec[64];
        int n;

        n = base64_encode((const uint8_t *)"",      0, enc, 64);
        enc[n] = '\0';
        check("B64 enc empty:    ", strcmp(enc, "") == 0);

        n = base64_encode((const uint8_t *)"f",     1, enc, 64);
        enc[n] = '\0';
        check("B64 enc 'f':      ", strcmp(enc, "Zg==") == 0);

        n = base64_encode((const uint8_t *)"fo",    2, enc, 64);
        enc[n] = '\0';
        check("B64 enc 'fo':     ", strcmp(enc, "Zm8=") == 0);

        n = base64_encode((const uint8_t *)"foo",   3, enc, 64);
        enc[n] = '\0';
        check("B64 enc 'foo':    ", strcmp(enc, "Zm9v") == 0);

        n = base64_encode((const uint8_t *)"foob",  4, enc, 64);
        enc[n] = '\0';
        check("B64 enc 'foob':   ", strcmp(enc, "Zm9vYg==") == 0);

        n = base64_encode((const uint8_t *)"foobar", 6, enc, 64);
        enc[n] = '\0';
        check("B64 enc 'foobar': ", strcmp(enc, "Zm9vYmFy") == 0);

        /* Round trip */
        n = base64_decode("Zm9vYmFy", dec, 64);
        int rt = (n == 6)
              && (dec[0] == 'f') && (dec[1] == 'o') && (dec[2] == 'o')
              && (dec[3] == 'b') && (dec[4] == 'a') && (dec[5] == 'r');
        check("B64 dec 'foobar': ", rt);

        /* Round-trip a longer binary-ish payload */
        static const uint8_t bin[] = {
            0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF, 0xAB,
            0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC
        };
        n = base64_encode(bin, 16, enc, 64);
        enc[n] = '\0';
        n = base64_decode(enc, dec, 64);
        int bin_ok = (n == 16);
        for (int i = 0; i < 16 && bin_ok; i++)
            if (dec[i] != bin[i]) bin_ok = 0;
        check("B64 bin round:    ", bin_ok);
    }

    /* Summary */
    newline();
    print_str("Results: ");
    print_int(g_pass);
    print_str(" pass, ");
    print_int(g_fail);
    print_str(" fail");
    newline();
    return g_fail;
}

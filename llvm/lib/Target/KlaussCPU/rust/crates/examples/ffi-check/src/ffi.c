/* ffi.c - the C half of ffi-check (C ABI version 2, ABI.md).
 *
 * Every function here has a twin in main.rs (rs_*) that computes the same
 * thing, so Rust can check what C returns and C can check what Rust returns.
 * The argument mixes cover each way an aggregate travels: a 3-byte struct
 * (i24), 8 bytes (i64), 12/16 bytes ([2 x i64]), a 16-byte-aligned one
 * (i128), a union, and 24 bytes (by reference to a copy; returned through
 * sret), past R0-R3 onto the stack; and a u32, which travels sign-extended.
 */
#include <stdbool.h>
#include <stdint.h>

typedef uint64_t u64;

struct Tri { uint8_t x, y, z; };
struct Pair { int32_t a; uint32_t b; };
struct Mid { u64 a; uint32_t b; };
struct Wide { __int128 v; };
struct Big { u64 a, b, c; };
union U { uint32_t w; uint8_t b[6]; };

static u64 mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static u64 hash_args(struct Pair p, struct Tri t, uint32_t u, int8_t s,
                     struct Big b, struct Wide w, union U un) {
    u64 h = mix(0, (u64)(int64_t)p.a);
    h = mix(h, p.b);
    h = mix(h, t.x); h = mix(h, t.y); h = mix(h, t.z);
    h = mix(h, (u64)u);
    h = mix(h, (u64)(int64_t)(int32_t)u);
    h = mix(h, (u64)(int64_t)s);
    h = mix(h, b.a); h = mix(h, b.b); h = mix(h, b.c);
    h = mix(h, (u64)w.v); h = mix(h, (u64)(w.v >> 64));
    h = mix(h, un.w); h = mix(h, un.b[4]); h = mix(h, un.b[5]);
    return h;
}

/* ---- C functions Rust calls ---- */

struct Mid c_combine(struct Pair p, struct Tri t, uint32_t u, int8_t s,
                     struct Big b, struct Wide w, union U un) {
    u64 h = hash_args(p, t, u, s, b, w, un);
    b.a = 0xdeadbeef; /* our own copy: the caller's must not change */
    b.c = 0;
    struct Mid m = { h, (uint32_t)h ^ u };
    return m;
}

struct Big c_big(u64 k, struct Mid m) {
    struct Big r = { k ^ m.a, m.b, k + m.b };
    return r;
}

int64_t c_sext(uint32_t x) { return (int64_t)(int32_t)x; }

uint32_t c_u32_ret(uint32_t x) { return x * 3u; }

struct Tri c_tri(struct Tri t) {
    struct Tri r = { t.z, t.x, (uint8_t)(t.y + 1) };
    return r;
}

struct Wide c_wide(struct Wide w, uint32_t u) {
    struct Wide r = { w.v * 3 + u };
    return r;
}

/* ---- Rust functions C calls ---- */

struct Mid rs_combine(struct Pair p, struct Tri t, uint32_t u, int8_t s,
                      struct Big b, struct Wide w, union U un);
struct Big rs_big(u64 k, struct Mid m);
int64_t rs_sext(uint32_t x);
uint32_t rs_u32_ret(uint32_t x);
struct Tri rs_tri(struct Tri t);
struct Wide rs_wide(struct Wide w, uint32_t u);

/* Values from volatile memory, so nothing is constant-folded across the
 * call. */
static volatile uint32_t v_u = 0x80000007u;
static volatile int32_t v_a = -12345;
static volatile u64 v_k = 0x0123456789abcdefull;

/* Calls every rs_* function and checks it against the C twin; returns a bit
 * mask of the checks that failed (0 = all passed). */
uint32_t c_calls_rust(void) {
    uint32_t fail = 0;
    struct Pair p = { v_a, v_u ^ 0x5u };
    struct Tri t = { 1, 0xfe, (uint8_t)v_u };
    struct Big b = { v_k, ~v_k, v_k * 7 };
    struct Wide w = { ((__int128)(int64_t)v_k << 64) | v_u };
    union U un;
    un.w = 0;
    un.b[4] = 0;
    un.b[5] = 0;
    un.w = v_u * 9;
    un.b[4] = 0x44;
    un.b[5] = 0x55;

    struct Mid m = rs_combine(p, t, v_u, (int8_t)-3, b, w, un);
    u64 h = hash_args(p, t, v_u, -3, b, w, un);
    if (m.a != h || m.b != ((uint32_t)h ^ v_u)) fail |= 1u << 0;
    if (b.a != v_k || b.c != v_k * 7) fail |= 1u << 1; /* the callee's copy */

    struct Big r = rs_big(v_k, m);
    struct Big e = c_big(v_k, m);
    if (r.a != e.a || r.b != e.b || r.c != e.c) fail |= 1u << 2;

    if (rs_sext(v_u) != (int64_t)(int32_t)v_u) fail |= 1u << 3;
    uint32_t x = rs_u32_ret(v_u);
    if ((int64_t)(int32_t)x != (int64_t)(int32_t)(v_u * 3u)) fail |= 1u << 4;
    if (x != v_u * 3u) fail |= 1u << 5;

    struct Tri tr = rs_tri(t), te = c_tri(t);
    if (tr.x != te.x || tr.y != te.y || tr.z != te.z) fail |= 1u << 6;

    struct Wide wr = rs_wide(w, v_u), we = c_wide(w, v_u);
    if (wr.v != we.v) fail |= 1u << 7;
    return fail;
}

/* ---- atomics shared with Rust (the __sync_* calls go to klauss-rt) ---- */

uint32_t c_fetch_add32(uint32_t *p, uint32_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

bool c_cas64(u64 *p, u64 *expected, u64 desired) {
    return __atomic_compare_exchange_n(p, expected, desired, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

uint8_t c_xchg8(uint8_t *p, uint8_t v) {
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

int16_t c_fetch_max16(int16_t *p, int16_t v) {
    return __atomic_fetch_max(p, v, __ATOMIC_SEQ_CST);
}

/* Stops the core like crt0 does after main (rustc has no inline assembly
 * for this target); klausscc reports R12 as the exit code. */
void c_halt(long rc) {
    register long r __asm__("r12") = rc;
    __asm__ volatile("halt" : : "r"(r));
    for (;;) { }
}

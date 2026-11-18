#!/usr/bin/env python3
"""ABI-level check of the libc crate's function signatures for klausscpu.

ctest only checks that each Rust `extern "C" fn` resolves to the same symbol
as the C declaration. This checks the signatures, at the level that matters
for the calling convention: for the return value and every parameter, the
class (integer / floating / pointer / aggregate / void), the size, and for
integers the signedness (sub-word arguments are extended by the caller).
Variadic-ness and the number of parameters are compared too.

Rust side: the libc crate macro-expanded for the target (expanded.rs, made by
run-fnsig.sh), parsed here; type aliases are resolved by name.
C side: a C++ file that includes the libc headers and turns each function's
type into numbers with templates (decltype + type traits), compiled with
clang++ for the target to assembly, whose .rodata is read back.

The function set is the one ctest tests (ctest_output.c), so functions that
ctest skips by name are skipped here too.

Usage: fnsig.py expanded.rs ctest_output.c workdir clang++ sysroot
"""
import re
import subprocess
import sys
from pathlib import Path

expanded, ctest_c, work, clangxx, sysroot = sys.argv[1:6]
work = Path(work)
src = Path(expanded).read_text()

# ---------------------------------------------------------------- Rust side
# Encoding shared with the C++ side: kind<<16 | size<<8 | extra
# kind: 1 int (extra = signed), 2 float, 3 pointer, 4 aggregate (extra =
# align), 5 void.
INT, FLT, PTR, AGG, VOID = 1, 2, 3, 4, 5


def enc(kind, size=0, extra=0):
    return kind << 16 | size << 8 | extra


PRIM = {
    "i8": enc(INT, 1, 1), "u8": enc(INT, 1, 0),
    "i16": enc(INT, 2, 1), "u16": enc(INT, 2, 0),
    "i32": enc(INT, 4, 1), "u32": enc(INT, 4, 0),
    "i64": enc(INT, 8, 1), "u64": enc(INT, 8, 0),
    "isize": enc(INT, 8, 1), "usize": enc(INT, 8, 0),
    "bool": enc(INT, 1, 0),
    "f32": enc(FLT, 4), "f64": enc(FLT, 8),
    # core::ffi on klausscpu (checked separately by prims.sh)
    "c_char": enc(INT, 1, 1), "c_schar": enc(INT, 1, 1), "c_uchar": enc(INT, 1, 0),
    "c_short": enc(INT, 2, 1), "c_ushort": enc(INT, 2, 0),
    "c_int": enc(INT, 4, 1), "c_uint": enc(INT, 4, 0),
    "c_long": enc(INT, 8, 1), "c_ulong": enc(INT, 8, 0),
    "c_longlong": enc(INT, 8, 1), "c_ulonglong": enc(INT, 8, 0),
    "c_float": enc(FLT, 4), "c_double": enc(FLT, 8),
    "c_void": enc(VOID), "()": enc(VOID), "!": enc(VOID),
}

# type aliases: name -> list of right-hand sides (a name can be defined in
# several private modules)
aliases = {}
for m in re.finditer(r"\btype\s+(\w+)\s*=\s*([^;]+);", src):
    aliases.setdefault(m.group(1), []).append(m.group(2).strip())
structs = set(re.findall(r"\b(?:struct|union)\s+(\w+)", src))
enums = set(re.findall(r"\benum\s+(\w+)", src))

TOK = re.compile(r'\s*(::|->|\.\.\.|"[^"]*"|[A-Za-z_]\w*|\d+|[<>()\[\];,*&!:=])')


def tokenize(s):
    out, pos = [], 0
    while pos < len(s):
        m = TOK.match(s, pos)
        if not m:
            if s[pos:].strip() == "":
                break
            raise ValueError("tokenize: %r" % s[pos:pos + 30])
        out.append(m.group(1))
        pos = m.end()
    return out


class P:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def next(self):
        self.i += 1
        return self.t[self.i - 1]

    def expect(self, x):
        y = self.next()
        if y != x:
            raise ValueError("expected %s got %s in %s" % (x, y, " ".join(self.t)))

    def ty(self):
        t = self.peek()
        if t == "*":
            self.next()
            self.next()  # const / mut
            self.ty()
            return ("ptr",)
        if t == "&":
            self.next()
            if self.peek() == "mut":
                self.next()
            self.ty()
            return ("ptr",)
        if t == "(":
            self.next()
            self.expect(")")
            return ("name", "()")
        if t == "!":
            self.next()
            return ("name", "!")
        if t == "[":
            self.next()
            self.ty()
            self.expect(";")
            while self.next() != "]":
                pass
            return ("array",)
        if t in ("unsafe", "extern", "fn"):
            self.fnptr()
            return ("ptr",)
        # path
        segs = [self.next()]
        if segs[0] == "::":
            segs = [self.next()]
        while self.peek() == "::":
            self.next()
            segs.append(self.next())
        if self.peek() == "<":
            self.next()
            args = [self.ty()]
            while self.peek() == ",":
                self.next()
                args.append(self.ty())
            self.expect(">")
            if segs[-1] == "Option" and args[0] == ("ptr",):
                return ("ptr",)
            return ("generic", segs[-1], args)
        return ("name", segs[-1])

    def fnptr(self):
        if self.peek() == "unsafe":
            self.next()
        if self.peek() == "extern":
            self.next()
            if self.peek().startswith('"'):
                self.next()
        self.expect("fn")
        self.expect("(")
        depth = 1
        while depth:
            x = self.next()
            depth += {"(": 1, ")": -1}.get(x, 0)
        if self.peek() == "->":
            self.next()
            self.ty()


def resolve(t, seen=()):
    """Rust type tree -> encoding, or a string describing why not."""
    if t[0] == "ptr":
        return enc(PTR, 8)
    if t[0] == "array":
        return "array by value"
    if t[0] == "generic":
        return "generic %s" % t[1]
    name = t[1]
    if name in PRIM:
        return PRIM[name]
    if name in seen:
        return "alias loop %s" % name
    if name in aliases:
        rs = set()
        for rhs in aliases[name]:
            try:
                rs.add(resolve(P(tokenize(rhs)).ty(), seen + (name,)))
            except ValueError as e:
                rs.add("parse %s" % e)
        if len(rs) == 1:
            return rs.pop()
        ints = [r for r in rs if isinstance(r, int)]
        if len(set(ints)) == 1:
            return ints[0]
        return "ambiguous alias %s: %s" % (name, rs)
    if name in structs:
        return ("agg", name)
    if name in enums:
        return "rust enum %s" % name
    return "unknown type %s" % name


# extern blocks
fns = {}
for m in re.finditer(r'extern\s+"C"\s*\{', src):
    i, depth = m.end(), 1
    while depth:
        c = src[i]
        depth += {"{": 1, "}": -1}.get(c, 0)
        i += 1
    body = src[m.end():i - 1]
    for item in body.split(";"):
        item = re.sub(r"#\[[^\]]*\]", " ", item)
        fm = re.search(r"\bpub\s+(?:safe\s+|unsafe\s+)?fn\s+(\w+)\s*\(", item)
        if not fm:
            continue
        name = fm.group(1)
        # params
        j, depth = fm.end(), 1
        while depth:
            depth += {"(": 1, ")": -1}.get(item[j], 0)
            j += 1
        params = item[fm.end():j - 1]
        rest = item[j:].strip()
        ret = rest[2:].strip() if rest.startswith("->") else "()"
        # split params at depth 0
        parts, depth, cur = [], 0, ""
        params = params.replace("->", "\x01")  # keep '>' of '->' out of the depth count
        for c in params:
            if c in "(<[":
                depth += 1
            elif c in ")>]":
                depth -= 1
            if c == "," and depth == 0:
                parts.append(cur)
                cur = ""
            else:
                cur += c
        if cur.strip():
            parts.append(cur)
        variadic = False
        args = []
        for p in parts:
            p = p.strip()
            if p == "..." or p.endswith(": ...") or p.endswith("..."):
                variadic = True
                continue
            # name: type  (the name may be a pattern like `_`)
            k = p.index(":")
            args.append(p[k + 1:].strip().replace("\x01", "->"))
        fns.setdefault(name, []).append((args, ret, variadic))

# ---------------------------------------------------------------- C side
ctest_src = Path(ctest_c).read_text()
tested = re.findall(
    r"ctest_foreign_fn__(\w+)\(void\) \{\s*return \(ctest_void_func\)([^;]+);", ctest_src)
# Functions ctest skips by name (execv*, strtol*, *scanf, strerror_r,
# lio_listio, ...) are checked here too: every Rust extern fn not in the
# ctest list, by its own name.
in_ctest = {n for n, _ in tested}
tested += [(n, n) for n in sorted(fns) if n not in in_ctest]
headers = re.findall(r"#include <([^>]+)>", ctest_src)
cxx_headers = [h for h in headers
               if not h.startswith(("linux/", "asm/"))]

MAXN = 24
cpp = ["#define _GNU_SOURCE 1"]
cpp += ["#include <%s>" % h for h in cxx_headers]
cpp.append(r"""
// clang type-trait builtins; no libc++ (its <stdlib.h> etc. add overloads)
template<class T> constexpr unsigned d() {
    using U = __remove_cv(T);
    if constexpr (__is_void(U)) return 5u << 16;
    else if constexpr (__is_pointer(U) || __is_same(U, decltype(nullptr)))
        return 3u << 16 | unsigned(sizeof(U)) << 8;
    else if constexpr (__is_enum(U)) return d<__underlying_type(U)>();
    else if constexpr (__is_floating_point(U)) return 2u << 16 | unsigned(sizeof(U)) << 8;
    else if constexpr (__is_integral(U))
        return 1u << 16 | unsigned(sizeof(U)) << 8 | (__is_signed(U) ? 1u : 0u);
    else if constexpr (__is_class(U) || __is_union(U))
        return 4u << 16 | unsigned(sizeof(U)) << 8 | unsigned(alignof(U));
    else return 0xffffffffu;
}
struct Sig { unsigned v[%d]; };
// Deduction from a function pointer also accepts noreturn / noexcept ones.
template<class R, class... A> constexpr Sig mk(R (*)(A...)) {
    return {{sizeof...(A), 0, d<R>(), d<A>()...}}; }
template<class R, class... A> constexpr Sig mk(R (*)(A..., ...)) {
    return {{sizeof...(A), 1, d<R>(), d<A>()...}}; }
""" % MAXN)
idx = []
for k, (name, cval) in enumerate(tested):
    idx.append(name)
    cpp.append('extern "C" const Sig ks_sig_%d = mk(&%s);'
               % (k, cval.strip()))
work.mkdir(parents=True, exist_ok=True)
(work / "fnsig.cpp").write_text("\n".join(cpp) + "\n")
asm = work / "fnsig.s"
r = subprocess.run([clangxx, "--target=klausscpu-unknown-linux-musl", "--sysroot=" + sysroot,
                    "-std=c++17", "-nostdinc++", "-S", "-O1", "-w", "-o", str(asm), str(work / "fnsig.cpp")],
                   capture_output=True, text=True)
if r.returncode:
    sys.stderr.write(r.stderr[-6000:])
    sys.exit("fnsig: C++ compile failed")

csig = {}
cur = None
for line in asm.read_text().splitlines():
    m = re.match(r"^ks_sig_(\d+):", line)
    if m:
        cur = int(m.group(1))
        csig[cur] = []
        continue
    if cur is not None:
        m = re.match(r"^\s*\.(?:long|word|4byte)\s+(-?\d+)", line)
        if m:
            csig[cur].append(int(m.group(1)) & 0xffffffff)
            continue
        m = re.match(r"^\s*\.zero\s+(\d+)", line)
        if m:
            csig[cur] += [0] * (int(m.group(1)) // 4)
            continue
        if line.strip() and not line.strip().startswith((".", "#")):
            cur = None
        elif line.strip().startswith(".size"):
            cur = None


def show(e):
    if not isinstance(e, int):
        return str(e)
    kind, size, extra = e >> 16, (e >> 8) & 0xff, e & 0xff
    if kind == INT:
        return ("i" if extra else "u") + str(size * 8)
    if kind == FLT:
        return "f%d" % (size * 8)
    if kind == PTR:
        return "ptr%d" % (size * 8)
    if kind == AGG:
        return "agg(size %d, align %d)" % (size, extra)
    if kind == VOID:
        return "void"
    return "?%x" % e


def same(rust, c):
    if isinstance(rust, tuple) and rust[0] == "agg":
        return (c >> 16) == AGG
    return rust == c


def int_vs_ptr(rust, c):
    """An 8-byte integer on one side and a pointer on the other: the same
    registers and size. musl's C++ mode makes pthread_t an unsigned long
    (C: struct __pthread *), and libc's sighandler_t is a size_t."""
    if not isinstance(rust, int):
        return False
    a, b = sorted([rust >> 16, c >> 16])
    return (a, b) == (INT, PTR) and (rust >> 8) & 0xff == 8 and (c >> 8) & 0xff == 8


ok = bad = unsure = 0
lines = []
notes = []
for k, name in enumerate(idx):
    if name not in fns:
        lines.append("UNSURE %s: not found in the expanded Rust source" % name)
        unsure += 1
        continue
    if len(fns[name]) > 1 and len({(tuple(a), r, v) for a, r, v in fns[name]}) > 1:
        lines.append("UNSURE %s: declared more than once with different types" % name)
        unsure += 1
        continue
    args, ret, variadic = fns[name][0]
    c = csig.get(k)
    if not c:
        lines.append("UNSURE %s: no C data" % name)
        unsure += 1
        continue
    cn, cvar, cret, cargs = c[0], c[1], c[2], c[3:3 + c[0]]
    try:
        rret = resolve(P(tokenize(ret)).ty())
        rargs = [resolve(P(tokenize(a)).ty()) for a in args]
    except ValueError as e:
        lines.append("UNSURE %s: %s" % (name, e))
        unsure += 1
        continue
    probs = []
    if len(rargs) != cn:
        probs.append("%d parameters in Rust, %d in C" % (len(rargs), cn))
    if bool(cvar) != variadic:
        probs.append("variadic: Rust %s, C %s" % (variadic, bool(cvar)))
    pairs = [("return", rret, cret)] + [("arg %d" % (i + 1), r, cc)
                                         for i, (r, cc) in enumerate(zip(rargs, cargs))]
    for what, r, cc in pairs:
        if not isinstance(r, (int, tuple)):
            lines.append("UNSURE %s %s: %s (C %s)" % (name, what, r, show(cc)))
            unsure += 1
            continue
        if not same(r, cc) and int_vs_ptr(r, cc):
            notes.append("%s %s: Rust %s, C %s" % (name, what, show(r), show(cc)))
        elif not same(r, cc):
            probs.append("%s: Rust %s, C %s" % (what, show(r) if isinstance(r, int) else "agg " + r[1],
                                                 show(cc)))
    if probs:
        bad += 1
        lines.append("BAD %s: %s" % (name, "; ".join(probs)))
    else:
        ok += 1
print("\n".join(lines))
for n in notes:
    print("NOTE (8-byte integer vs pointer) " + n)
print("fnsig: %d functions, %d match, %d mismatch, %d unsure" % (len(idx), ok, bad, unsure))
sys.exit(1 if bad else 0)

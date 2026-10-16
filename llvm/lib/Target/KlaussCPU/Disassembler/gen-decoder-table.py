#!/usr/bin/env python3
"""Generate KlaussCPUGenDecoderTable.inc from KlaussCPUMCCodeEmitter.cpp.

The encoder is hand-written (ISA encoding v2/v3, no -gen-emitter), so the
decoder table is derived from the encoder's own `case` lines instead of the
.td `Inst{}` fields, which are only documentary.  Re-run after adding or
changing an instruction encoding:

    python3 gen-decoder-table.py ../MCTargetDesc/KlaussCPUMCCodeEmitter.cpp \
        > KlaussCPUGenDecoderTable.inc

Each entry is {opcode, words, template, fixed-bit mask, fields}; a word
decodes to an entry when (word0 & mask) == template.  Overlapping entries are
an error unless one entry is a special case of the other (it is tried first)
or the pair is listed in PREFERRED (identical encodings); the decoder takes
the first matching entry.
"""
import re
import sys

RD, RS1, RS2 = "RD", "RS1", "RS2"
FIELD_MASK = {
    "RD": 0x00000F00, "RS1": 0x000000F0, "RS2": 0x0000000F,
    "N6": 0x001F8000, "ENTER22": 0x003FFFFF, "IMM4": 0x0000000F,
    "FBRCC": 0x03C00000, "PC13": 0x001FFF00, "PC18": 0x0003FFFF,
    "SH_S": 0x000FF000, "SH_Z": 0x000FF000, "SH_M0": 0x000FF000,
    "SH_M1": 0x000FF000, "SH_M2": 0x000FF000, "SH_M3": 0x000FF000,
    "LEGACY_R": 0x0000000F,
    # word-1 / word-2 fields do not touch word 0
    "W1S": 0, "W1U": 0, "W1PC": 0, "W2S": 0,
}

# Identical encodings: the first name is the one the decoder produces.
PREFERRED = [("SHLR1", "SHLAR")]

# Short-form immediate kinds, from KlaussCPUMCCompress.cpp's table.
SHORT_ZEXT = {"ADDV_SH", "MINUSV_SH", "ANDV_SH", "ORV_SH", "XORV_SH"}
# 2-word forms whose imm32 the CPU zero-extends (same ops, plus PUSHV).
LONG_ZEXT = {"ADDV", "MINUSV", "ANDV", "ORV", "XORV", "PUSHV"}

FIELD_CALL = re.compile(r"f(Rd|Rs1|Rs2)\s*\(MI,\s*(\d+)\)")


def reg_fields(expr):
    names = {"Rd": RD, "Rs1": RS1, "Rs2": RS2}
    return [(names[m.group(1)], int(m.group(2))) for m in FIELD_CALL.finditer(expr)]


def first_hex(expr):
    return int(re.search(r"0x([0-9A-Fa-f]+)u", expr).group(1), 16)


def parse(src):
    entries = []
    # Single-line `case KlaussCPU::X: return ...;` and the multi-line
    # pack2Sym cases (SETR/LEAPC/MEMREADR/MEMSETR) joined onto one line.
    text = re.sub(r"return\s+pack2Sym\(\s*\n\s*", "return pack2Sym(", src)
    text = re.sub(r",\s*\n\s*(KlaussCPU::FK_)", r", \1", text)
    case_re = re.compile(
        r"case KlaussCPU::(\w+):\s*(?://[^\n]*\n\s*)?return\s+(.*?);")
    for m in case_re.finditer(text):
        name, expr = m.group(1), m.group(2)
        if "0x0000" in expr[:12] and re.match(r"(pack2\()?0x0000", expr):
            continue  # retired v1 (LEN=00) opcodes: not decoded
        if expr.startswith("shortForm("):
            inner = expr[len("shortForm("):-1]
            tpl = (first_hex(inner) & 0x3FFFFFFF) | 0x40000000
            regs = reg_fields(inner)
            mem = re.search(r"memOff\(MI,\s*(\d+),\s*(\d)\)", inner)
            if mem:
                imm = ("SH_M" + mem.group(2), int(mem.group(1)))
            else:
                op = int(re.search(r"getImm32\(MI,\s*(\d+)\)\s*\)?$",
                                   inner).group(1))
                imm = ("SH_Z" if name in SHORT_ZEXT else "SH_S", op)
            entries.append((name, 1, tpl, regs + [imm]))
        elif expr.startswith("pack2Sym("):
            tpl = first_hex(expr)
            regs = reg_fields(expr)
            op = int(re.search(r"MI,\s*(\d+),\s*KlaussCPU::FK", expr).group(1))
            pcrel = "PCREL32" in expr
            kind = "W1PC" if pcrel else ("W1U" if not regs or name.startswith("MEM") else "W1S")
            entries.append((name, 2, tpl, regs + [(kind, op)]))
        elif expr.startswith("pack2("):
            tpl = first_hex(expr)
            regs = reg_fields(expr)
            op = int(re.search(r"getImm32\(MI,\s*(\d+)\)\)$", expr).group(1))
            kind = "W1U" if name in LONG_ZEXT else "W1S"
            entries.append((name, 2, tpl, regs + [(kind, op)]))
        else:
            tpl = first_hex(expr)
            fields = reg_fields(expr)
            n6 = re.search(r"getImm32\(MI,\s*(\d+)\)\s*&\s*0x3F\)\s*<<\s*15", expr)
            if n6:
                fields.append(("N6", int(n6.group(1))))
            enter = re.search(r"getImm32\(MI,\s*(\d+)\)\s*&\s*0x3FFFFFu", expr)
            if enter:
                fields.append(("ENTER22", int(enter.group(1))))
            entries.append((name, 1, tpl, fields))
    # Hand-written special cases (see encodeInstruction / encode32).
    entries.append(("SETR64", 3, 0xCBC00000, [(RD, 0), ("W1S", 1), ("W2S", 2)]))
    entries.append(("PUSHV64", 3, 0xE4400000, [("W1S", 0), ("W2S", 1)]))
    entries.append(("FBR_RR", 1, 0x74000000,
                    [(RS1, 0), (RS2, 1), ("FBRCC", 2), ("PC13", 3)]))
    entries.append(("FBR_RI", 1, 0x74200000,
                    [(RS1, 0), ("IMM4", 1), ("FBRCC", 2), ("PC13", 3)]))
    return entries


SHORT_BRANCHES = [  # (short opcode, absolute template) from KlaussCPUMCCompress.cpp
    ("JMP_SH", 0xA0000000), ("JMPZ_SH", 0xA0080000), ("JMPNZ_SH", 0xA00C0000),
    ("JMPC_SH", 0xA0100000), ("JMPNC_SH", 0xA0140000), ("JMPO_SH", 0xA0180000),
    ("JMPNO_SH", 0xA01C0000), ("JMPS_SH", 0xA0200000), ("JMPNS_SH", 0xA0240000),
    ("JMPLT_SH", 0xA0280000), ("JMPGE_SH", 0xA02C0000), ("JMPLE_SH", 0xA0300000),
    ("JMPGT_SH", 0xA0340000), ("JMPULT_SH", 0xA0380000), ("JMPUGE_SH", 0xA03C0000),
    ("JMPULE_SH", 0xA0400000), ("JMPUGT_SH", 0xA0440000), ("JMPE_SH", 0xA0480000),
    ("JMPNE_SH", 0xA04C0000),
]


def check_short_branches(src):
    # Guard against drift: the generator's list must match the source table.
    for name, tpl in SHORT_BRANCHES:
        pat = r"\{KlaussCPU::%s, KlaussCPU::\w+, [^,]+, 0x%08Xu\}" % (name, tpl)
        if not re.search(pat, src):
            sys.exit("short branch table drifted: %s" % name)


def mask_of(words, fields):
    m = 0xFFFFFFFF
    for kind, _ in fields:
        m &= ~FIELD_MASK[kind]
    return m


def main():
    emitter = open(sys.argv[1]).read()
    compress = open(sys.argv[1].replace("KlaussCPUMCCodeEmitter.cpp",
                                        "KlaussCPUMCCompress.cpp")).read()
    check_short_branches(compress)
    entries = parse(emitter)
    for name, tpl in SHORT_BRANCHES:
        entries.append((name, 1, 0x61000000 | (tpl & 0x00FC0000), [("PC18", 0)]))

    names = [e[0] for e in entries]
    dups = {n for n in names if names.count(n) > 1}
    if dups:
        sys.exit("duplicate entries: %s" % sorted(dups))

    rows = []
    for name, words, tpl, fields in entries:
        mask = mask_of(words, fields)
        if tpl & ~mask:
            sys.exit("%s: template 0x%08X has bits inside operand fields" % (name, tpl))
        rows.append((name, words, tpl, mask, fields))

    # Order: more specific masks first (e.g. ROLR1 is ROLV with N=1, so it
    # must be tried before ROLV), identical-encoding aliases after their
    # preferred spelling, otherwise emitter order.
    pref = {b: a for a, b in PREFERRED}
    rows.sort(key=lambda r: (-bin(r[3]).count("1"), 1 if r[0] in pref else 0))

    allowed = {frozenset(p) for p in PREFERRED}
    for i, a in enumerate(rows):
        for b in rows[i + 1:]:
            if a[1] != b[1] or ((a[2] ^ b[2]) & a[3] & b[3]) != 0:
                continue  # different length, or no word matches both
            if frozenset((a[0], b[0])) in allowed:
                continue
            if a[3] != b[3] and (b[3] & ~a[3]) == 0:
                continue  # a is a special case of b and is tried first
            sys.exit("ambiguous encodings: %s / %s" % (a[0], b[0]))

    print("// Generated by gen-decoder-table.py from KlaussCPUMCCodeEmitter.cpp;")
    print("// do not edit.  {Opcode, Words, Template, Mask, NumFields, Fields}")
    for name, words, tpl, mask, fields in rows:
        fs = ", ".join("{F_%s, %d}" % (k, op) for k, op in fields)
        print("{KlaussCPU::%s, %d, 0x%08Xu, 0x%08Xu, %d, {%s}}," %
              (name, words, tpl, mask, len(fields), fs))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate KlaussCPUGenDecoderTable.inc from the .td instruction encodings.

The decoder is hand-written (no -gen-disassembler), but its table comes from
the same `Inst` fields that drive the generated MC code emitter, dumped by
llvm-tblgen.  Re-run after adding or changing an instruction encoding:

    <build>/bin/llvm-tblgen --dump-json -I <llvm>/include -I .. \\
        ../KlaussCPU.td -o KlaussCPU.json
    python3 gen-decoder-table.py KlaussCPU.json > KlaussCPUGenDecoderTable.inc

Each entry is {opcode, words, template, fixed-bit mask, fields}; a word
decodes to an entry when (word0 & mask) == template.  Overlapping entries are
an error unless one entry is a special case of the other (it is tried first)
or the pair is listed in PREFERRED (identical encodings); the decoder takes
the first matching entry.
"""
import json
import sys

FIELD_MASK = {
    "RD": 0x00000F00, "RS1": 0x000000F0, "RS2": 0x0000000F,
    "N6": 0x001F8000, "ENTER22": 0x003FFFFF, "IMM4": 0x0000000F,
    "FBRCC": 0x03C00000, "PC13": 0x001FFF00, "PC18": 0x0003FFFF,
    "SH_S": 0x000FF000, "SH_Z": 0x000FF000, "SH_M0": 0x000FF000,
    "SH_M1": 0x000FF000, "SH_M2": 0x000FF000, "SH_M3": 0x000FF000,
    # word-1 / word-2 fields do not touch word 0
    "W1S": 0, "W1U": 0, "W1PC": 0, "W2S": 0,
}

# Identical encodings: the first name is the one the decoder produces.
PREFERRED = [("SHLR1", "SHLAR")]

# Immediates the CPU zero-extends (printed unsigned): the class 2 ALU forms
# with SGN=0 (ISA_ENCODING_V2_MAP §2) and their short twins, plus PUSHV.
SHORT_ZEXT = {"ADDV_SH", "MINUSV_SH", "ANDV_SH", "ORV_SH", "XORV_SH"}
LONG_ZEXT = {"ADDV", "MINUSV", "ANDV", "ORV", "XORV", "PUSHV"}

# Operand types whose word-1 value is a PC-relative displacement, and those
# that may be a symbol's absolute address.
PCREL_TYPES = {"PCRelTarget32", "PCRelCallTarget32", "pcrel_simm32"}
ABS_TYPES = {"BrTarget32", "CallTarget32", "simm32_abs"}

# Register fields, in the order the decoder fills them.
REG_FIELDS = {11: "RD", 7: "RS1", 3: "RS2"}


def operands(rec):
    """[(name, type)] in MCInst operand order (outs, then ins)."""
    ops = []
    for dag in ("OutOperandList", "InOperandList"):
        for arg, name in rec[dag]["args"]:
            ops.append((name, arg["def"]))
    return ops


def field_kind(name, var, ty, hi, lo, low_var_bit, has_regs):
    if hi < 32:
        if ty == "GPR" and hi in REG_FIELDS and hi - lo == 3:
            return REG_FIELDS[hi]
        if (hi, lo) == (3, 0):
            return "IMM4"
        if (hi, lo) == (20, 15):
            return "N6"
        if (hi, lo) == (21, 0):
            return "ENTER22"
        if (hi, lo) == (20, 8):
            return "PC13"
        if (hi, lo) == (17, 0):
            return "PC18"
        if (hi, lo) == (19, 12):
            if var == "offset":
                return "SH_M%d" % low_var_bit
            return "SH_Z" if name in SHORT_ZEXT else "SH_S"
    elif (hi, lo) == (63, 32):
        if ty in PCREL_TYPES:
            return "W1PC"
        if ty in ABS_TYPES:
            return "W1U" if not has_regs or name.startswith("MEM") else "W1S"
        return "W1U" if name in LONG_ZEXT else "W1S"
    elif (hi, lo) == (95, 64):
        return "W2S"
    sys.exit("%s: no decoder field kind for %s at Inst{%d-%d}"
             % (name, var, hi, lo))


def entry(name, rec):
    bits = rec["Inst"]  # LSB first
    words = rec["Size"] // 4
    tpl = 0
    spans = {}  # var -> [inst bit positions]
    for pos, b in enumerate(bits[:32 * words]):
        if b == 1:
            if pos >= 32:
                sys.exit("%s: constant bit %d outside word 0" % (name, pos))
            tpl |= 1 << pos
        elif isinstance(b, dict):
            spans.setdefault(b["var"], []).append((pos, b["index"]))
        elif b != 0:
            sys.exit("%s: unset Inst bit %d" % (name, pos))
    ops = operands(rec)
    index = {n: i for i, (n, _) in enumerate(ops)}
    types = dict(ops)
    regs, others = [], []
    has_regs = any(types[v] == "GPR" for v in spans)
    for var, ps in spans.items():
        # The FBR condition is split: PRED [25:23], INV [22].
        if var == "cc":
            others.append(("FBRCC", index[var]))
            continue
        hi, lo = max(p for p, _ in ps), min(p for p, _ in ps)
        if len(ps) != hi - lo + 1:
            sys.exit("%s: %s is not one contiguous field" % (name, var))
        low_var_bit = min(i for _, i in ps)
        kind = field_kind(name, var, types[var], hi, lo, low_var_bit, has_regs)
        (regs if kind in REG_FIELDS.values() else others).append(
            (kind, index[var]))
    order = list(REG_FIELDS.values())
    regs.sort(key=lambda f: order.index(f[0]))
    others.sort(key=lambda f: f[1])
    return (name, words, tpl, regs + others)


def mask_of(fields):
    m = 0xFFFFFFFF
    for kind, _ in fields:
        m &= ~FIELD_MASK[kind]
    return m


def main():
    records = json.load(open(sys.argv[1]))
    entries = []
    for name in records["!instanceof"]["KCInst"]:
        rec = records[name]
        if rec["isRetiredV1"]:
            continue  # LEN=00 words trap on the CPU: not decoded
        entries.append(entry(name, rec))

    rows = []
    for name, words, tpl, fields in entries:
        mask = mask_of(fields)
        if tpl & ~mask:
            sys.exit("%s: template 0x%08X has bits inside operand fields" % (name, tpl))
        rows.append((name, words, tpl, mask, fields))

    # Order: more specific masks first (e.g. ROLR1 is ROLV with N=1, so it
    # must be tried before ROLV), identical-encoding aliases after their
    # preferred spelling, otherwise by name.
    pref = {b: a for a, b in PREFERRED}
    rows.sort(key=lambda r: (-bin(r[3]).count("1"), 1 if r[0] in pref else 0,
                             r[0]))

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

    print("// Generated by gen-decoder-table.py from the KlaussCPUInstrInfo.td")
    print("// encodings; do not edit.  {Opcode, Words, Template, Mask, NumFields, Fields}")
    for name, words, tpl, mask, fields in rows:
        fs = ", ".join("{F_%s, %d}" % (k, op) for k, op in fields)
        print("{KlaussCPU::%s, %d, 0x%08Xu, 0x%08Xu, %d, {%s}}," %
              (name, words, tpl, mask, len(fields), fs))


if __name__ == "__main__":
    main()

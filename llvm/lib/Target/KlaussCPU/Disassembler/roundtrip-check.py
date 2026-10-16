#!/usr/bin/env python3
"""Round-trip check for the KlaussCPU disassembler.

For every decode-table entry, build an instruction word with distinct field
values, disassemble it with llvm-mc, reassemble the text with llvm-mc and
compare the bytes (a numeric short-branch target may reassemble to the
equivalent 2-word form, which must disassemble back to the same text).

    python3 roundtrip-check.py <build>/bin KlaussCPUGenDecoderTable.inc OUT.txt

OUT.txt is the body of test/MC/Disassembler/KlaussCPU/all-instructions.txt.
"""
import re
import subprocess
import sys

BIN = sys.argv[1]
TABLE = sys.argv[2]
OUT = sys.argv[3]

FIELD = {  # kind -> (bits to OR into word0, word1, word2)
    "RD": (0x300, 0, 0), "RS1": (0x30, 0, 0), "RS2": (0x7, 0, 0),
    "N6": (13 << 15, 0, 0), "ENTER22": (0x2A, 0, 0), "IMM4": (0xD, 0, 0),
    "FBRCC": (0x5 << 23 | 1 << 22, 0, 0), "PC13": (0x1FFC << 8, 0, 0),
    "PC18": (0x3FFF0, 0, 0), "SH_S": (0xF6 << 12, 0, 0),
    "SH_Z": (0xC8 << 12, 0, 0), "SH_M0": (0xFC << 12, 0, 0),
    "SH_M1": (0xFC << 12, 0, 0), "SH_M2": (0xFC << 12, 0, 0),
    "SH_M3": (0xFC << 12, 0, 0), "W1S": (0, 0xFFFFFF85, 0),
    "W1U": (0, 0xF0010020, 0), "W1PC": (0, 0xFFFFFFE0, 0),
    "W2S": (0, 0, 0x12345678),
}

entries = []
for line in open(TABLE):
    m = re.match(r"\{KlaussCPU::(\w+), (\d), 0x([0-9A-F]+)u, 0x([0-9A-F]+)u, \d, \{(.*)\}\},", line)
    if not m:
        continue
    name, words, tpl = m.group(1), int(m.group(2)), int(m.group(3), 16)
    kinds = re.findall(r"\{F_(\w+), \d\}", m.group(5))
    w = [tpl, 0, 0]
    for k in kinds:
        for i in range(3):
            w[i] |= FIELD[k][i]
    entries.append((name, words, w[:words]))


def hexbytes(ws):
    return " ".join("0x%02x" % ((x >> (8 * b)) & 0xFF) for x in ws for b in range(4))


def run(args, inp):
    return subprocess.run(args, input=inp, capture_output=True, text=True)


bad = 0
seen = set()
tests = []
for name, words, ws in entries:
    hb = hexbytes(ws)
    dis = run([BIN + "/llvm-mc", "-triple=klausscpu", "--disassemble"], hb + "\n")
    text = [l.strip() for l in dis.stdout.splitlines() if l.strip() and not l.strip().startswith(".text")]
    if dis.returncode or len(text) != 1 or "warning" in dis.stderr:
        print("DECODE FAIL %s %s: %s %s" % (name, hb, text, dis.stderr.strip()))
        bad += 1
        continue
    asm = run([BIN + "/llvm-mc", "-triple=klausscpu", "-show-encoding",
               "-klausscpu-short-forms=false"], text[0] + "\n")
    enc = re.search(r"encoding: \[([^\]]*)\]", asm.stdout)
    got = enc.group(1).replace(",", " ") if enc else None
    if got != hb:
        # Short forms are only chosen by the assembler for symbolic/fitting
        # operands; accept a different but equivalent long encoding, checked
        # by disassembling it back to the same text.
        asm2 = run([BIN + "/llvm-mc", "-triple=klausscpu", "-show-encoding"], text[0] + "\n")
        enc2 = re.search(r"encoding: \[([^\]]*)\]", asm2.stdout)
        got2 = enc2.group(1).replace(",", " ") if enc2 else None
        if got2 != hb and got2:
            # Equivalent re-encoding (e.g. a numeric short-branch target
            # reassembles to the 2-word form): it must disassemble back to
            # the same text.
            back = run([BIN + "/llvm-mc", "-triple=klausscpu", "--disassemble"], got2 + "\n")
            bt = [l.strip() for l in back.stdout.splitlines() if l.strip() and not l.strip().startswith(".text")]
            if bt == text:
                tests.append((name, hb, text[0]))
                continue
        if got2 != hb:
            print("ROUNDTRIP %s: %s -> '%s' -> %s / %s %s" % (name, hb, text[0], got, got2, asm.stderr.strip()[:200]))
            bad += 1
            continue
    tests.append((name, hb, text[0]))

with open(OUT, "w") as f:
    for name, hb, t in tests:
        f.write("# %s\n# CHECK: %s\n%s\n\n" % (name, t.replace("\t", " "), hb))
print("entries=%d ok=%d bad=%d" % (len(entries), len(tests), bad))
sys.exit(1 if bad else 0)

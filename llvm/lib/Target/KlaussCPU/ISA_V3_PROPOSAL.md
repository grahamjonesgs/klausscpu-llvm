# KlaussCPU ISA additions for compiler-generated code — proposal

For the RTL/ISA side (KlaussCPU core repo). Produced 2026-09-28 from execution
traces of LLVM-compiled programs. Companion to `ISA_ENCODING_V2.md` (the
encoding this builds on) and `PERF_LLVM_HANDOFF.md`.

**One line:** about half of every word the core fetches is a 32-bit immediate
or branch target, and nearly all of those values are tiny. Letting the common
cases fit in one word cuts fetched words by ~30% without changing what any
instruction does.

---

## 1. Recommendation

| # | Proposal | Fetched words | Instructions | RTL cost | Do it? |
|---|---|---|---|---|---|
| **A** | Short 1-word immediate/branch forms | **−30%** | — | decode only | **Yes, first** |
| **C** | `ENTER` / `LEAVE` / `LEAVERET` | −7% (−10% call-heavy) | **−9%** (−13% call-heavy) | small, contained | **Yes, second** |
| **B** | Fused 1-word compare-and-branch | −19% alone, −37% with A | **−10%** | pipeline (compare + redirect in one op) | Decide after A |
| **D** | 32-bit ops (`LDIDX32_S`, 32-bit compare, W ALU ops) | −3% | −5% avg, −13% queens | moderate | Only if `int`-heavy code matters |

Averages over five programs. The ranges per program and the method are in §7.
**These are fetched-word and instruction counts, not cycles.** How many cycles a
fetched word costs depends on how fetch-bound a loop still is after the M7
I-cache (branchy kernels were still ~50% IF_MISS). Measure A on the board with
the `perf_haz` counters before committing to B.

## 2. Where the fetched words go

Average share of fetched words, excluding the `crt0` startup delay loop:

| Group | Words | Size of the immediate / distance actually used |
|---|---|---|
| Conditional branches | 17% | 87% within ±128 instructions; ~100% within 16 bits |
| `CMPRV` (compare with immediate) | 15% | 98% fit in 8 bits |
| `LDIDX*`/`STIDX*` (base + imm32) | 15% | 96% of byte offsets fit in 8 bits |
| Prologue/epilogue (`PUSH`/`GETSP`/`ADDSP`/`SETSP`/`POP`/`RET`) | 10% (13% of instructions) | — |
| 2-word ALU immediates and `SETR` | 9% | 95% fit in 8 bits |
| `SEXTW`/`ZEXTW` | 4% (queens: 13% of instructions) | — |

Average instruction length is 1.4–1.7 words. The most common adjacent pair is
`CMPRV → JMPcc` (10% of all instructions are a compare immediately followed by
its branch).

## 3. Proposal A — short 1-word forms

**Idea.** Today a 1-word (`LEN=01`) encoding in a normally-2-word class (ALU
immediate, compare-immediate, indexed load/store, immediate branch) traps. Define
those combinations as *short forms*: same operation, same attribute bits, with a
small immediate placed in the register and `x` fields the long form leaves
unused. The 2-word forms stay as the fallback, so nothing existing changes.

| Form | Class | Distinguished by | Immediate bits (word 0) | Semantics |
|---|---|---|---|---|
| **A1** short branch / jump / call | 8 | `LEN=01`, `RIND=0`, `REL=1` | `[17:0]` signed, in **words** | target = PC + 4·disp (±512 KB) |
| **A3** short `CMPRV` | 3 | `LEN=01`, `B=0`, `SGN=1` (`CMPRR` has `SGN=0`) | `{x, rd, rs2}` = simm12 | flags ← rs1 − sext(imm12) |
| **A4** short load/store | 6/7 | `LEN=01`, `MODE=01` | `{x, rs2}` = simm8, **scaled by SIZE** | EA = rs1 + (sext(imm8) << SIZE) |
| **A5** short ALU-imm / `SETR` | 2 | `LEN=01` | `{x, rs2}` = imm8 (`SETR`: `{x, rs1, rs2}` = imm12) | as the long form; SGN picks sign/zero extension |

Notes:
- **A1** covers `JMP` (COND=0), all conditions incl. `INV`, and `CALL`
  (`LINK=1`). Short forms are always PC-relative, even in non-PIC code.
- **A4** scaling makes a 64-bit access reach ±1 KB and a 32-bit one ±512 B,
  enough for essentially all frame slots and struct fields. The 64-bit short
  load/store should follow the force-aligned `A=1` behaviour of
  `LDIDX64`/`STIDX64`, as today.
- Unused fields must still be zero and still trap, exactly as in v2.

**Estimated saving** (fetched words):

| | crypto | queens | bst | expr | test_64bit | avg |
|---|---|---|---|---|---|---|
| A1 short conditional branch | 6.7% | 11.1% | 8.5% | 8.6% | 8.0% | 8.6% |
| A1 short `JMPREL`/`CALLREL` | 1.3% | 1.0% | 3.3% | 3.3% | 3.0% | 2.4% |
| A3 short `CMPRV` | 6.3% | 7.9% | 7.1% | 8.5% | 7.9% | 7.5% |
| A4 short load/store | 5.1% | 3.8% | 10.5% | 8.7% | 8.9% | 7.4% |
| A5 short ALU / `SETR` | 4.0% | 5.1% | 4.1% | 3.8% | 4.6% | 4.3% |
| **All of A** | **23.5%** | **29.0%** | **33.5%** | **32.7%** | **32.5%** | **30.2%** |

(A4 was measured with *unscaled* 8-bit byte offsets, so scaling only improves it.)

**RTL cost.** Decode only: the immediate is extracted from word 0 instead of
word 1, and the fetch/length logic already handles `LEN=01`. No new ALU,
memory or flag behaviour.

**Compiler work.**
- A3/A4/A5: new instruction definitions + patterns preferring the short form
  when the constant fits, plus encoder cases. Symbolic immediates (`SETR sym`)
  stay long.
- A1: branch relaxation (emit short, fall back to long when out of range) and a
  new 18-bit PC-relative fixup/relocation. Calls to other objects stay long
  unless lld learns to relax them. Also update `klausscc` (golden-model
  emulator + assembler).

Suggested order: A3 + A4 + A5 first (no relocation or relaxation questions),
then A1.

## 4. Proposal C — `ENTER` / `LEAVE` / `LEAVERET`

Every call with a frame runs `PUSH R15; GETSP R15; ADDSP -N` and
`SETSP R15; POP R15; RET` — 6 instructions, 7 words, 13% of all instructions on
call-heavy code.

| Instruction | Class 9 OP (free: 8–15) | Semantics |
|---|---|---|
| `ENTER N` | 8, `LEN=01`, frame size in `[21:0]` scaled by 8 | `push R15; R15 ← SP; SP ← SP − 8·N` |
| `LEAVE` | 9 | `SP ← R15; pop R15` |
| `LEAVERET` | 10 | `LEAVE; RET` |

**Estimated saving:** bst −13.0% instructions / −9.7% words, expr −12.1% /
−9.1%, test_64bit −12.9% / −10.0%, crypto −5.6% / −5.0%, queens −0.7%
(few calls).

**RTL cost.** Each is a fixed short sequence of existing micro-operations (one
stack store or load plus register writes), comparable to `PUSH`/`POP`/`RET`.

**Compiler work.** Small: `emitPrologue`/`emitEpilogue`, plus folding the
return into `LEAVERET`.

*Alternative:* allowing SP as a load/store base register would let leaf
functions drop the frame pointer entirely. That's a bigger compiler change and
touches the SP datapath, so `ENTER`/`LEAVE` is the cheaper win.

## 5. Proposal B — fused compare-and-branch

`if (a < b)` is two instructions today (`CMPRR`/`CMPRV` + `JMPcc`, 3–4 words).
A fused form does both in one instruction and doesn't write the flags.

Suggested encoding, using reserved **class 0xD**, `LEN=01`:
`[25:23]` PRED (as class 3), `[22]` INV, `[21]` IMM (rs2 field is a signed
4-bit immediate instead of a register), `[20:8]` signed 13-bit displacement in
words (±16 KB; covers ~95% of measured branches), `[7:4]` rs1, `[3:0]`
rs2/imm4. Out-of-range cases keep using today's pair.

**Estimated saving** (upper bound, assuming every compare+branch pair fits):

| | crypto | queens | bst | expr | test_64bit | avg |
|---|---|---|---|---|---|---|
| B alone: words | 13.2% | 25.4% | 17.6% | 20.0% | 20.3% | 19.3% |
| B alone: instructions | 6.2% | 12.4% | 9.7% | 11.0% | 10.9% | 10.0% |
| A + B: words | 27.9% | 37.8% | 39.5% | 39.4% | 39.2% | 36.8% |

Unlike A, B also removes an issue slot per branch, so it helps even where
fetch is not the bottleneck.

**RTL cost.** The comparison result must drive the branch redirect in the same
instruction: a compare in EX feeding branch resolution without going through
the flags. This is the largest change here, and it overlaps A1/A3. **Decide
after A is measured.** The distribution of compare immediates still needs a
check: the 4-bit immediate assumes most compares are against small constants
such as 0 and 1.

**Compiler work.** Moderate: select the fused form in `BR_CC` lowering, and
relax it to the pair when out of range. The flag-reuse peephole becomes mostly
unnecessary.

## 6. Proposal D — 32-bit support

C `int` is 32 bits, but there are no 32-bit operations, so the compiler inserts
`ZEXTW`/`SEXTW` before compares, `abs`, division, right shifts and widening.
This is 13% of instructions in queens and 2–5% elsewhere.

| Item | Cost | Value |
|---|---|---|
| **D1** `LDIDX32_S` / `MEMGET32_S` (sign-extending 32-bit load; already listed as a free combination in `ISA_ENCODING_V2.md` §4.7) | trivial (like `LDIDX8_S`/`LDIDX16_S`) | ~0% in these programs on its own, but a prerequisite for D3 |
| **D2** 32-bit compare (class 3, a `W` bit at `[19]`) | small | removes most extensions before compares |
| **D3** `W` ALU ops (`ADDW`/`SUBW`/…, class 1 `W` bit at `[19]`, result sign-extended) | moderate | the RV64 model: `int` kept sign-extended, extensions nearly vanish |

Upper bound for D2+D3: queens −12.8% instructions, test_64bit −5.5%, others
−2–3%. Compiler work for D3 is significant (RV64-style sign-extension
tracking). Worth it only if `int`-heavy loops matter. Using `long` for hot loop
counters avoids the cost today.

## 7. Method and caveats

- Programs: `klausscpu-runtime/baremetal` crypto, queens, bst, expr, test_64bit,
  built `-O1 -fPIC` with this fork after the 2026-09 compiler work
  (`CLAUDE.md` Step 37 and the `int` zero-extension fixes).
- Every executed instruction was traced with `klausscc --emulate --trace -`:
  class and length from word 0; immediates and branch displacements read from
  the ELF; adjacent pairs counted. The `crt0` `_start` delay loop (~30k
  instructions per run) is excluded.
- Savings assume a value uses the short form whenever it fits the stated bits.
  Branch displacements were bucketed at 8/12/16 bits, so the 18-bit (A1) and
  13-bit (B) figures use the 16- and 12-bit buckets (conservative).
- B's numbers are upper bounds. They assume every compare+branch pair fits and
  count only pairs among the 60 most frequent.
- Not measured: cycles. The emulator is not cycle-accurate. Validate on the
  board with `perf_haz` before and after A.

## 8. Considered and not proposed

- **Conditional move / select:** would flatten branchy loops, but selects are
  already fused into one compare+branch by the compiler, and the remaining
  benefit can't be sized from these traces.
- **Jump tables:** measured neutral on this core (the `JMPR` redirect and table
  load cancel the saving). Nothing needed.
- **More callee-saved registers:** recursive code spills because only R4–R7
  are callee-saved. That's an ABI choice, not an ISA change; it can be
  evaluated separately.

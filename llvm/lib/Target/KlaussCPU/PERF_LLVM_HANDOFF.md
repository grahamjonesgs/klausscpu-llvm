# KlaussCPU LLVM performance work — handoff spec

For a Claude session working **in this tree** (`klausscpu-llvm`, where you can
build LLVM and iterate). Produced 2026-07-03 from a full CPU+ISA+LLVM
performance review + an on-silicon baseline. The RTL-side companion doc is
`KlaussCPU/PERFORMANCE_REVIEW.md` (§G reconciles the review against this actual
backend) and `KlaussCPU/POST_P41_BASELINE.md` (the numbers below).

## STATUS — implemented 2026-07-03 (this tree, built + llc-verified)

- **A4 (TTI): DONE, default-on.** `KlaussCPUTargetTransformInfo.{h,cpp}` added
  and registered via `TargetMachine::getTargetTransformInfo`. Disables
  partial/runtime unrolling + unroll-and-jam + loop peeling (keeps full-unroll
  of tiny constant-trip loops). Verified: a runtime-trip reduction loop keeps a
  single body at `-O2` (one `ldidx64`, no remainder). Needs an on-board
  instruction-count read to confirm the win.
- **A3a (flag reuse): DONE, default-OFF behind `-klausscpu-arith-flag-reuse`.**
  Implemented as a **post-RA MI peephole** (`KlaussCPUFlagReuse.cpp`, run in
  `addPreEmitPass`), NOT the DAG-level rewrite the "Two viable implementations"
  section below suggested. **The DAG approach (option 1) is unsound** and was
  reverted: gluing the arithmetic op adjacent to the branch deadlocks the DAG
  list-scheduler whenever the arith result also feeds a loop back-edge (its
  `CopyToReg` can't be placed) — confirmed by a `SUnit::ComputeHeight` overflow
  crash on a countdown loop. The MI peephole rewrites the already-scheduled
  `<arith Rd>; CMPRV Rd,0; JMP{E,NE}` → `<arith Rd>; JMP{Z,NZ}` (arith ∈
  INCR/DECR/ADDR/SUBR); adjacency in the final stream *is* the proof that no
  flag-clobber sits between producer and branch. Verified (llc): fires on
  `fact` (`decr; jmpnz`, the canonical countdown), `x+y==0`, `x-y!=0`, both
  non-PIC (JMPZ/JMPNZ) and PIC (JMPZREL/JMPNZREL); inert by default; no crash
  across every KlaussCPU test × {-O1,-O2,PIC}. **Still needs the on-silicon
  regression + branchy/calls_fib measurement before flipping to `cl::init(true)`
  in `KlaussCPUFlagReuse.cpp`.** Note LSR shifts many counted-loop compares off
  zero (e.g. to `CMPRV -1`) — those don't qualify; the win lands on loops whose
  controlling value is compared to 0 directly (bit/hash/tree/`fact`-shaped).
- Regression test: `llvm/test/CodeGen/KlaussCPU/flag-reuse.ll` (OFF/ON prefixes).
- Not done: A3b (needs the flag model / analyzeCompare), A5, and the RTL-gated
  B2/E3. Pre-existing stale tests unrelated to this work: `branch.ll`,
  `frame.ll`, `globaladdr.ll` (their CHECK lines predate the 2026-06-22
  immediate-ALU `ANDV`/`SETR` patterns; regenerate with
  `utils/update_llc_test_checks.py` when convenient).

### ON-SILICON MEASUREMENT of A3a (2026-07-03, board on `/dev/cu.usbserial-…2621`, klausscc `--monitor`)

**Verdict: leave A3a default-OFF. Correct on silicon, but net-negative on this
suite — the target pattern barely survives to the peephole, and the code-shift
it causes creates alignment noise that outweighs the one win.**

- **Correctness gate PASSED** both ways: `queens`/`test_64bit`/`bst`/`expr`/`crypto`
  = **110/110 pass** with A3a OFF *and* ON. No wrong-flag miscompiles.
- **A3a fires in exactly one hot loop**: `k_mem` (mem_stream), `decr r8; cmprv r8,0;
  jmpe` → `decr r8; jmpzrel`. That's the only hit in `perf_baseline` (30→29
  `cmprv`), plus one cold spot in `crypto`. It does **NOT** fire in `branchy` or
  `calls_fib` (the intended targets) — their loop compares are LSR-shifted /
  canonicalised to `cmpeqr`, so the pattern never appears.
- **Per-kernel cycles (OFF → ON), CPU confirmed deterministic (OFF run1==run2, Δ<100 cyc):**

  | kernel | Δinstr | Δcycles | note |
  |---|---|---|---|
  | alu | 0 | +0.00% | unaffected |
  | mem_stream | **−131072** | **−2.56%** | the one real A3a win |
  | ptr_chase | 0 | **+5.11%** | identical code — pure alignment shift |
  | branchy | 0 | −0.00% | A3a did not fire |
  | calls_fib | 0 | +1.93% | identical code — alignment shift |
  | muldiv | 0 | +5.39% | identical code — alignment shift |

  Net over the suite: instr **−0.29%**, cycles **+1.10% (worse)**. The +5% swings
  on ptr_chase/muldiv are collateral: A3a removed 8 B from `k_mem`, shifting every
  downstream kernel's code off its 16-B fetch-line alignment. Cache-insensitive
  kernels (alu/branchy) are unmoved; cache-sensitive ones swing.
- **Takeaway for A3a:** the win is real but tiny and rare; enabling it by default
  is not worth the alignment collateral. To make A3a pay off you'd need it to
  reach `branchy`/`calls_fib`, which requires attacking the *upstream* cause
  (LSR moving the loop-exit compare off zero) — a bigger change than the peephole.
  A4 (TTI) is unaffected by all this and stays default-on.

## Ground truth: what is ALREADY done — do NOT redo

Verified in this tree 2026-07-03. Earlier planning docs are stale; these are
implemented and correct:

- **Callee-saved split** — R4–R7 callee-saved (`CSR_KlaussCPU`,
  `KlaussCPURegisterInfo.cpp:37`). Not "caller-saved-everything."
- **Signed imm-offset folding into LDIDX/STIDX** — offsets are `simm32`
  (`KlaussCPUInstrInfo.td:269,282`), general `(load/store (add GPR, simm32_imm))`
  fold patterns exist (`td:768,776,798-800,1049-1051`), `eliminateFrameIndex`
  folds negative frame offsets (`KlaussCPURegisterInfo.cpp:89-95`), spills use
  LDIDX64/STIDX64 directly. Negative offsets already work.
- **INCR/DECR peephole** — `(add GPR,1)→INCR`, `(add GPR,-1)→DECR` (`td:752-753`).
- **Branchless min/max/abs** — `SMIN/SMAX/UMIN/UMAX/ABS` all `Legal`
  (`KlaussCPUISelLowering.cpp:155-158,170`), so DAGCombiner forms them before SELECT.
- **Fall-through layout** — `analyzeBranch`/`insertBranch`/`removeBranch` exist,
  MachineBlockPlacement runs.

## The measurement loop (validated 2026-07-03)

Hold the **bitstream fixed** while measuring LLVM changes (an LLVM change moves
instruction counts, so it is the only variable). Board on `/dev/ttyUSB1`.

```
# 1. build this backend:  (your normal ninja/make build of llvm)
# 2. rebuild the test program with the fresh toolchain:
cd <klausscpu-runtime>/baremetal
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin
make clean && make perf_baseline.elf
# 3. flash + capture (Linux klausscc is built at
#    /home/graham/klausscc-linux-target/release/klausscc):
klausscc --input perf_baseline.elf --serial /dev/ttyUSB1 --monitor   # Ctrl-C after the 6 CSV lines
```

Read the 6 `CSV,name,ms,cycles,instr,cpi_milli,...` lines + the per-kernel
`CPI=`, `FASTPATH=`, `cyc: fetch=…` blocks. **Also run the functional regression
suite** (`make queens test_64bit bst expr crypto`, flash each) — a flag-model
bug corrupts control flow silently, so a green regression is the gate.

### Baseline to beat (post-P4.1, 2026-07-03, on-silicon)

| kernel | CPI | fast-path % | fetch % | instr |
|---|---|---|---|---|
| alu | 4.437 | 43.8% | 69.0 | 8,000,022 |
| mem_stream | 8.441 | 60.0% | 41.9 | 1,966,142 |
| ptr_chase | 6.804 | 38.5% | 46.3 | 2,600,021 |
| **branchy** | **4.676** | **14.7%** | **74.8** | 14,687,112 |
| calls_fib | 6.272 | 21.2% | 59.4 | 16,969,566 |
| muldiv | 4.944 | 46.7% | 63.4 | 750,019 |

`branchy` is the primary target for A3: worst fast-path rate, highest fetch
share. A3 should **reduce its instruction count** (fewer compares) and its CPI.

---

## A3 — compare/flag optimization  ★ start here (biggest LLVM win, zero Fmax risk)

### Current state (read before designing)

- **No flag register is modeled.** `CMPRR_I`/`CMPRV_I` carry `hasSideEffects=1`
  (`td:481,637`); the conditional jumps (`JMPE…JMPUGE`, `td:826-839`) have **no**
  `Uses=[FLAGS]`. The compare→branch dependency is expressed as an **SDAG chain**
  in `KlaussCPUISelDAGToDAG.cpp:367-413` (`Select(BR_CC)` emits `CMPRR_I`/`CMPRV_I`
  then the `JMPcc`, glued by chain), *not* by a register. Correctness today relies
  on the compare being emitted immediately before its branch.
- **`BR_CC` always emits a fresh compare** — even when the same values were just
  compared, and even when the branch is `x != 0` and `x` came from an arithmetic
  op that already set the hardware zero flag.
- **`JMPZ`/`JMPNZ` are defined but never generated** (`td:824-825`, `JMPZREL`
  `td:858`). The `BR_CC` switch (`ISelDAGToDAG.cpp:394-407`) maps only to the
  CMPRR-based conditions (`JMPE/JMPNE/JMPLT/…`).

### Critical hardware discipline (get this wrong → silent miscompiles)

The hardware has **two distinct** flags:
- `zero_flag` — set by **arithmetic** (ADD/SUB/ADC/SBC/INCR/DECR/…). Read by **JMPZ/JMPNZ**.
- `equal_flag` — set only by **CMPRR/CMPRV**. Read by **JMPE/JMPNE**.

They are different registers. **Never** emit `JMPE`/`JMPNE` after an arithmetic
op, and **never** emit `JMPZ`/`JMPNZ` after `CMPRR`/`CMPRV`. (See
`CPU_ARCHITECTURE.md` §6.) So arithmetic-flag reuse must route to **JMPZ/JMPNZ**,
never JMPE/JMPNE.

### Two sub-optimizations (ship incrementally, measure each)

**A3a — count-down / arithmetic-flag reuse (the big one).**
Target: `DECR Rn; JMPNZ` replaces `DECR Rn; CMPRV Rn,0; JMPNZ` — deletes a 2-word
compare + 2 execute cycles per loop iteration (branchy, calls_fib, any counted loop).
- Detect a branch whose condition is `(setcc X, 0)` for EQ/NE where `X` is defined
  by a flag-setting arithmetic op (ADD/SUB/INCR/DECR/…) with **no intervening
  flag-clobbering instruction**, and emit **JMPZ/JMPNZ with no compare**.
- Two viable implementations — pick after a spike:
  1. *DAG-level in `Select(BR_CC)`*: when `LHS` is an add/sub-family node and
     `RHS==0` and `CC∈{EQ,NE}`, skip the compare and emit `JMPZ`/`JMPNZ`
     consuming `LHS`'s chain. Lowest-friction, but you must ensure the arithmetic
     op is actually emitted (its result used) and lands adjacent to the branch.
  2. *MI peephole (`optimizeCompareInstr`)*: model a `FLAGS`/`ZFLAG` register,
     mark arithmetic ops `Defs=[ZFLAG]`, `JMPZ/JMPNZ` `Uses=[ZFLAG]`, then in
     `optimizeCompareInstr` delete the compare when a preceding def already
     produced the needed flag. More robust and reusable, but a bigger change:
     you must add the flag register(s) to `KlaussCPURegisterInfo.td` and audit
     that scheduling/regalloc never separates producer from consumer.
- **Recommendation:** prototype (1) behind a flag on `branchy` first to confirm
  the win and the regression stays green; graduate to (2) if you also want A3b
  and robust cross-block reuse.

**A3b — redundant/duplicate compare elimination.**
- `analyzeCompare` + `optimizeCompareInstr` to drop a `CMPRR/CMPRV` when a prior
  instruction already set the equivalent flags with no clobber between.
- CSE identical `CMPRR`s feeding an if/else-if cascade on the same operand pair
  (one compare sets equal/less/ult in one shot → feed `JMPLT` then `JMPE`).
- This needs the flag-register model from A3a-option-2, so do it second.

### Verification
Regression suite green (esp. `queens`, `test_64bit`, `bst` — loop/branch heavy).
`branchy` and `calls_fib` instruction counts must **drop**; CPI must not regress.
Watch the zero/equal discipline — a wrong-flag branch will pass some tests and
fail others nondeterministically-looking; the golden-model `--emulate` path in
klausscc is a good cross-check.

---

## A4 — add a TargetTransformInfo (stop unrolling a fetch-bound core)

There is **no `KlaussCPUTargetTransformInfo`** in this backend, so LLVM uses the
generic cost model — it unrolls and over-inlines on a core where fetch is 59–75%
of cycles and the IFB is one 16-byte line.
- Create `KlaussCPUTargetTransformInfo.{h,cpp}` (subclass `BasicTTIImplBase`),
  register it via `TargetMachine::getTargetTransformInfo`, add to `CMakeLists.txt`.
- Key hooks: `getUnrollingPreferences` (disable runtime unrolling / low
  thresholds), a raised `getInstructionCost` bias so IR-size-growing transforms
  are discouraged, and conservative inline/`getInliningThresholdMultiplier`.
- Measure: `branchy`/`calls_fib`/`alu` instruction counts should hold or drop;
  no CPI regression. This is mechanical but needs a build.

## A5 — leaf link-register convention (lower priority)

No TCO (`LowerCall` sets `IsTailCall=false`, `ISelLowering.cpp:676`); CALL always
saves the return address to the DDR2 stack. For genuinely leaf functions, a
convention that materializes the return address with `LEAPC` into a reg and
returns via `JMPR` avoids the ~5+5-cycle stack round trip. Mostly subsumed once
E3 lands (the dominant per-call cost is the frame-pointer push/pop, not the
return-address push), so do E3 first.

---

## Blocked on RTL — do these only after the CPU RTL adds the opcode

These are the LLVM halves of "both" items. The RTL work is happening in the
`KlaussCPU` repo; wait for the opcode, then:

- **B2 — signed i32 load.** Once RTL ships `LDIDX32_S`: change
  `setLoadExtAction(ISD::SEXTLOAD, MVT::i64, MVT::i32, …)` from `Expand` to
  `Legal` (`ISelLowering.cpp:92-93`) and add the fold pattern (mirror the
  `LDIDX8_S`/`LDIDX16_S` patterns at `td:1044-1051`). Removes a `SEXTW` per
  sign-relevant `int` load.
- **E3 — frame-pointer elimination.** Once RTL ships `LDIDXSP`/`STIDXSP`
  (SP+imm addressing): stop always-reserving R15 and always emitting a frame
  pointer (`KlaussCPURegisterInfo.cpp:62`, `KlaussCPUFrameLowering.cpp:80-108`);
  gate the FP on `hasFP()`, address locals SP-relative for fixed frames. Frees a
  16th allocatable register and deletes PUSH/POP R15 from every function — the
  single biggest register-pressure + per-call win. Highest-value "both" item.

## Suggested order

1. **A3a** (count-down flag reuse) on `branchy` — biggest, cheapest, on the current bitstream.
2. **A4** (TTI) — mechanical, broad.
3. **A3b** (compare CSE) if you built the flag-register model.
4. **A5**, then the RTL-gated **B2** / **E3 LLVM halves** as those opcodes land.

Coordinate with the RTL session on one rule: never measure an LLVM change and a
bitstream change in the same run — attribute one variable at a time.

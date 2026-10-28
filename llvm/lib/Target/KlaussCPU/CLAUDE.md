# KlaussCPU LLVM Backend

Custom LLVM backend for KlaussCPU — a home-designed 64-bit CPU implemented in
Verilog/SystemVerilog on a Digilent Nexys A7 (Artix-7) FPGA.  Built against
LLVM trunk (v23).  Use the **RISC-V backend** as the style reference, not X86 or ARM.

This file describes the backend as it is now.  The ISA itself is defined in
the hardware repo (`klaussCPU/CPU_ARCHITECTURE.md`, `ISA_ENCODING_V2_MAP.md`,
`ISA_V3_IMPL.md`; RTL ground truth `pipeline_core.sv` / `klauss_pkg.sv`).
`HISTORY.md` is the step-by-step development log; many of its statements
describe earlier states of the backend and are superseded by this file.
[`ABI.md`](ABI.md) is the C ABI (calling convention version 2): the rules,
the consumers and the measurements behind the design.

---

## Architecture facts

| Property | Value |
|---|---|
| GPRs | R0–R15, 64-bit; R15 = frame pointer (P), reserved everywhere |
| PC / SP | 32-bit (128 MiB RAM addressable) |
| Memory bus / register file | little-endian |
| DataLayout | `e-m:e-p:64:64-i64:64-i128:128-n32:64` (only in `TargetDataLayout.cpp`; `rust-target-spec.test` checks the Rust spec's copy) |
| C type model | `int`=32; `long`/`long long`/`void*`=64 |
| Hardware FP | none — soft-float (compiler-rt builtins); `fabs`/`fneg`/`copysign` are bit operations on the integer representation (`BCLR`/`ANDV`, `BTGL`/`XORI`, a masked OR), never libcalls (`fp-sign-ops.ll`) |
| CMOV | none — `select` becomes a `SELECT_CC_RR/RI` pseudo, expanded to compare + branch; a run of selects on the same condition shares one compare/branch (one PHI each) |
| Atomics | no instructions — "forced atomics" (as RISC-V `+forced-atomics`), **atomic within one core only**: an aligned atomic load/store of 1/2/4/8 bytes is the plain access (patterns), a fence a compiler barrier, every RMW / cmpxchg a `__sync_*_N` libcall (LibCall actions) that the runtime's `src/atomic.c` runs with `INT_MASK` saved/zeroed/restored; FP / `uinc_wrap` / `udec_wrap` / `usub_*` RMWs are cmpxchg loops (`shouldExpandAtomicRMWInIR`); wider or misaligned atomics are `__atomic_*` libcalls (not provided).  The type legaliser promotes a 1/2/4-byte RMW to i64 first, so those libcall values go in any-extended and come back zero-extended (not the C ABI's sign-extended i32; `atomic.c` takes and returns `uint64_t`).  Clang inlines atomics up to 64 bits (`MaxAtomicInlineWidth`).  Nothing makes them atomic against core 2 (AMP), whose DDR view also bypasses core 1's D-cache |
| Sign-extending loads | i8/i16/i32: hardware (`LDIDX8_S`/`LDIDX16_S`/`LDIDX32_S`) |
| Load/store addressing | `[r]` (`MEMGETn`/`MEMSETn`), `r + simm32` (`LDIDXn`/`STIDXn`, 1-word short form for a small scaled offset), `r + r` at every width (`LDIDXnR`/`LDIDXn_SR`/`STIDXnR`, MODE=11), an absolute imm32 at every width (`LDABSn`/`LDABSn_S`/`STABSn`, `MEMREADR`/`MEMSETR`, MODE=10, 2 words) for a naturally aligned constant address such as an MMIO register; 16/32-bit forms other than `MEMGET32` force-align the EA and no 64-bit form reads an unaligned doubleword, so an under-aligned non-volatile 32-bit load is `MEMGET32` of the computed address (`allowsMisalignedMemoryAccesses`, loads only) and an under-aligned i64 load two 32-bit loads (`MEMGET32` below 4-byte alignment, `LDIDX32` at 4): `allowsMisalignedMemoryAccesses` allows it so that the combiner can build one from 8 byte loads, then `LowerLOAD` splits it at legalization (the post-legalize combiner legalizes what it creates; `PreprocessISelDAG` is a backstop for one that still reaches isel, `unaligned-load-backstop.ll`), and the `STORE` combine keeps a copy two 32-bit copies when store merging swapped its halves (a ROLV) or under optsize (otherwise SHLV + ORR + `STIDX64`: one instruction more than two copies, 0.3-0.5 cycles per 8 bytes faster on the board); the halves carry `KlaussCPU::MOSplitHalf`, so nothing joins them again; every other under-aligned access (stores, volatile) is split |
| C environment | hosted by default (the driver no longer adds `-ffreestanding`): `memcpy`/`memset`/`memmove` of a constant size are intrinsics, inline up to 8 accesses (4 under optsize) at the width both sides' alignment allows (a source below 4-byte alignment still allows 4, `MEMGET32`); a constant source is stored as immediates (`shouldConvertConstantLoadToIntImm`); stack objects and module globals they touch get 8-byte alignment (`shouldAlignPointerArgs`).  `printf`/`fprintf` are not turned into `puts`/`putchar`/`fputc`/`fputs`/`fwrite` (`TargetLibraryInfo`: each would link another stdio function for UART-bound output).  Code that implements the C library (picolibc, compiler-rt, crt0's BSS loop, kernels) is built `-ffreestanding` / `no_builtin` |
| 32-bit ops | `ADDW`/`SUBW`/`ADDIW`/`MULW`, `CMPRRW`/`CMPRVW`, boolean `CMPxxRW`/`CMPxxVW` (ISA v3 D2/D3) |
| Immediates | each class 2/3 op exists with a sign- and a zero-extended imm32 (SGN bit); isel picks the one that holds the constant: `SETR`/`SETRU` (MOV), `ADDI`/`ADDV`/`MINUSV`, `ANDI`/`ANDV` (and OR/XOR), `CMPRV`/`CMPRVU`; boolean compares (`setcc` as a value) `CMPxxV` with a simm32; `SETR64` only for a value neither reaches.  An add of -255..-129 (a frame address too) is `MINUSV` of the negation, whose short form reaches it |
| Multiply by a constant | `MULV` (simm32; EX-serialised, `WriteMul`); a constant 2^N+1, 2^N-1 or 1-2^N that is not a simm32 becomes `SHLV` + `ADDR`/`SUBR` (`decomposeMulByConstant`); for a simm32 one only with `-klausscpu-mul-imm-shift-add` (one more instruction than `MULV`, fewer cycles) |
| Addresses | 32-bit, zero-extended in a register: `SETRU sym` (static), `LEAPC` (PIC); `sym+off` is folded only for `off` in [0, 2^31), other offsets are a separate add so a negative S + off stays negative; `R_KLAUSSCPU_ABS64` data is the exact 64-bit S + A (lld) |
| Direct call | the 1-word short `CALL` (ISA v3 A1, LINK=1: PC + 4·simm18, ±512 KiB, pushes PC+4; written `callrel`; a tail call's jump likewise, `tail`) for every call to a symbol: resolved by the assembler for a local (static) callee in its own section (relaxed to the 2-word `CALLREL` if out of reach), else (a global or weak callee too, which the link may override) an `R_KLAUSSCPU_CALL18` relocation, for which lld calls an out-of-reach callee through a `jmprel` thunk.  Code a loader without thunks relocates (Zephyr LLEXT) is built with `-mllvm -klausscpu-short-calls=false` (2-word `CALL` / `CALLREL`) |
| Indirect branch / call | `JMPR` / `CALLR` |
| Switches | a compare tree, or a jump table from 12 cases (clusters) at 40% density (the board break-even): a 4-byte-aligned table of absolute 32-bit block addresses (`EK_Custom32`, `R_KLAUSSCPU_ABS32`, also under PIC: only the base is PC-relative, `LEAPC`), dispatched by `LDIDX32R` + `JMPR` (`LowerBR_JT`) |
| `llvm.trap` / `llvm.debugtrap` | both `TRAP`: an abnormal stop (crash dump; `stop = Trap` in the emulator; a debug halt with SIGTRAP under the debugger).  `HALT` is the clean stop the runtime ends a program with (exit status in R12) |
| Shifts / rotates | 64-bit only; a register amount uses only rs2[5:0], so isel drops masks/extensions of it (`shamt`); an i32 rotate is a 64-bit rotate of the value copied into both halves (`ROLW`/`RORW`) |
| Bit fields | `BEXTR` for `(x >> s) & (2^l - 1)` with s, l in [1, 31]; other fields and high/low masks beyond an imm32 as a shift pair (a mask stays SETR64 + ANDR in a loop, where MachineLICM hoists the SETR64, unless optsize) |

### Instruction encoding

ISA v2/v3 (hardware repo `klaussCPU/ISA_ENCODING_V2_MAP.md`): 1–3 LE 32-bit
words, word 0 = LEN[31:30] CLASS[29:26] attrs/OP[25:12] rd[11:8] rs1[7:4]
rs2[3:0], imm32 in word 1.  The `Inst` fields in `KlaussCPUInstrInfo.td` are
the encodings (each format class takes the ISA map's word-0 template; short
forms derive theirs from the 2-word twin), emitted by `-gen-emitter`'s
`getBinaryCodeForInstr`.  The AsmPrinter and AsmParser turn a 2-word
instruction into its 1-word short form through the `CompressPat`s
(`-gen-compress-inst-emitter`, `KlaussCPU::compress`) when its literal
immediate fits, or when a branch or call target is a symbol; the asm backend
relaxes a short branch by expanding its pattern again (`KlaussCPU::uncompress`)
when its target is out of reach or not resolved in the same section, and a
short call only when it is resolved (a local callee in the same section) and
out of reach: otherwise the call keeps its `FK_KlaussCPU_CALL18` fixup, which
becomes `R_KLAUSSCPU_CALL18`.  The
`short-forms` / `short-branches` / `short-calls` subtarget features gate this
(each implies the one before); every CPU has them, and
`-mllvm -klausscpu-short-forms=false` / `-klausscpu-short-branches=false` /
`-klausscpu-short-calls=false` turn them off (overriding `-mattr`).  The
short instructions carry the same predicates, so without the feature the
disassembler does not decode them and the AsmPrinter's predicate verifier
rejects one.  Since the target has a feature table, an unknown `-mattr`
feature draws LLVM's "not a recognized feature" warning.  Operands that may
be symbols carry an `EncoderMethod` that creates the fixup.  Every immediate
operand has a range-checked `AsmOperandClass` (`KCImmAsmOperand`): the
assembler rejects an out-of-range constant, and a symbol where no fixup
exists, with the class's `DiagnosticString`.  Registers have no prefix, so
a lone register name is the register in a register slot and the symbol of
that name in an immediate/target slot (`KlaussCPUAsmParser.cpp` header) — so
a typo such as `call r1` for `callr r1` is an undefined symbol at link time,
not an assembler error.  SETR accepts [-2^31, 2^32) and the CPU sign-extends
the imm32: `setr rX, 0xFFFFFFFF` loads -1 (kept for the runtime's
`setr r0, 0xF00F0000` MMIO idiom); `setru` zero-extends.  `setr rX, 128..255`
compresses to the zero-extending short `setru` (same value), so the asm
printer and disassembler show `setru` there; likewise `addi`/`andi`/`ori`/
`xori` with 128..255 become the short `addv`/`andv`/`orv`/`xorv`.

The disassembler is `-gen-disassembler` with one table per width
(`--specialize-decoders-per-bitwidth`: 32/64/96 bits); `getInstruction` picks
the table from LEN.  Each operand type names its `DecoderMethod`: the CPU's
SGN rule decides signed vs unsigned imm32, PC-relative operands decode to the
byte displacement, and the fused-branch condition rejects PRED 5..7 (which
the CPU traps on).  LEN=00 words (retired v1 opcodes, lld's fill word, data)
do not decode.  The ISA v1 LED / 7-segment / RGB opcodes are not defined:
those peripherals, like the UART, are memory-mapped.

### Adding an instruction

Everything lives in `KlaussCPUInstrInfo.td`; no C++ changes are needed for an
ordinary instruction:

1. `def` it with the format class matching its operand layout (`RRR`,
   `RV_imm`, `RRV_ld`, `Vbr_cond`, …), passing the word-0 template from the
   ISA map's "Template" column.  The format class's `KCInst32/64/96` base
   asserts that the template's LEN matches the size.
2. Use the existing operand types (`simm32`, `uimm32`, `uimm6`, `simm32_abs`,
   `PCRelTarget32`, …): they carry the assembler range check, the decoder
   and, for symbol-capable operands, the fixup.  A new operand type needs a
   `KCImmAsmOperand` and a `DecoderMethod` (`decodeSImmOperand<N>` /
   `decodeUImmOperand<N>` usually fit); only a new fixup needs C++ (an
   `EncoderMethod` in `KlaussCPUMCCodeEmitter.cpp` plus the fixup kind in
   `KlaussCPUFixupKinds.h` / `KlaussCPUAsmBackend.cpp`).
3. Set `Defs = [FLAGS]` / `Uses = [FLAGS]`, `mayLoad`, `mayStore`,
   `hasSideEffects` and `SchedRW` to what the RTL does (the register
   formats default to `Defs = [FLAGS]`; one whose decode sets no flag write
   mask goes in a `let Defs = []<Register>, SchedRW = [WriteALU]` block),
   then regenerate the runtime's flags test (see Flags).  An instruction with
   no isel pattern (one selected in C++, e.g. the W compares) must state
   `hasSideEffects = 0` explicitly: otherwise TableGen guesses 1, which
   blocks MachineCSE/LICM/sinking and splits select groups.
4. For codegen, add an isel `Pat<>`; for a v3 short twin, a `ShortForm`
   instance and its `CompressPat`.
5. Pin the encoding in `llvm/test/MC/KlaussCPU/encoding-coverage.s` (checked
   against the ISA map, not against the backend) and the decoding in
   `llvm/test/MC/Disassembler/KlaussCPU/all-instructions.txt`.

### Flags

There is one flag register, Z/S/C/V (`FLAGS` in `KlaussCPURegisterInfo.td`,
in no allocatable class).  `CMP` is `SUB` without writeback; every branch
condition is derived from Z/S/C/V (EQ = Z, signed LT = S^V, unsigned
ULT = C, borrow polarity as on x86).  Exactly the instructions whose RTL
decode sets a flag write mask (`pipeline_core.sv` f_decode_raw `fmask`)
declare `Defs = [FLAGS]`: ADD/SUB/ADC/SBC/INC/DEC/CMP and their W and
immediate forms, the shifts and rotates (F = 1 in every template), BTST,
BEXTR, NEG/NOT/ABS/SEXTB/SEXTH/ZEXTB/ZEXTH/POPCNT, MUL/DIV/MOD and IRET.
AND/OR/XOR (every form), MIN/MAX, the boolean compares, the bit set/clear/
toggle ops, SHLR1/SHRR1/SHRAR, BSWAP/BITREV/CLZ/CTZ/SEXTW/ZEXTW, moves,
loads, stores and branches write none, so the schedulers may put them
between a compare and its branch.  A missing def is a miscompile:
`test_flags` in the runtime repo (`baremetal/programs/gen_test_flags.py`
generates it from this backend's records and checks them against its
table of the RTL's flag masks) runs every instruction with the flags all
clear and all set and fails if a flag outside its table entry changes, or
if an op FlagReuse trusts does not set Z = (rd == 0); it passes on the
emulator and on the board.  Conditional branches and GETF (`setfr`)
`Uses = [FLAGS]`, calls and inline asm clobber FLAGS, and the BR_CC
compare is glued to its branch at isel.  Two pre-emit passes rely on this
liveness:

- `KlaussCPUFlagReuse` (pre-emit, default on): drops a `CMPRV Rd, 0` before
  `JMPE`/`JMPNE` when the op that wrote Rd, the nearest FLAGS writer before
  the compare, already set Z = (Rd == 0) (`setsZeroFlagAsResult`: add/sub,
  INC/DEC, the shifts, NEG/NOT/ABS/extensions/POPCNT, BEXTR, MUL; not
  DIV/MOD, whose zero divisor writes only V).
- `KlaussCPUFuseCmpBr` (pre-emit, default on): fuses a compare and its
  conditional branch into the 1-word ISA v3 `FBR_RR`/`FBR_RI` (64-bit only:
  FBR has no width bit, so `CMPRRW`/`CMPRVW` never fuse).  It scans back
  from the branch to the nearest FLAGS writer, so PHI copies, constants and
  spills between the pair do not stop it; it gives up at a FLAGS reader, a
  redefined compare operand, a call or inline asm.  Range is checked per
  branch at worst-case sizes plus alignment padding; a non-empty inline asm
  (unbounded size) blocks only the branches that span it.  At -O1 and up, a
  loop compare with a constant outside simm4 (in the header or the only
  latch) gets the constant in a register free throughout the loop, set by a
  SETR/SETRU in the preheader, and becomes `FBR_RR`; other compares with
  that constant in the loop reuse it.  That loop-constant setup runs as a
  second instance of the pass before the post-RA scheduler (`addPreSched2`,
  the compare becomes `CMPRR` with the constant register), so the
  scheduler's register renaming cannot take the free register.  A branch
  whose next instruction in layout is a divide (`DIVR`/`DIVUR`/`MODR`/
  `MODUR`/`DIVV`/`MODV`), an LCD write (`LCDCMDV`/`LCDDATAV`/`LCDDATAR`) or
  a non-empty inline asm never fuses: a taken FBR redirects from MEM and
  squashes that instruction in EX, but the squash does not undo either
  effect.  The RTL divider has already started on the divide and is not
  reset, so the next real divide would complete with the wrong-path result;
  the LCD strobe fires from EX in that same cycle, so the wrong-path byte
  reaches the LCD (board-only bugs: the emulator has no pipeline;
  `fuse-cmp-br-div-shadow.ll`/`.mir`).  Those instructions carry the
  `ActsInEXShadow` flag (`TSFlags`, `KlaussCPUII::actsInEXShadow`), which the
  assembler also reads: it warns when assembly puts one directly behind an
  `fbr`/`fbri` (labels between them do not count, a directive does;
  `MC/KlaussCPU/fbr-shadow-warning.s`).
- Both rewrite only when FLAGS is dead after the branch
  (`KlaussCPUInstrInfo::flagsDeadAfter`): no later reader in the block and
  no successor with FLAGS live-in (MachineCSE can leave a successor reading
  the same compare's flags).

---

## Calling convention (`KlaussCPUCallingConv.td`, [`ABI.md`](ABI.md))

ABI version 2 (`KlaussCPUABI::ABIVersion`; RISC-V LP64, integer only):

| Role | Register |
|---|---|
| Arg 0–3 | R0–R3 (A–D), caller-saved, named or unnamed |
| Args 4+ | stack, 8-byte slots: `[CallerSP + n×8]` = `[incoming_SP + 8 + n×8]` = `[R15 + 16 + n×8]`; a variadic call puts them 32 bytes higher (home area) |
| Return | R12 (M); R12+R11 (M+L) for an i128 or a 9–16 byte aggregate, caller-saved; larger returns go through a hidden sret pointer (clang; `CanLowerReturn` for other IR) |
| Callee-saved | R4–R7 (E–H) + R15 (P, frame pointer, saved by the prologue; a frameless function leaves it alone) |
| Temporaries | R8–R11, R13–R14 (I–L, N–O) caller-saved |

- **Extension**: i8/i16/i32 are promoted to i64 as the IR's `signext`/
  `zeroext` says, and clang (`clang/lib/CodeGen/Targets/KlaussCPU.cpp`)
  marks every i32 `signext`, also `unsigned int` (as RV64), narrower
  integers by their signedness: the producer extends, the consumer relies on
  it (`AssertSext`/`AssertZext`).  The backend's libcalls do the same
  (`shouldSignExtendTypeInLibCall`; a softened f32 is not extended, a
  softened f16 is zero-extended as compiler-rt's `uint16_t`), and
  `TargetLibraryInfo` marks the i32s of the library calls the optimiser
  creates `signext`.
- **Aggregates** (clang): up to 16 bytes direct, coerced to iN / i128 /
  `[2 x i64]` (one or two GPRs; two parts may straddle R3 and the stack);
  larger ones by reference to a copy the caller makes.  **byval** (other
  frontends): the argument is passed as a pointer to a copy the caller makes
  in its own frame (`LowerCall`), so the callee may modify it.
- **Varargs**: unnamed arguments are assigned like named ones.  A variadic
  call reserves a 32-byte home area for R0–R3 at `[incoming_SP+8..39]`
  (`VarArgHomeSize`), and the variadic callee stores its unnamed register
  arguments there, so they are contiguous with the stack args and `va_arg`
  walks 8-byte slots, never realigned (nothing is aligned beyond 8; an i128
  takes two).  A call to a non-variadic function reserves nothing (no call
  frame at all without stack args).  Clang emits the walk as IR (a slot of
  an aggregate over 16 bytes holds its address), so the pointer can stay in
  a register; the IR `va_arg` of other frontends is `LowerVAARG` (full slot
  + truncate).  C23 `f(...)` works (R0 has a home slot).  A call without a
  prototype (K&R `f();`) is variadic when its arguments fit in R0–R3 (clang
  `isNoProtoCallVariadic`), so a variadic callee finds the home area.
- **Call clobbers**: the call instructions `Def` only FLAGS; the registers a
  call clobbers come from its register-mask operand, `CSR_KlaussCPU_Call`
  (R4–R7, R15 and SP preserved).
- **Version**: every object carries the ABI version in `e_flags` (an MC
  target streamer), lld refuses to link code of different versions, and
  clang predefines `__klausscpu_abi__`.
- **Tail calls** (`isEligibleForTailCallOptimization`, as RISC-V): a sibling
  call in tail position (IR `tail`) whose arguments are all
  in registers is the epilogue (`LEAVE`, nothing in a frameless function)
  then `PseudoTAIL` or `PseudoTAILIndirect` (`JMPR`, the address in a
  `GPRTC` register: not R4–R7, which the epilogue reloads).  `PseudoTAIL` is
  `tail sym`: `TAILREL` (JMPREL's encoding), compressed like a call to
  `TAIL_SH`, the 1-word short jump with the short call's fixup
  (`FK_KlaussCPU_CALL18` / `R_KLAUSSCPU_CALL18`, lld's jmprel thunk when out
  of reach), so a tail call costs no more words than CALL + LEAVERET; both
  are `isAsmParserOnly` (the disassembler shows `jmprel`).  The callee finds the caller's return
  address at `[SP]` and returns to the caller's caller.  Not for a variadic
  callee (its home area belongs to a variadic call), stack, byval or sret
  arguments, or `"disable-tail-calls"`; a `musttail` call that cannot be one
  is a fatal error.  Any calling convention qualifies (all are lowered as
  `CC_KlaussCPU`, so the preserved-register masks match; GlobalOpt makes
  internal functions `fastcc`).  CodeGenPrepare does not duplicate a shared
  return block to put calls in tail position (`mayBeEmittedAsTailCall`,
  `-klausscpu-tail-call-dup-ret`): every copy needs its own epilogue, which
  cost 0.15% of `.text` for no measurable board cycles.  Debug info marks
  tail calls `DW_AT_call_tail_call`.

### Frame layout

**CALL pre-decrements SP by 8** (stores return address at [SP-8], SP -= 8).
So `incoming_SP = CallerSP - 8`.  From the **caller's** `GETSP_R` value (= CallerSP):

```
  CallerSP ─────┐  (GETSP_R reads this; SP before CALL pre-decrements)
  Stack args     │  [CallerSP+0 .. CallerSP+N×8]  = [incoming_SP+8 ..]
                 │    (variadic call: [CallerSP+0 .. +31] R0-R3 home area,
                 │     the stack args from CallerSP+32 = incoming_SP+40)
                 │  [CallerSP-8 .. CallerSP-1]   return address (CALL writes here)
  incoming_SP ──┘  ← CallerSP - 8  (SP after CALL pre-decrement; callee entry SP)
  Saved R15      │  ← ENTER pushes R15
  R15 ──────────┘  ← R15 = SP after the push (frame pointer = incoming_SP - 8)
  Callee-saved,  │  [R15 - framesize .. R15 - 8]
  locals, call   │
  frames         │
  SP ────────────┘  ← SP -= 8*N
```

From the **callee's** `incoming_SP` perspective (what `LowerFormalArguments` uses):
- `[incoming_SP+0..7]`   return address
- `[incoming_SP+8..39]`  a variadic function only: the R0–R3 home slots
- `[incoming_SP+8..]` (variadic: `+40..`) stack arguments

`LowerFormalArguments` pre-reserves the return address (8 bytes,
`IncomingArgOffset`) and, in a variadic function, the home area
(`VarArgHomeSize`) in `CCState`; `LowerCall` reserves only the home area of a
variadic call (`OutgoingArgOffset` is 0), so the offsets agree.  Fixed objects are
created relative to `incoming_SP`; `getFrameIndexReference` adds 8 to make
them R15-relative (and the debug info uses the same function, so
`DW_OP_fbreg` agrees).

**Prologue:** `ENTER N` (push R15; R15 = SP; SP -= 8×N), then the
callee-saved spills.  A frame too large for ENTER's 22-bit field uses
`PUSH R15 ; GETSP R15 ; ADDSP -size`.
**Epilogue:** the reloads, then `LEAVERET` (`LEAVE` in front of any other
terminator: a tail call, IRET).  `-klausscpu-enter-leave=false` restores the old
`PUSH/GETSP/ADDSP` / `SETSP R15 ; POP R15 ; RET` sequences.

**Frameless leaves** (`hasFPImpl`): a function without calls and without
stack objects (no local, spill, callee-saved register, stack argument or
vararg slot; no `frameaddress`/`returnaddress`) has no frame: no prologue,
a plain `RET`.  CALL leaves the return address at `[SP]` and R15 untouched
(`pipeline_core.sv` U_CALL/U_RET), so R15 keeps the caller's frame and every
function that calls has one: the R15 chain from any call site is complete.
The `"frame-pointer"` attribute can ask for a frame: clang's default for
KlaussCPU is `non-leaf` (`clang/lib/Driver/ToolChains/CommonArgs.cpp`;
`-mno-omit-leaf-frame-pointer`, i.e. `all`, gives every function one, for
its whole body: it also turns shrink-wrapping off).  Every stack access
goes through R15 (SP is not a GPR), so a frame is never omitted from a
function that calls or uses the stack.  The frame register of the debug info
is SP in a frameless function (`getFrameRegister`).  A shrink-wrapped
function's `DW_AT_frame_base` is R15 throughout, so a stack variable's
location is wrong in the frameless stretch before its ENTER or after its
LEAVE (R15 is the caller's frame there); generated code is unaffected, and
x86 and RISC-V have the same limitation.

**Shrink-wrapping** (`enableShrinkWrapping`, -O1 and up; off at -Os/-Oz,
where the second return costs more than the early exit saves, and under
`"frame-pointer"="all"`): ShrinkWrap may put
the prologue in the block that dominates every use of the frame (a frame
index, a call frame, a callee-saved register, SP, or R15:
`isNonallocatableRegisterCalleeSave`) and the epilogue in one that
post-dominates them, outside loops; the rest of the function runs frameless.
The epilogue goes in front of the restore block's first terminator.  A
restore block must leave the function or only continue to a block that does
(`canUseAsEpilogue`): a `LEAVE` in front of a compare and branch would be one
more instruction and stop them fusing.  Tail duplication then leaves
`LEAVE ... RET`, which `KlaussCPULeaveRet` (pre-emit) folds into `LEAVERET`,
moving the LEAVE past instructions that use neither R15 nor SP and removing a
return block left without predecessors (`leaveret-fold.mir`).  In practice
few frames move: the register allocator often puts a value that lives across
a call in a callee-saved register in the entry block (`fib`'s accumulator).

**Stack realignment:** the ABI stack is only 8-byte aligned.  A function with
a local aligned to A > 8 (`_Alignas(32)`, an `__int128`) reserves **R7 as a
base pointer**: the frame grows by A-8, R7 is saved with the other callee-saved
registers (R15-relative), then `ANDI R7, R15, -A`, and the locals are
addressed R7-relative (`DW_OP_breg7` in the debug info).  Incoming arguments
and callee-saved slots stay R15-relative; the epilogue and CFA are unchanged.
Inline asm may not write R7 in such a function.  See
`KlaussCPUFrameLowering.cpp`.

**CFI** (with `-g`, unwind tables or IR without `nounwind`): CIE
`CFA = SP+8, RA (DWARF 17) = [CFA-8]`; after ENTER `CFA = R15+16`, caller R15
at `CFA-16`; each callee-saved spill is followed by its `.cfi_offset` rule;
after a `LEAVE` (a tail call, a shrink-wrapped restore point) the CIE's
state again.  A frameless function needs nothing beyond the CIE.
`setCFIFixup(true)` gives every block laid out after the prologue the state
it is entered with (frameless code after a shrink-wrapped prologue or
behind a LEAVERET) (`DebugInfo/KlaussCPU/frame-cfi.ll`).  CFI
instructions are scheduling boundaries, so -g must not add one where the
scheduler could otherwise move code: the ENTER/LEAVE (PUSH/ADDSP, SETSP/POP)
CFI follows instructions that write SP, which are boundaries anyway, and the
callee-saved rules are only added after the post-RA scheduler
(`KlaussCPUCalleeSavedCFI`, the last pre-emit pass).  -g and -g0 give the
same code (`csr-cfi-sched.ll`), except with `-klausscpu-enter-leave=false`
in a function without locals (no ADDSP, so the CFI behind `GETSP R15` is
an extra boundary).

### Inline asm

Constraints `r` and `m`/`o`.  A memory operand is a `(base, simm32 offset)`
pair printed as `base, offset` — the operand syntax of `LDIDX*`/`STIDX*`
(`SelectInlineAsmMemoryOperand`, `KlaussCPUAsmPrinter::PrintAsmMemoryOperand`).
Clang adds `~{flags}` to every asm statement, so an asm clobbers FLAGS.
R15, SP and (in a realigning function) R7 draw the reserved-register warning
when clobbered.

---

## Codegen pipeline notes

- Instruction selection is the generated `SelectCode` from the `.td`
  patterns.  `PreprocessISelDAG` first splits any under-aligned i64 load
  that still reaches isel into two 32-bit loads, a backstop that nothing
  normally reaches (`LowerLOAD` splits them at legalization; the i64 load
  patterns take only 8-aligned ones, `load_a8`; see the Load/store
  addressing row, `unaligned-load.ll`, `unaligned-load-backstop.ll`).  `KlaussCPUISelDAGToDAG.cpp` keeps only BR_CC / `SELECT_CC`
  (glued compare) and the 32-bit W-compare choice for those and SETCC:
  `tryW32Compare` takes the W form when an operand's 32-bit extension then
  dies, i.e. it feeds nothing but compares of the same operands and
  condition; not for a branch whose sign extensions are of an add, subtract
  or multiply, which compares 64 bits so that FuseCmpBr can make it an FBR,
  while the extension becomes the W op (a W-op pattern, or
  `KlaussCPUOptExtensions` rule 4): `n++ ; n < max` is `ADDIW` + `FBR`, not
  `INCR` + `CMPRRW` + `JMPcc` (`w32-compare-branch.ll`).  An i32 is promoted
  by sign extension wherever either extension would do (unsigned compares;
  `isSExtCheaperThanZExt`, as RV64), since arguments arrive sign-extended
  and the W ops produce sign-extended values.  A frame index, or a frame index plus a constant in
  [-2^29, 2^29) (`add` or disjoint `or`), is the `AddrFI` / `AddrFIMem`
  ComplexPattern `(TargetFrameIndex, offset)`: one
  `ADDI rd, FI, offset` as a value, or the base and offset of an
  `LDIDXn`/`STIDXn` frame access.  The constant is folded when the access is
  likely to stay in 1-word short-form reach of R15 (estimated from the locals
  laid out before the slot, as running sums computed once per function) or
  the slot has at most two accesses in the block; otherwise the slot address
  is computed once and the accesses use small offsets from it
  (`selectFrameAddr`).  `eliminateFrameIndex` adds the slot offset to the
  folded offset; a frame index anywhere else (or a sum that overflows
  simm32) gets the slot address in a scavenged register (`ADDI`, which
  clobbers FLAGS, so where FLAGS is live it is a fatal error instead: the
  ISA has no flag-free add, and only a frame over 1.5 GiB gets there).  A
  frame-address `ADDI` 129..255 bytes below the frame
  register becomes `MINUSV` (1-word short form) while its FLAGS def is
  dead.
- Shifts and bit fields are patterns over four more ComplexPatterns:
  `shamt` (`selectShiftMask`, as in RISC-V: the amount register of
  SHLR/SHRR/SARR/ROLR/RORR loses an `and` keeping bits [5:0], the
  zero-extension of an i32 amount and any nest of `+`/`-`/`|`/`^` 64k;
  64k - X becomes NEGR, 63 - X / X ^ 63 NOTR), `Dup32` (the
  `ORR (SHLV x, 32), zext(x)` operand of the 32-bit rotate nodes
  `ROLW`/`RORW` that `ReplaceNodeResults` makes from an i32 rotl/rotr),
  `BitFieldExtract` (BEXTR) and `FieldShiftPair`.  The 128-bit `*_PARTS`
  lowering tests bit 6 of the amount and shifts by Amt - 64 in the >= 64
  arm: the DAG must never see a shift by 64 or more there (once the
  combiner proves Amt >= 64 it folds such a shift to 0); `shamt` drops the
  -64 again at isel.  A `SELECT_CC` whose operands fold to a constant
  condition is replaced by the chosen value (`PerformDAGCombine`).
- Scheduling: the model in `KlaussCPUSchedule.td` (CPU `generic`, the
  default) is the RTL's interlocks as issue distances: ALU result 2, load 3,
  FLAGS 2 (`WriteFlags`, the second write of an ALU op with a destination),
  MUL 5 (EX busy 4), DIV 20; the D-cache hit freeze and SP are not modelled
  (order-independent / not movable).  It drives the pre-RA machine scheduler
  (GenericScheduler, in-order: an unready node waits in the pending queue)
  and the post-RA list scheduler, which runs from **-O1** up
  (`getOptLevelToEnablePostRAScheduler`), issues one instruction per cycle
  (`KlaussCPUInstrInfo::CreateTargetPostRAHazardRecognizer`: LLVM's default
  recognizer sets no issue limit for a model without itineraries) and
  renames registers to break anti-dependencies on the critical path
  (`getAntiDepBreakMode`, `ANTIDEP_CRITICAL`).  The pre-RA scheduler keeps
  a `CMPRR`/`CMPRV`/`CMPRVU` next to the conditional branch that reads its
  flags (a macro-fusion DAG mutation, `KlaussCPUTargetMachine::
  createMachineScheduler`): an instruction it put in between could be given
  the compare operand's register by the allocator, and that redefinition
  stops FuseCmpBr (`sched-cmp-br-pair.ll`).  An inline memcpy is
  lowered as groups of four loads then four stores
  (`MaxGluedStoresPerMemcpy`), so no store directly follows its load.
  The post-RA recognizer also keeps the issue slot busy behind a MUL or DIV
  for its `ReleaseAtCycles` (the RTL's EX hold), which the list scheduler
  does not read on its own (`sched-ex-busy.ll`).
  `-mcpu=no-sched` is the A/B baseline without either scheduler (and
  without `KlaussCPUEarlyIVInc` or the TTI unrolling, which serve them).
- Before register allocation (-O1 and up): `KlaussCPUJumpTableHoist` moves
  a jump table's base (`SETRU`/`LEAPC`) and entry offset (`SHLV`, and a
  `COPY` of the index it reads) from the dispatch block into the range-check
  block in front of its compare, where they fill bubbles; the dispatch block
  is left with the dependent `LDIDX32R` + `JMPR` (`jump-table-hoist.ll`,
  `.mir`).  It runs after MachineSink, which would sink them back.
- In the machine SSA optimisations, before MachineLICM (`addILPOpts`, -O1
  and up): `KlaussCPUConstAddrBase` gives three or more absolute accesses
  within short-form reach of one of them (simm8 scaled by the access size)
  one base register (`SETRU`/`SETR`) at their nearest common dominator and
  makes them 1-word `LDIDXn`/`STIDXn` base + offset accesses, e.g. a
  driver's accesses to one device's registers; one or two accesses stay
  absolute.  Outside loops the base is not live across a call: a group with
  a call in the way is split (the accesses behind the dominator's last
  call, the runs between calls, the dominator-tree subtrees).  In a loop
  nest that holds a call, MachineLICM would take any base out of the loop,
  so the pass sets one in the preheader, live across the calls (a
  callee-saved register, or rematerialised), only for at least
  `-klausscpu-const-addr-base-loop-call-min` (6) accesses in the nest;
  fewer stay absolute (`const-addr-base.ll`).  The trade is size for
  instructions: outside a loop a group costs one more instruction (the
  SETRU) and saves N-2 words.  Over the bare-metal corpus the pass saves
  0.4-0.5% of `.text` at -O1/-O2, with no measurable change in the
  emulator's instruction count.
- Before instruction selection (-O1 and up): GlobalMerge merges the internal
  (static) globals of a module, up to 1016 bytes (the reach of a 64-bit
  short-form access), so neighbouring statics share one `LEAPC`/`SETRU`
  base; external globals are not merged (`global-merge.ll`).
- After the generic machine SSA optimisations (-O1 and up):
  `KlaussCPUOptExtensions` deletes a `ZEXTB/H/W` or `SEXTB/H/W` whose
  source is already extended (extending loads, W ops, compares, constants,
  through COPY, PHI and bitwise/min/max ops), whose result is read only in
  its low bits, or whose PHI source can take the same extension already
  made in each predecessor instead (`opt-extensions.mir`, `ext-phi.ll`);
  and a `SEXTW` of an add, subtract or multiply that is read only in its
  low 32 bits, by making that op its W form (`ADDW`/`SUBW`/`MULW`/`ADDIW`;
  off with `-klausscpu-w32-alu=false`; `w32-alu.ll`).
  The last three change bits no instruction reads but a debug value may: a
  debug value of the removed extension describes the source with the
  extension in its DIExpression (`DW_OP_LLVM_convert`), and one of another
  register whose high bits change is set undef unless its variable fits in
  the low bits (`DebugInfo/KlaussCPU/opt-extensions-dbg.ll`).
- Before the post-RA scheduler: FuseCmpBr's loop constants; pre-emit passes:
  LeaveRet (see Frame layout, -O1 and up), FlagReuse, then FuseCmpBr (see
  Flags), then CalleeSavedCFI (see CFI).
- TTI (`KlaussCPUTargetTransformInfo.cpp`): moderate partial/runtime
  unrolling (MaxCount 4) with the budget scaled down for branch-dense loop
  bodies, no peeling; `-mcpu=no-sched` reverts to no body-duplicating
  unrolling.  Loop strength reduction ranks solutions by instruction count first
  (`isLSRCostLess`, as RISC-V), sees 15 GPRs and no vector registers, and
  drops a solution that costs more than the loop it started from
  (`shouldDropLSRSolutionIfLessProfitable`); `-klausscpu-lsr-insns-first=false`
  turns both off (generic register-first order) for A/B.
  `isLegalAddressingMode` (in `KlaussCPUISelLowering.cpp`) is exactly what
  isel selects: r + simm32,
  r + r (no displacement, accesses up to 8 bytes), a naturally aligned
  constant address that SETR or SETRU reaches, no scaled index, no symbol
  address; `isLegalAddImmediate` / `isLegalICmpImmediate` are the
  imm32 forms (ADDI/ADDV/MINUSV, branch compares CMPRV/CMPRVU; a value setcc
  above INT32_MAX still needs SETRU + CMPxxR).  So array loops share one
  byte-offset IV across their r + r accesses.
- After LSR (end of `addIRPasses`, -O1 and up), `KlaussCPUEarlyIVInc`
  rewrites a single-block loop whose exit test `icmp eq/ne %i.next, N`
  reads the incremented counter to count from Init - C and compare with
  N - C, so the increment comes first in the body and the schedulers can
  issue it early instead of directly in front of the branch that reads it.
  Only with constant Init and N (the new constants still 32-bit, still
  simm4 for FBR_RI if N was), no other use of `%i.next`, and every other
  in-loop use of the counter waiting at least as long for another operand
  (so not a load index or GEP; the latencies are the subtarget's schedule
  model's).  Not for `-mcpu=no-sched`.  The counter keeps its debug
  location: its dbg_values move to the new add, with a copy as the new PHI
  + C at the top of the loop, and the old increment's become the new add + C
  (`DebugInfo/KlaussCPU/early-iv-inc.ll`).

### Hidden options (`-mllvm`)

| Option | Default | Effect |
|---|---|---|
| `-klausscpu-short-forms` / `-klausscpu-short-branches` / `-klausscpu-short-calls` | on | ISA v3 1-word forms, branches, calls (override `-mattr`) |
| `-klausscpu-enable-global-merge` | on at -O1+ | GlobalMerge of internal globals |
| `-klausscpu-opt-extensions` | on | KlaussCPUOptExtensions |
| `-klausscpu-enter-leave` / `-klausscpu-leaveret` | on | ENTER/LEAVE frames, LEAVE+RET → LEAVERET |
| `-klausscpu-frameless-leaf` | on | a leaf without stack objects has no frame |
| `-klausscpu-shrink-wrap` | on (not -Os/-Oz) | shrink-wrapping (also LLVM's `-enable-shrink-wrap`) |
| `-klausscpu-tail-calls` | on | sibling calls with register arguments become tail calls |
| `-klausscpu-tail-call-dup-ret` | off | CodeGenPrepare duplicates a shared return so calls before it become tail calls |
| `-klausscpu-fuse-cmp-br` | on | FBR fused compare-and-branch |
| `-klausscpu-fuse-loop-const` | on | FBR for a loop compare with a non-simm4 constant, via a free register set in the preheader |
| `-klausscpu-arith-flag-reuse` | on | FlagReuse peephole |
| `-klausscpu-w32-alu` / `-klausscpu-w32-compare` | on | 32-bit W ops / W compares |
| `-klausscpu-w32-compare-branch` | off | branch on a W compare even when its sign extensions are of an add/sub/mul (=false: compare 64 bits, the extension becomes the W op, FBR fuses) |
| `-klausscpu-sext-i32` | on | promote an i32 by sign extension where either would do (`isSExtCheaperThanZExt`) |
| `-klausscpu-group-selects` | on | consecutive selects on one condition share a compare/branch |
| `-klausscpu-fold-fi-offset` | on | fold frame index + constant into the access / ADDI offset |
| `-klausscpu-sched-cmp-br-pair` | on | pre-RA scheduler keeps a compare next to its branch (for FuseCmpBr) |
| `-klausscpu-break-anti-deps` | on | post-RA scheduler renames registers to break critical-path anti-dependencies |
| `-klausscpu-postra-single-issue` | on | post-RA scheduler issues one instruction per cycle (=false: LLVM's unlimited default) |
| `-klausscpu-postra-ex-busy` | on | post-RA scheduler issues nothing behind a MUL/DIV until its EX hold ends (=false: every op takes one slot) |
| `-klausscpu-early-iv-inc` | on | count a single-block loop from Init - C so its increment can issue early |
| `-klausscpu-min-jump-table-entries` | 12 | jump tables from N cases (0 = none) |
| `-klausscpu-jump-table-density` | 40 | minimum % of a jump table's entries that are cases |
| `-klausscpu-hoist-jt-addr` | on | KlaussCPUJumpTableHoist |
| `-klausscpu-const-addr-base` / `-klausscpu-const-addr-base-min` | on / 3 | KlaussCPUConstAddrBase; fewest absolute accesses that share a base |
| `-klausscpu-const-addr-base-loop-call-min` | 6 | fewest absolute accesses in a loop nest that holds a call that share a base set in its preheader |
| `-klausscpu-pref-loop-align` | 16 | loop-header alignment in bytes |
| `-klausscpu-unaligned-load32` | on | an under-aligned non-volatile 32-bit load is one `MEMGET32` (=false: byte loads + shifts) |
| `-klausscpu-unaligned-load64` | on | the combiner may form an under-aligned i64 load (8 bytes ORed together, with BSWAP for big-endian), which is then split into two 32-bit loads (=false: no i64 load is formed; 8 ORed bytes stay one `MEMGET32` + 4 byte loads + shifts) |
| `-klausscpu-split-load64-at-legalize` | on | testing only: =false leaves an under-aligned non-volatile i64 load to the `PreprocessISelDAG` backstop |
| `-klausscpu-mul-imm-shift-add` | off | multiply by a simm32 2^N±1 / 1-2^N as shift + add/sub when the schedule model rates MULV slower |
| `-klausscpu-unroll-branch-density-gate` | on | scale the unroll budget for branchy loops |

---

## Build

The standard configuration is the `Release` + assertions build in
[`README.md`](README.md#build) — it is what the lit suites, the runtime repo and
the board builds use.  To step through the backend in a debugger, configure a
separate `Debug` build instead:

```bash
# From llvm-project/ (out-of-source build dir), for debugging the backend only:
cmake -G Ninja -S llvm -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="KlaussCPU" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_OPTIMIZED_TABLEGEN=ON \
  -DLLVM_CCACHE_BUILD=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

ninja -C build-debug llc llvm-mc
```

- **`-DLLVM_CCACHE_BUILD=ON`**: route compiles through `ccache` (installed) so
  rebuilds reuse cached objects (~70%+ hit rate). The cache lives outside `build/`
  (survives a `build/` wipe); bump it with `ccache -M 30G` so objects aren't
  evicted between configs.
- **Disk**: a `Debug` build with split DWARF is ~60 GB; the `Release` +
  assertions build is ~10–15 GB. The only artifacts the board builds consume
  are the `build/bin/` tools (`clang`, `ld.lld`, `llvm-objcopy`, …), a few
  hundred MB total.
- The README's `ninja` line builds the full cross toolchain the runtime repo
  needs.  `llvm-readelf` is required — Zephyr's post-build step uses it
  (`CMAKE_READELF`).
- **Then build the compiler-rt builtins** (soft-FP + integer divide) into the
  clang resource dir — the clang driver auto-links `-lclang_rt.builtins` and the
  archive is a `build/` artifact, **not** produced by `clang;lld`. The
  `build-builtins.sh` script now lives in the **`klausscpu-runtime`** repo (run it
  with `KLAUSSCPU_LLVM_BIN` pointing at this fork's `build/bin`):
  ```bash
  <klausscpu-runtime>/build-builtins.sh   # → build/lib/clang/<ver>/lib/<triple>/libclang_rt.builtins.a
  ```
  Re-run it after any `build/` wipe or toolchain rebuild, or the Zephyr link
  fails with `unable to find library -lclang_rt.builtins`. (The bare-metal
  `Makefile` in that repo links individual `crt-*.o` instead and doesn't need this.)


### Quick smoke test

```bash
printf 'define i64 @add(i64 %%a, i64 %%b) {\n  %%r = add i64 %%a, %%b\n  ret i64 %%r\n}\n' \
  | build/bin/llc -march=klausscpu -O0 -x ir -
```

Expected output (a leaf without stack objects has no frame; the CFI
directives appear because the IR function is not `nounwind`, and the CIE
describes it):
```
add:
	.cfi_startproc
	addr	r12, r0, r1
	ret
	.cfi_endproc
```

### Tests

```bash
build/bin/llvm-lit -q llvm/test/CodeGen/KlaussCPU llvm/test/MC/KlaussCPU \
  llvm/test/MC/Disassembler/KlaussCPU llvm/test/DebugInfo/KlaussCPU \
  llvm/test/Transforms/LoopUnroll/KlaussCPU lld/test/ELF/klausscpu-*.s
```

`ninja KlaussCPUTests && build/unittests/Target/KlaussCPU/KlaussCPUTests`
checks `KlaussCPUABI.h` against the .td files (see "Shared constants").

Beyond lit, a change that alters generated code is validated with the
harness in klausscpu-runtime's
[`tools/validate/`](https://github.com/grahamjonesgs/klausscpu-runtime/tree/main/tools/validate)
(`KLAUSSCPU_LLVM_BIN` set to this `build/bin`):

- `validate.sh quick`: the bare-metal corpus at -O1 and -O2 on the klausscc
  emulator against committed UART baselines (each program must also end the
  same way: a program that used to halt and now hangs or traps fails; one
  the instruction cap stops, such as test_rtos, is checked on its first 12
  lines and on its PASS/FAIL verdicts wherever they come), with
  instruction counts and `.text` sizes, a few ABI-conformance and Csmith
  seeds.
- `validate.sh full`: adds 300 Csmith + 150 `--float` seeds, 30 ABI seeds,
  these lit suites (plus any KlaussCPU lldb tests) and an output-byte
  oracle (`ORACLE_BASE=<previous snapshot>` diffs it; a refactor must leave
  it identical).
- `validate.sh csmith N FIRST`: more Csmith seeds.
- `validate.sh board`: board cycles (Dhrystone, also `-fno-builtin`,
  perf_baseline, perf_haz, CoreMark, Embench) against `baselines/perf.tsv`.
- `validate.sh csmith-board N`: Csmith with the same ELF on the board and
  the emulator.
- `validate.sh zephyr`: the Zephyr apps.

Board modes take `BOARD_LOCK`: every session sharing the board must export
the same lock file (the default, `${XDG_RUNTIME_DIR:-/tmp}/klausscpu-board.lock`,
is only safe when nobody uses another), and a board run prints the lock it
uses in its first lines.  After an intended change of output or
performance, `validate.sh baseline <run dirs>` rewrites the baselines; commit
them with the change.  After changing the harness itself, run
`validate.sh selftest`.

Every CodeGen test that compiles to output runs with `-verify-machineinstrs`
(the `not llc` diagnostic tests and the post-`patchable-function` MIR test
are the only exceptions) — keep it there.  The
CodeGen tests are autogenerated: `llvm/utils/update_llc_test_checks.py`
supports `-march=klausscpu`.

### Shared constants

Each KlaussCPU constant that more than one tool needs has one source:
the DataLayout `TargetDataLayout.cpp`, `EM_KLAUSSCPU`
`BinaryFormat/ELF.h`, the relocations `BinaryFormat/ELFRelocs/KlaussCPU.def`,
and everything else `llvm/include/llvm/TargetParser/KlaussCPUABI.h`:
register names and DWARF numbers, the argument / return / callee-saved
registers, the ABI version (`e_flags`), the largest aggregate passed in
registers, the stack alignment and the stack-argument / home-area / CFA
offsets, the
NOP / HALT / TRAP / JMPREL words and lld's fill word (`InvalidWord`, a
reserved encoding that traps as an invalid opcode), the 32-bit address
space.  The backend's code generation follows the .td files, so a change to
the registers, the calling convention or an encoding must update
`KlaussCPUABI.h` too: `llvm/unittests/Target/KlaussCPU` fails until it does
(it runs `CC_KlaussCPU` / `RetCC_KlaussCPU`, so the order of the argument and
return registers and the stack slots' size and alignment are checked too).
The backend's frame code, clang, lld and lldb read the header.  Copies outside
this repository (the runtime's loaders and Zephyr LLEXT relocator, the lldb
RTOS plugin, the FreeRTOS and Zephyr stack alignment, klausscc's gdbserver
and ISA table) are compared with these
sources by klausscpu-runtime's `check-toolchain-constants.py`.

---

## File map

```
llvm/lib/Target/KlaussCPU/
├── CMakeLists.txt              tablegen steps + add_llvm_target
├── KlaussCPU.td                target root: subtarget features, AsmWriter/AsmParser, includes the other .td files
├── KlaussCPURegisterInfo.td    16 GPRs (r0–r15, alt names a–p) + SP + FLAGS; allocation order
├── ABI.md                      the C ABI (version 2): rules, consumers, measurements
├── KlaussCPUCallingConv.td     CC_KlaussCPU, RetCC_KlaussCPU, CSR_KlaussCPU, the call mask CSR_KlaussCPU_Call
├── KlaussCPUSchedule.td        scheduling model (RTL interlock latencies); processors `generic` and `no-sched`
├── KlaussCPUInstrInfo.td       encodings, operand types, isel patterns, short forms + CompressPats, aliases
├── KlaussCPUInstrInfo.h/.cpp   branch analysis, copies, spills, flagsDeadAfter, post-RA hazard recognizer (single issue)
├── KlaussCPURegisterInfo.h/.cpp reserved regs, stack realignment, eliminateFrameIndex
├── KlaussCPUFrameLowering.h/.cpp prologue/epilogue, frameless leaves, shrink-wrapping, CFI, base pointer, getFrameIndexReference
├── KlaussCPUISelLowering.h/.cpp  calls/args/returns, tail calls, varargs, SELECT_CC, BR_JT, i32 rotates, i128 shifts, DAG combines
├── KlaussCPUISelDAGToDAG.cpp   PreprocessISelDAG (under-aligned i64 loads); SelectCode() + C++ for BR_CC, SELECT_CC, W SETCC; AddrFI, shift/bit-field ComplexPatterns; inline-asm 'm'
├── KlaussCPUMachineFunctionInfo.h  per-function state (vararg frame index)
├── KlaussCPUAsmPrinter.cpp     MachineInstr → MCInst (+ CompressPat), inline-asm operand printing
├── KlaussCPUEarlyIVInc.cpp     IR, after LSR: loop counter counts from Init - C so its increment can issue early
├── KlaussCPUFlagReuse.cpp      pre-emit: CMPRV Rd,0 + JMPE/NE → JMPZ/NZ (default on)
├── KlaussCPUJumpTableHoist.cpp pre-RA: jump-table base and entry offset into the range-check block
├── KlaussCPULeaveRet.cpp       pre-emit: a shrink-wrapped epilogue's LEAVE ... RET → LEAVERET
├── KlaussCPUConstAddrBase.cpp  pre-RA (SSA, before MachineLICM): one base register for nearby constant-address accesses
├── KlaussCPUOptExtensions.cpp  pre-RA (SSA): redundant sign/zero extensions
├── KlaussCPUFuseCmpBr.cpp      pre-emit: compare + branch → ISA v3 FBR (default on); pre-sched2 instance: loop constants
├── KlaussCPUCalleeSavedCFI.cpp pre-emit: a callee-saved register's .cfi_offset behind its scheduled spill
├── KlaussCPUTargetTransformInfo.h/.cpp  TTI — M8 unroll budget gated on branch density, no peel; LSR cost order, register count
├── KlaussCPUSubtarget.h/.cpp   single subtarget; member init order is critical
├── KlaussCPUTargetMachine.h/.cpp CodeGenTargetMachineImpl; pass config (AtomicExpand, EarlyIVInc, isel, ILP-opts, pre-RA, pre-sched2 and pre-emit passes); pre-RA scheduler compare/branch pairing
├── AsmParser/
│   └── KlaussCPUAsmParser.cpp  -gen-asm-matcher driver; registers vs symbols, PC-relative targets
├── Disassembler/
│   └── KlaussCPUDisassembler.cpp   reads LEN, runs the -gen-disassembler table for 32/64/96 bits; operand DecoderMethods
├── MCTargetDesc/
│   ├── CMakeLists.txt          LLVMKlaussCPUDesc; depends on KlaussCPUCommonTableGen
│   ├── KlaussCPUMCAsmInfo.h/.cpp  MCAsmInfoELF subclass
│   ├── KlaussCPUInstPrinter.h/.cpp printInst / printOperand; uses GenAsmWriter.inc
│   ├── KlaussCPUMCCodeEmitter.cpp  generated getBinaryCodeForInstr (encodings = .td Inst fields) + fixup operand hooks
│   ├── KlaussCPUFixupKinds.h   ABS32, PCREL32, PCREL18 (short branch), PCREL13 (FBR), CALL18 (short call, tail jump)
│   ├── KlaussCPUAsmBackend.cpp fixups, ELF relocations, short-branch / short-call relaxation, NOP fill
│   └── KlaussCPUMCTargetDesc.h/.cpp  LLVMInitializeKlaussCPUTargetMC; MCInstrAnalysis; generated CompressPat compress/uncompress; ELF target streamer (ABI version in e_flags)
├── TargetInfo/
│   ├── KlaussCPUTargetInfo.h   declares getTheKlaussCPUTarget()
│   └── KlaussCPUTargetInfo.cpp LLVMInitializeKlaussCPUTargetInfo
└── rust/                       rustc target spec, patches and demo crates (see rust/README.md)
```

Outside this directory: `clang/lib/Basic/Targets/KlaussCPU.*` and
`clang/lib/Driver/ToolChains/KlaussCPU.*` (clang target + driver, which
compiles hosted and links with `ld.lld -m elf32klausscpu`; the
`"frame-pointer"="non-leaf"` default is in `CommonArgs.cpp`),
`clang/lib/CodeGen/Targets/KlaussCPU.cpp` (the C ABI's argument
classification, `ABI.md`; `va_arg`), `llvm/lib/Analysis/TargetLibraryInfo.cpp`
(no synthesised stdio calls; `signext` i32 in
`llvm/include/llvm/Analysis/TargetLibraryInfo.h`),
`lld/ELF/Arch/KlaussCPU.cpp` (relocations; `R_KLAUSSCPU_CALL18` range and
thunk decision; the ABI version check of `e_flags`) with the `jmprel` call thunk in `lld/ELF/Thunks.cpp`,
`llvm/include/llvm/BinaryFormat/ELFRelocs/KlaussCPU.def`,
`llvm/lib/TargetParser/TargetDataLayout.cpp` (the DataLayout),
`llvm/include/llvm/TargetParser/KlaussCPUABI.h` (shared constants) with its
check `llvm/unittests/Target/KlaussCPU/`,
`lldb/source/Plugins/ABI/KlaussCPU/`.
`llvm/CMakeLists.txt`: `KlaussCPU` in `LLVM_ALL_EXPERIMENTAL_TARGETS`.

---

## Subtarget member declaration order (critical)

`KlaussCPUSubtarget` must declare `RI` before `II` — C++ initialises members in
declaration order, and `II` takes a reference to `RI` in its constructor.

```cpp
KlaussCPURegisterInfo     RI;          // ← first
KlaussCPUInstrInfo        II;          // ← second: II(*this, RI)
KlaussCPUFrameLowering    FL;
KlaussCPUTargetLowering   TLI;
SelectionDAGTargetInfo    TSI;
```

---

## LLVM 23 gotchas

### General
- `llvm/Support/TargetRegistry.h` removed → use `llvm/MC/TargetRegistry.h`
- `isUnconditionalBranch` removed → use `isBarrier = 1` in .td
- `TargetSelectionDAG.td` must NOT be included explicitly — it's pulled in by `Target.td`
- `RemapAllTargetPseudoPointerOperands<GPR>` required in `KlaussCPU.td`
- `GET_REGINFO_ENUM` must be defined BEFORE `GET_REGINFO_HEADER` in the header — LLVM 23 splits the inc file into per-`#ifdef` sub-includes
- `TargetInfo/KlaussCPUTargetInfo.h` must be created manually (not generated)
- `KlaussCPUGenInstrInfo` constructor: `(const TargetSubtargetInfo &STI, const TargetRegisterInfo &TRI, unsigned CFSetupOpcode=~0u, ...)`

### MCTargetDesc / AsmPrinter
- `MCExpr::print()` is private → use `MAI.printExpr(OS, *expr)` instead
- Include order in `KlaussCPUMCTargetDesc.cpp` matters:
  `GET_REGINFO_ENUM` → `GET_INSTRINFO_ENUM` → `GET_INSTRINFO_MC_DESC` → `GET_SUBTARGETINFO_MC_DESC` → `GET_REGINFO_MC_DESC`
  (the subtarget feature enum comes from `KlaussCPUMCTargetDesc.h`; the CompressPat `.inc` goes last)
  (register enum must precede instr MC desc because implicit Uses/Defs reference register identifiers)
- `SelectionDAGISel::getPassName()` is not virtual — do not try to override it
- `INITIALIZE_PASS` expands to `void llvm::initializeXxxPass(PassRegistry&)` — experimental targets are not in `InitializePasses.h`, so add a manual forward declaration before the macro, and call the initializer from `LLVMInitializeKlaussCPUTarget` so `-run-pass` / `-stop-after` know the pass
- R15 (frame pointer) must **not** be in `CSR_KlaussCPU` — the prologue saves it (ENTER / PUSH R15); listing it would make PEI spill it a second time.  It is in the call register mask (`CSR_KlaussCPU_Call`), or a call would end every R15-based debug location range
- Branch/call target operands have no `PrintMethod`: `printOperand` prints them, and the `Address` overload turns an `OPERAND_PCREL` displacement into a target address under `PrintBranchImmAsAddress` (llvm-objdump)
- Do not define `PRINT_ALIAS_INSTR` unless `printAliasInstr` / `printCustomAliasOperand` are declared in the InstPrinter header

### Calling convention / call frame (step 8)
- `getCallFrameSetupOpcode()` / `getCallFrameDestroyOpcode()` are **NOT virtual** in LLVM 23 — do not declare them as `override`. Pass opcodes as the 3rd/4th constructor parameters to `KlaussCPUGenInstrInfo(STI, RI, ADJCALLSTACKDOWN, ADJCALLSTACKUP)`.
- `copyPhysReg` signature uses `Register` (not `MCRegister`) for DestReg / SrcReg in LLVM 23.
- `InstrEmitter::countOperands` strips chain and glue from the **tail** of the operand list. Machine node operand order must be `[explicit_ops..., chain, glue]` — **chain goes last, not first**. Getting this wrong with `[chain, imm1, imm2]` causes the `#operands for dag node doesn't match .td file!` assertion.
- `KlaussCPUGenCallingConv.inc` must be included inside `namespace llvm {}` — the generated code uses unqualified names (MVT, CCValAssign, etc.).
- Do NOT re-include `KlaussCPUGenInstrInfo.inc` with `GET_INSTRINFO_ENUM` in `KlaussCPUISelDAGToDAG.cpp` — the enum is already visible transitively through `KlaussCPUTargetMachine.h → KlaussCPUSubtarget.h → KlaussCPUInstrInfo.h`.
- Callee operand on the `KlaussCPUISD::CALL` node must be typed as `MVT::i64` (not `MVT::i32`) — KlaussCPU has no i32 register class, so any i32 target address would fail type promotion.

### Global address / large-constant materialization (step 13)
- `DAG.getTargetGlobalAddress(same_params)` is **CSE-deduplicated** — calling it twice returns the *same* `SDNode*`. If you then call `getMachineNode(SETR, DL, MVT::i64, SDValue(tga_node, 0))` and `ReplaceNode(tga_node, setr_node)`, `ReplaceAllUsesWith` rewrites SETR's own operand from `tga_node` to `setr_node` → self-referential machine instruction `%0 = SETR %0` → RegAllocFast crash `"no reload in start block. Missing vreg def?"`.
- **Fix — ADDR wrapper node:** `LowerGlobalAddress` / `LowerExternalSymbol` wrap the TargetGlobalAddress in a `KlaussCPUISD::ADDR` node (`LEAPC` under PIC), which the `(KlaussCPUAddr tglobaladdr)` → `SETRU` pattern selects: `ADDR` (not TGA) is the node replaced → no circular reference.
- Call-site callee addresses **bypass `LowerOperation`** — `LowerCall` calls `DAG.getTargetGlobalAddress()` directly, and the bare TGA is consumed by the `CALL_I` tablegen pattern. Never wrap call-target TGAs in ADDR.
- `SETR64` is a single 12-byte instruction for 64-bit constants: `rd = (hi32 << 32) | lo32`. Isel uses it only for an i64 constant that is neither a simm32 (`SETR`) nor a uimm32 (`SETRU`). Global addresses are always ≤ 32 bits and use SETRU (zero-extended, like LEAPC), not SETR64. `LowerGlobalAddress` / `LowerBlockAddress` fold only an offset in [0, 2^31) into the SETRU / LEAPC operand: `SETRU arr-800000` would zero-extend a negative S + off (`&arr[i - 100000]`, arr at a low address), so such an offset becomes SETRU sym + ADDI off (test `symbol-offset.ll`).
- `SDUse` iterators: `for (auto UI = N->use_begin(); UI != N->use_end(); ++UI)` — `*UI` is an `SDUse&`. Access the user node via `UI->getUser()` (arrow, not dot).

---

## Systems built on this backend

The backend is feature-complete and hardware-confirmed. **The runtime/OS layers
were split out (2026-06-11) into a separate repo:
[`klausscpu-runtime`](https://github.com/grahamjonesgs/klausscpu-runtime).** That
repo depends on a *built* copy of this fork via the `KLAUSSCPU_LLVM_BIN` env var
(→ `<this-fork>/build/bin`) — it is not a submodule. Paths below are relative to
the `klausscpu-runtime` repo root; see its READMEs for build/run detail:

- **Bare-metal programs** (`baremetal/programs/`, `make -C baremetal all`) — load `.elf`/`.bin`
  via the FPGA serial loader.
- **FreeRTOS V11.1.0 port** (`freertos/`) — demo, console shell, lwIP
  net demos, telnet, wolfSSL/wolfSSH.
- **lwIP 2.2.0** (`lwip_port/`, fetched by `get-lwip.sh`) —
  Ethernet (LiteEth) + DHCP; ping/UDP/TCP/HTTP demos. Checksum fix:
  `LWIP_CHKSUM_ALGORITHM=1` in `lwipopts.h`.
- **Zephyr 3.7 LTS port** (`zephyr-ws/klausscpu-zephyr/`) — `hello_world`
  through to the `ssh_shell` app (SSH login + `run` command).
- **LLEXT loadable programs** — the SSH `run` command loads ELFCLASS32 `ET_REL`
  programs at runtime against the kernel's exported libc; concurrent runs across
  SSH sessions are supported. Needs the vendored Zephyr patch
  (`zephyr-ws/klausscpu-zephyr/zephyr-patches/llext-klausscpu.patch`).

---

## Known issues

- **A misaligned access through a pointer that claims natural alignment reads
  or writes the aligned-down address.**  Every 16/32-bit load and store except
  `MEMGET32` (`[r]` 32-bit, `U_LOAD32`) clears the low address bits in hardware
  (`pipeline_core.sv` U_LOAD issue address / store byte enables).  That is UB in
  C (`*(uint32_t *)(buf + 1)`), and the backend only splits (or, for a
  32/64-bit load, makes `MEMGET32`) accesses whose IR alignment is below
  natural (`allowsMisalignedMemoryAccesses`).  The runtime's `test_unaligned`
  checks the loads at every offset on the board.  Since
  `r + r` selects `LDIDX32R` instead of `ADDR` + `MEMGET32`, a type-punned
  `*(uint32_t *)(buf + i)` with odd `i` no longer gets away with it; `buf + 4`
  (`LDIDX32`) never did.  Use `memcpy` or a packed struct for unaligned data.
- **`ldidx64`/`stidx64` mean a different encoding in klausscc.**  LLVM's
  `ldidx64`/`stidx64` (A=1, address `& ~7`) are klausscc's
  `LDIDX64A`/`STIDX64A`; klausscc's `LDIDX64`/`STIDX64` are LLVM's
  `ldidx64u`/`stidx64u` (A=0).  A line copied from LLVM output into a
  `.kla` file unchanged assembles to the other form, which differs only for
  an address that is not 8-byte aligned.  Write `LDIDX64U`/`LDIDX64A` (both
  assemblers accept them; klausscc's listing uses only those).  Otherwise
  klausscc's listing (since its commit e8e82f3) matches `llvm-objdump -d`
  operand for operand over the bare-metal corpus, including a separate
  `rs1` on the in-place forms (`addv r1, r2, 5` is `ADDV B C 5`).  Other
  spellings differ without changing meaning: `ldidx8s` is `LDIDX8_S`
  (`LDIDX8S` also accepted), `fbr`/`fbri` are `BEQ`…`BUGTI`, `lcd` is
  `LCDRST`, and klausscc names the 1-word forms LLVM picks by value with a
  `.S` suffix (`jmpzrel` near is `JMPZ.S`, the short `callrel` `CALL.S`).
- **A data initializer `sym + A` with `A` outside int32 does not assemble.**
  `uintptr_t q = (uintptr_t)&arr + 0x80000000UL;` at file scope emits
  `.quad arr+2147483648`, which MC rejects with `relocation addend out of
  range`: an ELFCLASS32 RELA addend is 32-bit signed, so it cannot be carried.
  The same expression in code is exact (`SETRU arr` + `ADDV`/`ADDI`, see the
  Addresses row).  Compute such values at run time instead.
- **Zephyr *packaged* logging (`cbprintf_package`) may still mis-render mixed
  32/64-bit conversions.**  The packaged path (`lib/os/cbprintf_packaged.c`,
  used by deferred/immediate `LOG_*`) packs each arg by its C-type size
  (`sizeof(int)`=4 for `%d`) and `VA_STACK_ALIGN`, which does not match
  KlaussCPU's uniform 8-byte vararg slots.  This is separate from the fixed
  `va_arg` load-width bug (`HISTORY.md`, "Fixed issues";
  `shouldReduceLoadWidth` returns false; clang now emits a 32-bit load of
  the slot for an `int` `va_arg`, `MEMGET32`/`LDIDX32`, confirmed on the
  board) — the *direct* `cbvprintf`/`printk`
  path works; verify whether the packaged path needs a KlaussCPU
  `VA_STACK_ALIGN` definition.  Production `ssh_shell` already silences it
  (`CONFIG_LLEXT_LOG_LEVEL_WRN`).  Use `%lu`, not `%llu`: `long` is 64-bit
  and picolibc lacks the `ll` modifier.

# KlaussCPU LLVM Backend

Custom LLVM backend for KlaussCPU — a home-designed 64-bit CPU implemented in
Verilog/SystemVerilog on a Digilent Nexys A7 (Artix-7) FPGA.  Built against
LLVM trunk (v23).  Use the **RISC-V backend** as the style reference, not X86 or ARM.

This file describes the backend as it is now.  The ISA itself is defined in
the hardware repo (`klaussCPU/CPU_ARCHITECTURE.md`, `ISA_ENCODING_V2_MAP.md`,
`ISA_V3_IMPL.md`; RTL ground truth `pipeline_core.sv` / `klauss_pkg.sv`).
`HISTORY.md` is the step-by-step development log; many of its statements
describe earlier states of the backend and are superseded by this file.

---

## Architecture facts

| Property | Value |
|---|---|
| GPRs | R0–R15, 64-bit; R15 = frame pointer (P) |
| PC / SP | 32-bit (128 MiB RAM addressable) |
| Memory bus / register file | little-endian |
| DataLayout | `e-m:e-p:64:64-i64:64-i128:128-n32:64` (only in `TargetDataLayout.cpp`) |
| C type model | `int`=32; `long`/`long long`/`void*`=64 |
| Hardware FP | none — soft-float (compiler-rt builtins) |
| CMOV | none — `select` becomes a `SELECT_CC_RR/RI` pseudo, expanded to compare + branch |
| Atomics | none — `setMaxAtomicSizeInBitsSupported(0)`; AtomicExpand turns every atomic op into an `__atomic_*` libcall, fences are compiler barriers |
| Sign-extending loads | i8/i16/i32: hardware (`LDIDX8_S`/`LDIDX16_S`/`LDIDX32_S`) |
| Load/store addressing | `[r]` (`MEMGETn`/`MEMSETn`), `r + simm32` (`LDIDXn`/`STIDXn`, 1-word short form for a small scaled offset), `r + r` at every width (`LDIDXnR`/`LDIDXn_SR`/`STIDXnR`, MODE=11); 16/32-bit forms other than `MEMGET32` force-align the EA |
| 32-bit ops | `ADDW`/`SUBW`/`ADDIW`/`MULW`, `CMPRRW`/`CMPRVW` (ISA v3 D2/D3) |
| Immediates | each class 2/3 op exists with a sign- and a zero-extended imm32 (SGN bit); isel picks the one that holds the constant: `SETR`/`SETRU` (MOV), `ADDI`/`ADDV`/`MINUSV`, `ANDI`/`ANDV` (and OR/XOR), `CMPRV`/`CMPRVU`; `SETR64` only for a value neither reaches |
| Addresses | 32-bit, zero-extended in a register: `SETRU sym` (static), `LEAPC` (PIC), `R_KLAUSSCPU_ABS64` data (lld zero-extends) |
| Indirect branch / call | `JMPR` / `CALLR`; jump tables are off by default (`-klausscpu-min-jump-table-entries=N` enables them) |
| `llvm.trap` / `llvm.debugtrap` | `HALT` / `TRAP` (the debugger's breakpoint) |

### Instruction encoding

ISA v2/v3 (hardware repo `klaussCPU/ISA_ENCODING_V2_MAP.md`): 1–3 LE 32-bit
words, word 0 = LEN[31:30] CLASS[29:26] attrs/OP[25:12] rd[11:8] rs1[7:4]
rs2[3:0], imm32 in word 1.  The `Inst` fields in `KlaussCPUInstrInfo.td` are
the encodings (each format class takes the ISA map's word-0 template; short
forms derive theirs from the 2-word twin), emitted by `-gen-emitter`'s
`getBinaryCodeForInstr`.  The AsmPrinter and AsmParser turn a 2-word
instruction into its 1-word short form through the `CompressPat`s
(`-gen-compress-inst-emitter`, `KlaussCPU::compress`) when its literal
immediate fits, or when a branch target is a symbol; the asm backend relaxes
a short branch by expanding its pattern again (`KlaussCPU::uncompress`).  The
`short-forms` / `short-branches` subtarget features gate this; every CPU has
them, and `-mllvm -klausscpu-short-forms=false` /
`-klausscpu-short-branches=false` turn them off (overriding `-mattr`).  The
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
printer and disassembler show `setru` there.

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
   `hasSideEffects` and `SchedRW` to what the RTL does.
4. For codegen, add an isel `Pat<>`; for a v3 short twin, a `ShortForm`
   instance and its `CompressPat`.
5. Pin the encoding in `llvm/test/MC/KlaussCPU/encoding-coverage.s` (checked
   against the ISA map, not against the backend) and the decoding in
   `llvm/test/MC/Disassembler/KlaussCPU/all-instructions.txt`.

### Flags

There is one flag register, Z/S/C/V (`FLAGS` in `KlaussCPURegisterInfo.td`,
in no allocatable class).  `CMP` is `SUB` without writeback; every branch
condition is derived from Z/S/C/V (EQ = Z, signed LT = S^V, unsigned
ULT = C, borrow polarity as on x86).  Every flag-writing instruction declares
`Defs = [FLAGS]`, conditional branches `Uses = [FLAGS]`, calls and inline asm
clobber FLAGS, and the BR_CC compare is glued to its branch at isel.  Two
pre-emit passes rely on this liveness:

- `KlaussCPUFlagReuse` (pre-emit, default on): drops a `CMPRV Rd, 0` before
  `JMPE`/`JMPNE` when the arithmetic op that wrote Rd already set Z.
- `KlaussCPUFuseCmpBr` (pre-emit, default on): fuses a compare and its
  conditional branch into the 1-word ISA v3 `FBR_RR`/`FBR_RI`.
- Both rewrite only when FLAGS is dead after the branch
  (`KlaussCPUInstrInfo::flagsDeadAfter`): no later reader in the block and
  no successor with FLAGS live-in (MachineCSE can leave a successor reading
  the same compare's flags).

---

## Calling convention (`KlaussCPUCallingConv.td`)

| Role | Register |
|---|---|
| Arg 0–3 | R0–R3 (A–D), caller-saved; i8/i16/i32 promoted to i64 |
| Args 4+ | stack: `[CallerSP + 24 + n×8]` = `[incoming_SP + 32 + n×8]` = `[R15 + 40 + n×8]` |
| Return | R12 (M) for i64; R12+R11 (M+L) for i128, caller-saved; larger returns go through a hidden sret pointer (`CanLowerReturn`) |
| Callee-saved | R4–R7 (E–H) + R15 (P, frame pointer, saved by the prologue) |
| Temporaries | R8–R11, R13–R14 (I–L, N–O) caller-saved |

- **byval**: the argument is passed as a pointer to a copy the caller makes
  in its own frame (`LowerCall`), so the callee may modify it.
- **Varargs**: the caller reserves `[incoming_SP+8..31]` as a home area for
  R1–R3; a variadic callee stores the unnamed register args there, so they
  are contiguous with the stack args and `va_arg` walks 8-byte slots.  R0 has
  no home slot (it would be the return address), so a variadic function
  with no named parameter (C23 `f(...)`) is diagnosed as unsupported.
- No tail calls.

### Frame layout

**CALL pre-decrements SP by 8** (stores return address at [SP-8], SP -= 8).
So `incoming_SP = CallerSP - 8`.  From the **caller's** `GETSP_R` value (= CallerSP):

```
  CallerSP ─────┐  (GETSP_R reads this; SP before CALL pre-decrements)
  Stack args     │  [CallerSP+24 .. CallerSP+24+N×8]  = [incoming_SP+32 ..]
                 │  [CallerSP+0 .. CallerSP+23]  R1-R3 vararg save area
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
- `[incoming_SP+8..31]`  reserved (R1–R3 vararg save slots)
- `[incoming_SP+32..]`   stack arguments

`LowerCall` pre-reserves **24** bytes (`AllocateStack(24)`) so that the first
stack-arg offset of 24 maps to `CallerSP+24 = incoming_SP+32` on the callee side.
`LowerFormalArguments` pre-reserves **32** bytes (`AllocateStack(32)`) from
`incoming_SP`, matching the callee-side layout above.  Fixed objects are
created relative to `incoming_SP`; `getFrameIndexReference` adds 8 to make
them R15-relative (and the debug info uses the same function, so
`DW_OP_fbreg` agrees).

**Prologue:** `ENTER N` (push R15; R15 = SP; SP -= 8×N), then the
callee-saved spills.  A frame too large for ENTER's 22-bit field uses
`PUSH R15 ; GETSP R15 ; ADDSP -size`.
**Epilogue:** the reloads, then `LEAVERET` (`LEAVE` in front of any other
terminator, e.g. IRET).  `-klausscpu-enter-leave=false` restores the old
`PUSH/GETSP/ADDSP` / `SETSP R15 ; POP R15 ; RET` sequences.  Every function
has a frame (no frame-pointer elimination).

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
at `CFA-16`.  `setCFIFixup(true)` repairs blocks laid out after a `LEAVE`.

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
  patterns; `KlaussCPUISelDAGToDAG.cpp` keeps only BR_CC / `SELECT_CC`
  (glued compare, 32-bit W-compare choice) and loads/stores straight off a
  frame index.  Frame indexes as values select through the `AddrFI`
  ComplexPattern (`ADDI rd, FI, 0`).
- Scheduling: the M8 model (`KlaussCPUSchedule.td`, CPU `generic`, the
  default) drives the machine scheduler and the post-RA scheduler, which runs
  from **-O1** up (`getOptLevelToEnablePostRAScheduler`).  `-mcpu=no-sched`
  is the A/B baseline without either.
- Pre-emit passes: FlagReuse, then FuseCmpBr (see Flags).
- TTI (`KlaussCPUTargetTransformInfo.cpp`): moderate partial/runtime
  unrolling (MaxCount 4) with the budget scaled down for branch-dense loop
  bodies, no peeling; `-mcpu=no-sched` reverts to no body-duplicating
  unrolling.

### Hidden options (`-mllvm`)

| Option | Default | Effect |
|---|---|---|
| `-klausscpu-short-forms` / `-klausscpu-short-branches` | on | ISA v3 1-word forms (override `-mattr`) |
| `-klausscpu-enter-leave` / `-klausscpu-leaveret` | on | ENTER/LEAVE frames, LEAVE+RET → LEAVERET |
| `-klausscpu-fuse-cmp-br` | on | FBR fused compare-and-branch |
| `-klausscpu-arith-flag-reuse` | on | FlagReuse peephole |
| `-klausscpu-w32-alu` / `-klausscpu-w32-compare` | on | 32-bit W ops / W compares |
| `-klausscpu-min-jump-table-entries` | 0 (off) | jump tables from N cases |
| `-klausscpu-pref-loop-align` | 16 | loop-header alignment in bytes |
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

Expected output (the `.cfi_*` lines appear because the IR function is not
`nounwind`):
```
add:
	.cfi_startproc
	enter	0
	.cfi_def_cfa r15, 16
	.cfi_offset r15, -16
	addr	r12, r0, r1
	leaveret
	.cfi_endproc
```

### Tests

```bash
build/bin/llvm-lit -q llvm/test/CodeGen/KlaussCPU llvm/test/MC/KlaussCPU \
  llvm/test/MC/Disassembler/KlaussCPU llvm/test/DebugInfo/KlaussCPU \
  llvm/test/Transforms/LoopUnroll/KlaussCPU lld/test/ELF/klausscpu-*.s
```

Every CodeGen test that compiles to output runs with `-verify-machineinstrs`
(the `not llc` diagnostic tests and the post-`patchable-function` MIR test
are the only exceptions) — keep it there.  The
CodeGen tests are autogenerated: `llvm/utils/update_llc_test_checks.py`
supports `-march=klausscpu`.

---

## File map

```
llvm/lib/Target/KlaussCPU/
├── CMakeLists.txt              tablegen steps + add_llvm_target
├── KlaussCPU.td                target root: subtarget features, AsmWriter/AsmParser, includes the other .td files
├── KlaussCPURegisterInfo.td    16 GPRs (r0–r15, alt names a–p) + SP + FLAGS; allocation order
├── KlaussCPUCallingConv.td     CC_KlaussCPU, RetCC_KlaussCPU, CSR_KlaussCPU
├── KlaussCPUSchedule.td        M8 scheduling model; processors `generic` and `no-sched`
├── KlaussCPUInstrInfo.td       encodings, operand types, isel patterns, short forms + CompressPats, aliases
├── KlaussCPUInstrInfo.h/.cpp   branch analysis, copies, spills, flagsDeadAfter
├── KlaussCPURegisterInfo.h/.cpp reserved regs, stack realignment, eliminateFrameIndex
├── KlaussCPUFrameLowering.h/.cpp prologue/epilogue, CFI, base pointer, getFrameIndexReference
├── KlaussCPUISelLowering.h/.cpp  calls/args/returns, varargs, SELECT_CC, BR_JT, DAG combines
├── KlaussCPUISelDAGToDAG.cpp   SelectCode() + C++ for BR_CC, SELECT_CC, frame-index loads/stores; AddrFI; inline-asm 'm'
├── KlaussCPUMachineFunctionInfo.h  per-function state (vararg frame index)
├── KlaussCPUAsmPrinter.cpp     MachineInstr → MCInst (+ CompressPat), inline-asm operand printing
├── KlaussCPUFlagReuse.cpp      pre-emit: CMPRV Rd,0 + JMPE/NE → JMPZ/NZ (default on)
├── KlaussCPUFuseCmpBr.cpp      pre-emit: compare + branch → ISA v3 FBR (default on)
├── KlaussCPUTargetTransformInfo.h/.cpp  TTI — M8 unroll budget gated on branch density, no peel
├── KlaussCPUSubtarget.h/.cpp   single subtarget; member init order is critical
├── KlaussCPUTargetMachine.h/.cpp CodeGenTargetMachineImpl; pass config (AtomicExpand, isel, pre-emit passes)
├── AsmParser/
│   └── KlaussCPUAsmParser.cpp  -gen-asm-matcher driver; registers vs symbols, PC-relative targets
├── Disassembler/
│   └── KlaussCPUDisassembler.cpp   reads LEN, runs the -gen-disassembler table for 32/64/96 bits; operand DecoderMethods
├── MCTargetDesc/
│   ├── CMakeLists.txt          LLVMKlaussCPUDesc; depends on KlaussCPUCommonTableGen
│   ├── KlaussCPUMCAsmInfo.h/.cpp  MCAsmInfoELF subclass
│   ├── KlaussCPUInstPrinter.h/.cpp printInst / printOperand; uses GenAsmWriter.inc
│   ├── KlaussCPUMCCodeEmitter.cpp  generated getBinaryCodeForInstr (encodings = .td Inst fields) + fixup operand hooks
│   ├── KlaussCPUFixupKinds.h   ABS32, PCREL32, PCREL18 (short branch), PCREL13 (FBR)
│   ├── KlaussCPUAsmBackend.cpp fixups, ELF relocations, short-branch relaxation, NOP fill
│   └── KlaussCPUMCTargetDesc.h/.cpp  LLVMInitializeKlaussCPUTargetMC; MCInstrAnalysis; generated CompressPat compress/uncompress
├── TargetInfo/
│   ├── KlaussCPUTargetInfo.h   declares getTheKlaussCPUTarget()
│   └── KlaussCPUTargetInfo.cpp LLVMInitializeKlaussCPUTargetInfo
└── rust/                       rustc target spec, patches and demo crates (see rust/README.md)
```

Outside this directory: `clang/lib/Basic/Targets/KlaussCPU.*` and
`clang/lib/Driver/ToolChains/KlaussCPU.*` (clang target + driver, which adds
`-ffreestanding` and links with `ld.lld -m elf32klausscpu`),
`lld/ELF/Arch/KlaussCPU.cpp`, `llvm/include/llvm/BinaryFormat/ELFRelocs/KlaussCPU.def`,
`llvm/lib/TargetParser/TargetDataLayout.cpp` (the DataLayout),
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
- R15 (frame pointer) must **not** be in `CSR_KlaussCPU` — the prologue saves it (ENTER / PUSH R15); listing it would make PEI spill it a second time
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
- `SETR64` is a single 12-byte instruction for 64-bit constants: `rd = (hi32 << 32) | lo32`. Isel uses it only for an i64 constant that is neither a simm32 (`SETR`) nor a uimm32 (`SETRU`). Global addresses are always ≤ 32 bits and use SETRU (zero-extended, like LEAPC), not SETR64.
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
  C (`*(uint32_t *)(buf + 1)`), and the backend only splits accesses whose IR
  alignment is below natural (`allowsMisalignedMemoryAccesses` is false).  Since
  `r + r` selects `LDIDX32R` instead of `ADDR` + `MEMGET32`, a type-punned
  `*(uint32_t *)(buf + i)` with odd `i` no longer gets away with it; `buf + 4`
  (`LDIDX32`) never did.  Use `memcpy` or a packed struct for unaligned data.
- **The klausscc assembler/disassembler table (`src/isa/table.rs`) only names
  `LDIDX64R`/`STIDX64R` among the MODE=11 forms, and none of the opposite-SGN
  immediate forms (`setru`, `cmprvu`, `andi`/`ori`/`xori` and their short
  forms).**  The emulator executes them correctly (generic decode), but
  klausscc disassembly and crash dumps do not name them; extend the table there.
- **Zephyr *packaged* logging (`cbprintf_package`) may still mis-render mixed
  32/64-bit conversions.**  The packaged path (`lib/os/cbprintf_packaged.c`,
  used by deferred/immediate `LOG_*`) packs each arg by its C-type size
  (`sizeof(int)`=4 for `%d`) and `VA_STACK_ALIGN`, which does not match
  KlaussCPU's uniform 8-byte vararg slots.  This is separate from the fixed
  `va_arg` load-width bug (`HISTORY.md`, "Fixed issues";
  `shouldReduceLoadWidth` returns false) — the *direct* `cbvprintf`/`printk`
  path works; verify whether the packaged path needs a KlaussCPU
  `VA_STACK_ALIGN` definition.  Production `ssh_shell` already silences it
  (`CONFIG_LLEXT_LOG_LEVEL_WRN`).  Use `%lu`, not `%llu`: `long` is 64-bit
  and picolibc lacks the `ll` modifier.

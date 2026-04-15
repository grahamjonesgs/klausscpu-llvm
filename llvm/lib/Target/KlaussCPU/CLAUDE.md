# KlaussCPU LLVM Backend

Custom LLVM backend for KlaussCPU — a home-designed 64-bit CPU implemented in
Verilog/SystemVerilog on a Digilent Nexys A7 (Artix-7) FPGA.  Built against
LLVM trunk (v23).  Use the **RISC-V backend** as the style reference, not X86 or ARM.

---

## Architecture facts

| Property | Value |
|---|---|
| GPRs | R0–R15, 64-bit; R15 = frame pointer (P) |
| PC / SP | 32-bit (128 MiB RAM addressable) |
| Memory bus | big-endian |
| Register file | little-endian |
| DataLayout | `e-m:e-p:32:32-i64:64-n32:64` |
| Hardware FP | none — soft-float only |
| CMOV | none — SELECT must be expanded |
| Atomics | none — `setMaxAtomicSizeInBitsSupported(0)` |
| Sign-extending loads | none — SEXTLOAD i8/i16/i32 → Expand |

### Instruction encoding families

- **3-reg ALU (RRR/RR/R)** — upper 16 bits of word 0 are non-zero
- **Legacy / immediate (RV/RRV/Vimm)** — upper 16 bits of word 0 are zero; 8-byte instructions

### Flag sets — NEVER MIX

- Arithmetic ops (ADDR, SUBR, …) set: zero / sign / carry / overflow
- Compare ops (CMPRR, CMPRV) set: equal / less / ult / sign

---

## Calling convention (hardcoded in ISelLowering until step 8)

| Role | Register |
|---|---|
| Arg 0–3 | R0–R3 (A–D), caller-saved |
| Args 4+ | stack: `[CallerSP + 32 + n×8]` |
| Return | R12 (M), caller-saved |
| Callee-saved | R4–R7 (E–H) + R15 (P, frame pointer) |
| Temporaries | R8–R11, R13–R14 (I–L, N–O) caller-saved |

### Frame layout

```
  CallerSP ─────┐  (before CALL)
  Stack args     │  [CallerSP+32 .. CallerSP+32+N×8]
                 │  [CallerSP+8 .. CallerSP+31]  reserved
                 │  [CallerSP+0 .. CallerSP+7]   return address (saved by CALL)
  CalleeSP ─────┘
  Saved R15      │  ← PUSH R15
  R15 ──────────┘  ← GETSP R15  (frame pointer)
  Locals         │  [R15 - framesize .. R15 - 8]
  SP ────────────┘  ← ADDSP -N
```

**Prologue:** `PUSH R15 ; GETSP R15 ; ADDSP -N`  (ADDSP omitted if N=0)
**Epilogue:** `SETSP R15 ; POP R15`  (RET emitted by `KlaussCPURetGlue` pattern)

---

## Build

```bash
# From llvm-project/build/ (create if needed):
cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_TARGETS_TO_BUILD="X86" \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="KlaussCPU" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_OPTIMIZED_TABLEGEN=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

ninja LLVMKlaussCPUInfo LLVMKlaussCPUDesc LLVMKlaussCPUCodeGen llc
```

### Quick smoke test

```bash
printf 'define i64 @add(i64 %%a, i64 %%b) {\n  %%r = add i64 %%a, %%b\n  ret i64 %%r\n}\n' \
  | ./bin/llc -march=klausscpu -O0 -x ir -
```

Expected output (as of step 7):
```
add:
    push    r15
    getsp   r15
    addr    r12, r0, r1
    setsp   r15
    pop     r15
    ret
```

---

## File map

```
llvm/lib/Target/KlaussCPU/
├── CMakeLists.txt              tablegen steps + add_llvm_target
├── KlaussCPU.td                target root: includes Register+Instr .td, SchedModel
├── KlaussCPURegisterInfo.td    16 GPRs (r0–r15) + SP; GPR class; allocation order
├── KlaussCPUInstrInfo.td       full ISA + KlaussCPURetGlue SDNode + isel patterns
├── KlaussCPUInstrInfo.h/.cpp   KlaussCPUGenInstrInfo wrapper
├── KlaussCPURegisterInfo.h/.cpp getCalleeSavedRegs, getReservedRegs, eliminateFrameIndex
├── KlaussCPUFrameLowering.h/.cpp emitPrologue / emitEpilogue
├── KlaussCPUISelLowering.h/.cpp  LowerFormalArguments, LowerReturn, KlaussCPUISD nodes
├── KlaussCPUISelDAGToDAG.cpp   SelectionDAGISelLegacy pass; SelectCode() dispatch
├── KlaussCPUAsmPrinter.cpp     MachineInstr → MCInst → text assembly
├── KlaussCPUSubtarget.h/.cpp   single global subtarget; member init order is critical
├── KlaussCPUTargetMachine.h/.cpp CodeGenTargetMachineImpl; PassConfig::addInstSelector
├── MCTargetDesc/
│   ├── CMakeLists.txt          LLVMKlaussCPUDesc; depends on KlaussCPUCommonTableGen
│   ├── KlaussCPUMCAsmInfo.h/.cpp  MCAsmInfoELF subclass
│   ├── KlaussCPUInstPrinter.h/.cpp printInst / printOperand; uses GenAsmWriter.inc
│   └── KlaussCPUMCTargetDesc.h/.cpp  LLVMInitializeKlaussCPUTargetMC
└── TargetInfo/
    ├── CMakeLists.txt
    ├── KlaussCPUTargetInfo.h   declares getTheKlaussCPUTarget()
    └── KlaussCPUTargetInfo.cpp LLVMInitializeKlaussCPUTargetInfo
```

`llvm/CMakeLists.txt` line ~600: `KlaussCPU` in `LLVM_ALL_EXPERIMENTAL_TARGETS`.

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
  `GET_REGINFO_ENUM` → `GET_INSTRINFO_MC_DESC` → `GET_SUBTARGETINFO_MC_DESC` → `GET_REGINFO_MC_DESC`
  (register enum must precede instr MC desc because implicit Uses/Defs reference register identifiers)
- `SelectionDAGISel::getPassName()` is not virtual — do not try to override it
- `INITIALIZE_PASS` expands to `void llvm::initializeXxxPass(PassRegistry&)` — experimental targets are not in `InitializePasses.h`, so add a manual forward declaration before the macro
- R15 (frame pointer) must **not** be in `getCalleeSavedRegs()` when it is managed manually in `emitPrologue`/`emitEpilogue` — PEI would otherwise call `storeRegToStackSlot` which is not implemented
- Omit `PrintMethod` from branch/call target operands in `.td` until `printBrTarget` etc. are implemented in the InstPrinter
- Do not define `PRINT_ALIAS_INSTR` unless `printAliasInstr` / `printCustomAliasOperand` are declared in the InstPrinter header

---

## Completed steps

1. ✅ Fix bad include in `KlaussCPUTargetMachine.cpp`
2. ✅ `ninja LLVMKlaussCPUInfo` builds
3. ✅ `KlaussCPURegisterInfo.td` — 16 GPRs + SP, GPR class
4. ✅ `KlaussCPUInstrInfo.td` — full ISA with isel patterns
5. ✅ `KlaussCPUTargetMachine.cpp` — DataLayout + SubtargetInfo
6. ✅ `KlaussCPUISelLowering` — expansion flags (no FP, no CMOV, no atomics, no sextload)
7. ✅ Minimal instruction selector — `llc -march=klausscpu` produces correct assembly
   - MCTargetDesc layer (AsmInfo, MCTargetDesc, InstPrinter)
   - DAGToDAG pass (`KlaussCPUISelDAGToDAG.cpp`)
   - AsmPrinter (`KlaussCPUAsmPrinter.cpp`)
   - `LowerFormalArguments` (hardcoded R0–R3), `LowerReturn` (R12), `LowerCall` (fatal_error stub)
   - `emitPrologue` / `emitEpilogue` fully implemented
   - `eliminateFrameIndex` replaces FI pseudo with R15+offset

---

## Next steps

### Step 8 — Calling convention in TableGen
- Write `KlaussCPUCallingConv.td`:
  - `CC_KlaussCPU`: args → R0–R3, overflow on stack at `CallerSP+32+n×8`
  - `RetCC_KlaussCPU`: return value → R12
  - Callee-saved: R4–R7
- Add `-gen-callingconv` to `CMakeLists.txt` → `KlaussCPUGenCallingConv.inc`
- Replace hardcoded logic in `LowerFormalArguments` / `LowerReturn` with `CCState::AnalyzeFormalArguments` / `CCState::AnalyzeReturn`
- Implement `LowerCall` (currently `report_fatal_error`) — required for any function with outgoing calls

### Step 9 — Load/store isel patterns + frame index
- Add patterns for `LDIDX64` / `STIDX64` in `KlaussCPUInstrInfo.td`
- Handle `ISD::FrameIndex` in `KlaussCPUISelDAGToDAG::Select()` — convert to `TargetFrameIndex` so `eliminateFrameIndex` can rewrite it to `R15+offset`
- Without this, any function with local variables will crash during selection

### Step 10 — Callee-saved register spilling
- Implement `storeRegToStackSlot` / `loadRegFromStackSlot` in `KlaussCPUInstrInfo`
  using `STIDX64` / `LDIDX64`
- Required when a function clobbers R4–R7

### Step 11 — End-to-end clang test
```bash
clang -O0 -target klausscpu-unknown-elf -nostdlib -S foo.c -o foo.s
```

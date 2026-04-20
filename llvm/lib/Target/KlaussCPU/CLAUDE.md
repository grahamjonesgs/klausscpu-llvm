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
| Memory bus | little-endian |
| Register file | little-endian |
| DataLayout | `e-m:e-p:64:64-i64:64-i128:128-n64` |
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

### Calling convention / call frame (step 8)
- `getCallFrameSetupOpcode()` / `getCallFrameDestroyOpcode()` are **NOT virtual** in LLVM 23 — do not declare them as `override`. Pass opcodes as the 3rd/4th constructor parameters to `KlaussCPUGenInstrInfo(STI, RI, ADJCALLSTACKDOWN, ADJCALLSTACKUP)`.
- `copyPhysReg` signature uses `Register` (not `MCRegister`) for DestReg / SrcReg in LLVM 23.
- `InstrEmitter::countOperands` strips chain and glue from the **tail** of the operand list. Machine node operand order must be `[explicit_ops..., chain, glue]` — **chain goes last, not first**. Getting this wrong with `[chain, imm1, imm2]` causes the `#operands for dag node doesn't match .td file!` assertion.
- `KlaussCPUGenCallingConv.inc` must be included inside `namespace llvm {}` — the generated code uses unqualified names (MVT, CCValAssign, etc.).
- Do NOT re-include `KlaussCPUGenInstrInfo.inc` with `GET_INSTRINFO_ENUM` in `KlaussCPUISelDAGToDAG.cpp` — the enum is already visible transitively through `KlaussCPUTargetMachine.h → KlaussCPUSubtarget.h → KlaussCPUInstrInfo.h`.
- Callee operand on the `KlaussCPUISD::CALL` node must be typed as `MVT::i64` (not `MVT::i32`) — KlaussCPU has no i32 register class, so any i32 target address would fail type promotion.

### Global address / large-constant materialization (step 13)
- `DAG.getTargetGlobalAddress(same_params)` is **CSE-deduplicated** — calling it twice returns the *same* `SDNode*`. If you then call `getMachineNode(SETR, DL, MVT::i64, SDValue(tga_node, 0))` and `ReplaceNode(tga_node, setr_node)`, `ReplaceAllUsesWith` rewrites SETR's own operand from `tga_node` to `setr_node` → self-referential machine instruction `%0 = SETR %0` → RegAllocFast crash `"no reload in start block. Missing vreg def?"`.
- **Fix — ADDR wrapper node:** `LowerGlobalAddress` / `LowerExternalSymbol` wrap the TargetGlobalAddress in a `KlaussCPUISD::ADDR` node. `Select()` then replaces `ADDR` (not TGA) with `SETR(TGA)`. `ReplaceAllUsesWith(ADDR, SETR)` never touches TGA → no circular reference.
- Call-site callee addresses **bypass `LowerOperation`** — `LowerCall` calls `DAG.getTargetGlobalAddress()` directly, and the bare TGA is consumed by the `CALL_I` tablegen pattern. Never wrap call-target TGAs in ADDR.
- Hardware `SETR64` is **buggy** (inverts PC[2] of the address). Use the 3-instruction sequence `SETR hi32 ; SHLV 32 ; ORV lo32` for any 64-bit constant (including global addresses).
- `SDUse` iterators: `for (auto UI = N->use_begin(); UI != N->use_end(); ++UI)` — `*UI` is an `SDUse&`. Access the user node via `UI->getUser()` (arrow, not dot).

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
8. ✅ Calling convention in TableGen — `LowerCall` implemented, `call @bar` produces correct assembly
   - `KlaussCPUCallingConv.td`: `CC_KlaussCPU` (R0–R3, stack≥32), `RetCC_KlaussCPU` (R12), `CSR_KlaussCPU` (R4–R7)
   - `LowerFormalArguments` / `LowerReturn` / `LowerCall` all CCState-driven
   - `ADJCALLSTACKDOWN` / `ADJCALLSTACKUP` pseudos selected manually in `Select()` (no tablegen patterns — i32 type-set failure)
   - `copyPhysReg` implemented using `COPY_R`
   - Smoke test: `foo` calling `bar(i64)` → correct prologue + `call bar` + epilogue

   Expected output (step 8):
   ```
   bar:
       push    r15
       getsp   r15
       copy    r12, r0
       setsp   r15
       pop     r15
       ret
   foo:
       push    r15
       getsp   r15
       addsp   -32
       call    bar
       setsp   r15
       pop     r15
       ret
   ```

9. ✅ Load/store isel patterns + frame index
   - DataLayout changed to `p:64:64` — FrameIndex is i64 from the start (no type-promotion crash)
   - `ISD::FrameIndex` → `TargetFrameIndex` (i64) in `Select()` → `eliminateFrameIndex` rewrites to R15+offset
   - `ISD::LOAD` / `ISD::STORE` with TargetFrameIndex base → `LDIDX64` / `STIDX64` in C++ `Select()`
   - Tablegen patterns added for GPR-base loads/stores with `simm32_imm` offset
   - `SETR` pattern added for 32-bit constant materialization (`simm32_imm`)
   - Smoke tests:
     ```
     sum(i64 %a, i64 %b) with alloca:
         push r15; getsp r15; addsp -8
         stidx64 r0, r15, -8
         addr r12, r0, r1
         setsp r15; pop r15; ret

     foo() calling bar(42):
         push r15; getsp r15; addsp -32
         setr r0, 42
         call bar
         setsp r15; pop r15; ret
     ```

---

10. ✅ Callee-saved register spilling
    - `storeRegToStackSlot` / `loadRegFromStackSlot` implemented in `KlaussCPUInstrInfo`
      using `STIDX64` / `LDIDX64` with `MachineMemOperand` and `MIFlag`
    - LLVM 23 signature: no `TRI` param; has `VReg`, `SubReg=0`, `Flags=NoFlags`
    - Smoke test (`outer` calling `inner` with `%a` live across the call):
      ```
      addsp   -40
      stidx64 r0, r15, -8     # 8-byte Folded Spill
      call    inner
      ldidx64 r0, r15, -8     # 8-byte Folded Reload
      addr    r12, r12, r0
      setsp   r15; pop r15; ret
      ```
    - Also fixed: LDIDX64/STIDX64 opcodes corrected (were swapped with LDIDX32/STIDX32)
    - Also fixed: SETR comment corrected (sign-extends, not zero-extends)

---

11. ✅ Sub-word loads/stores + sign-extension
    - Hardware instruction notes (from opcode_select_opcodes.json):
      - No LDIDX8/16 — use MEMGET8/MEMGET16 (RR register-addressed, zero-extending)
      - MEMGET32 (0x000079) for 32-bit register-addressed loads
      - LDIDX32 (0x00000C) / STIDX32 (0x00000D) for base+offset 32-bit access (RRV format)
      - MEMSET8/16/32 (0x000074/76/78) for truncating stores
    - `KlaussCPUISelLowering`: ZEXTLOAD/EXTLOAD i8/i16/i32 → Legal; SEXTLOAD → Expand
      (expands to ZEXTLOAD + SIGN_EXTEND_INREG); TruncStore i8/i16/i32 → Legal;
      SIGN_EXTEND_INREG i8/i16 → Legal (SEXTB/SEXTH); i32 → Expand (shift pair)
    - C++ `Select()`: extended to handle i32 TargetFrameIndex loads/stores via LDIDX32/STIDX32
    - i8/i16 frame-slot access: not yet implemented (requires register scavenging
      in `eliminateFrameIndex`); -O1+ eliminates these via mem2reg
    - Smoke tests:
      ```
      load_byte_zext:   memget8 r12, r0
      load_byte_sext:   memget8 r12, r0 ; sextb r12
      load_short_zext:  memget16 r12, r0
      load_short_sext:  memget16 r12, r0 ; sexth r12
      load_int_zext:    memget32 r12, r0
      load_int_sext:    memget32 r12, r0 ; setr r14,32 ; shlr r12,r12,r14 ; sarr r12,r12,r14
      store_byte:       memset8 r1, r0
      store_short:      memset16 r1, r0
      store_int:        memset32 r1, r0
      ```

---

12. ✅ Conditional branches — `if` / loops
    - `hasSideEffects = 1` added to CMPRR_I and CMPRV_I (prevents reordering away from branch)
    - `ISD::BR_CC` → Legal in ISelLowering; `ISD::BR` + `ISD::BR_CC` handled in C++ Select()
    - `ISD::BR` → JMP; `ISD::BR_CC` → CMPRR_I (or CMPRV_I for simm32 constants) + JMPxx
    - Chain output from compare consumed by branch → correct scheduling dependency at -O0
    - All 10 CondCodes mapped: SETEQ→JMPE, SETNE→JMPNE, SETLT→JMPLT, SETLE→JMPLE,
      SETGT→JMPGT, SETGE→JMPGE, SETULT→JMPULT, SETULE→JMPULE, SETUGT→JMPUGT, SETUGE→JMPUGE
    - Smoke tests:
      ```
      signed_lt:    cmprr r0,r1 ; jmpge .false ; jmp .true
      cmp_const:    cmprv r0,42 ; jmpne .no    ; jmp .yes
      unsigned_lt:  cmprr r0,r1 ; jmpuge .false ; jmp .true
      countdown:    cmprv r14,0 ; jmpne .loop  (back-edge)
      ```

---

13. ✅ Large constant materialization + global address lowering
    - Hardware `SETR64` is buggy — use 3-instruction sequence: `SETR hi32 ; SHLV 32 ; ORV lo32`
    - `ISD::Constant` i64 values > INT32_MAX handled in C++ `Select()` (ISD::Constant handler)
    - `ISD::GlobalAddress` / `ISD::ExternalSymbol` → Custom in ISelLowering
    - `LowerGlobalAddress` / `LowerExternalSymbol` wrap TGA/TES in `KlaussCPUISD::ADDR` node
      (avoids CSE-induced self-referential SETR — see LLVM 23 gotcha above)
    - `Select()` ADDR handler: `getMachineNode(SETR, DL, MVT::i64, Sym)` + `ReplaceNode(ADDR, SETR)`
    - Direct calls (`call @foo`) still use `CALL_I` tablegen pattern — callee TGA bypasses LowerOperation
    - Smoke tests:
      ```
      large constant 5000000000:    setr r12, 1; shlv r12, 32; orv r12, 705032704
      large constant -5000000000:   setr r12, -2; shlv r12, 32; orv r12, -705032704
      global variable read:         setr r12, g; ldidx64 r12, r12, 0
      global variable write:        setr r12, g; stidx64 r0, r12, 0
      return global address:        setr r12, g
      direct call:                  setr r0, 42; call callee   (CALL_I, not CALLR)
      ```

---

14. ✅ End-to-end clang test — `clang -O0 -target klausscpu-unknown-elf -nostdlib -S foo.c`
    - Registered `klausscpu` in `llvm/include/llvm/TargetParser/Triple.h` (ArchType enum)
    - Updated `llvm/lib/TargetParser/Triple.cpp` — arch name/prefix, parse, canonicalize,
      ELF format, 64-bit pointer width, no 32-bit variant, already-64-bit, LE-only,
      no BE variant, DwarfCFI exception handling
    - `KlaussCPUTargetInfo.cpp`: typed `RegisterTarget<Triple::klausscpu>`
    - New `clang/lib/Basic/Targets/KlaussCPU.h/.cpp` — minimal `KlaussCPUTargetInfo`
      with datalayout `e-m:e-p:64:64-i64:64-i128:128-n64`, 64-bit types, GCC reg names,
      `__klausscpu__` / `__KlaussCPU__` macros
    - `clang/lib/Basic/Targets.cpp` + `CMakeLists.txt` wired in
    - DataLayout updated to include `i128:128` (Clang's default `__int128` align is 16 bytes;
      LLVM defaults i128 to 8-byte aligned when unspecified → assertion in checkDataLayoutConsistency)
    - LLVM 23 gotcha: MCAsmInfo ExceptionHandlingType must match `Triple::getDefaultExceptionHandling()`
      — KlaussCPU needed to be added to the DwarfCFI case in Triple.cpp or the assertion fires
    - LLVM 23 gotcha: at -O0 FastISel is active; when it falls back to SelectionDAG, the STORE/LOAD
      nodes can be visited before their FrameIndex operand has been converted to TargetFrameIndex.
      The C++ Select() handlers for STORE/LOAD must normalise FrameIndex → TargetFrameIndex inline
      rather than assuming it has already been done, otherwise MEMSET32/MEMGET32 tablegen patterns
      match the bare FrameIndex (typed i64) as a GPR address and produce a MachineInstr with a
      FrameIndex as a register operand → `eliminateFrameIndex` crash on out-of-bounds operand access.
    - Smoke tests (both `int` and `long`):
      ```
      int add(int a,int b):   stidx r0,r15,-4; stidx r1,r15,-8; ldidx/ldidx; addr r12,r12,r14
      read_global():          setr r12, global_var; memget32 r12, r12
      main():                 setr r0,1; setr r1,2; call add; call read_global; addr r12,r12,r14
      global_var:             .long 42
      ```

---

15. ✅ i8/i16 frame-slot access — register scavenging in `eliminateFrameIndex`
    - `requiresRegisterScavenging()` → `true` in `KlaussCPURegisterInfo.h` (creates RS in PEI)
    - **LLVM 23 gotcha:** `requiresFrameIndexScavenging()` must remain `false`.  Returning `true`
      switches PEI to virtual-register mode which sets `FrameIndexEliminationScavenging = false`
      → `eliminateFrameIndex` receives `nullptr` for RS.  The correct combination is:
      `requiresRegisterScavenging()=true`, `requiresFrameIndexScavenging()=false` (default) →
      `FrameIndexEliminationScavenging = (RS && !false) = true` → real RS passed.
    - `eliminateFrameIndex` detects MEMGET8/16/MEMSET8/16 (and MEMGET32/MEMSET32 defensively),
      scavenges a scratch reg, emits `SETR scratch, <offset>` + `ADDR scratch, R15, scratch`,
      replaces the FrameIndex operand with scratch.
    - `KlaussCPUISelDAGToDAG.cpp`: explicit i8/i16 FrameIndex LOAD/STORE handlers emit
      `MEMGET8/16` and `MEMSET8/16` with `TargetFrameIndex` as the address operand (consistent
      with how i32/i64 are handled, rather than leaving it to `SelectCode`).
    - Smoke tests:
      ```
      volatile byte store+load:   setr rN,-1; addr rN,r15,rN; memset8 r0,rN; setr r14,-1; addr r14,r15,r14; memget8 r12,r14
      volatile short store+load:  setr rN,-2; addr rN,r15,rN; memset16 r0,rN; setr r14,-2; addr r14,r15,r14; memget16 r12,r14
      ```

## Next steps

### Step 16 — LDIDX32 hardware bug workaround
- Hardware `LDIDX32` inverts `addr[2]` — replace with `LDIDX64` + `ANDV 0xFFFFFFFF` to zero-extend
- Required for correct 32-bit frame-slot loads in all code paths

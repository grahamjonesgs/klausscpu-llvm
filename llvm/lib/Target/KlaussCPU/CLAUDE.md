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
| DataLayout | `e-m:e-p:64:64-i64:64-i128:128-n32:64` |
| C type model | `int`=32; `long`/`long long`/`void*`=64 |
| Hardware FP | none — soft-float only |
| CMOV | none — SELECT must be expanded |
| Atomics | none — `setMaxAtomicSizeInBitsSupported(0)` |
| Sign-extending loads | i8/i16: hardware (LDIDX8_S/LDIDX16_S); i32: SEXTLOAD → Expand → SEXTW |
| Indirect branch / call | JMPR / CALLR — jump tables and function pointers supported |

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
- `SETR64` is a single 12-byte instruction for 64-bit constants: `rd = (hi32 << 32) | lo32`. Use it for any i64 constant that does not fit in a simm32. Global addresses are always ≤ 32 bits and use SETR, not SETR64.
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
    - `ISD::Constant` i64 values > INT32_MAX handled in C++ `Select()` → single `SETR64` instruction
    - `ISD::GlobalAddress` / `ISD::ExternalSymbol` → Custom in ISelLowering
    - `LowerGlobalAddress` / `LowerExternalSymbol` wrap TGA/TES in `KlaussCPUISD::ADDR` node
      (avoids CSE-induced self-referential SETR — see LLVM 23 gotcha above)
    - `Select()` ADDR handler: `getMachineNode(SETR, DL, MVT::i64, Sym)` + `ReplaceNode(ADDR, SETR)`
    - Direct calls (`call @foo`) still use `CALL_I` tablegen pattern — callee TGA bypasses LowerOperation
    - Smoke tests (after Step 17 SETR64 implementation):
      ```
      large constant 5000000000:    setr64 r12, 705032704, 1
      large constant -5000000000:   setr64 r12, -705032704, -2
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
      `MEMGET8/16` and `MEMSET8/16` with `TargetFrameIndex` as the address operand.
    - **Superseded by Step 16**: LDIDX8/16/STIDX8/16 hardware instructions now exist;
      scavenging code removed, frame-slot access goes through the standard base+offset path.
    - Smoke tests (Step 15 approach, now emitted differently in Step 16):
      ```
      volatile byte store+load:   stidx8 r0,r15,-1; ldidx8 r12,r15,-1
      volatile short store+load:  stidx16 r0,r15,-2; ldidx16 r12,r15,-2
      ```

---

16. ✅ Hardware opcode fixes + sub-word indexed instructions + SEXTW/ZEXTW
    - **LDIDX32 opcode corrected**: was `0x00000C` (= non-aligned LDIDX64!), now `0x0000C0`.
      Mnemonic updated to `ldidx32`. Every `int` frame slot was doing a 64-bit access before.
    - **STIDX32 opcode corrected**: was `0x00000D` (= non-aligned STIDX64!), now `0x0000C1`.
      Mnemonic updated to `stidx32`.
    - **LDIDX8/STIDX8/LDIDX16/STIDX16 added** (opcodes 0x0000C4/C5/C2/C3, RRV base+offset format):
      - Replaces the Step 15 scavenging workaround for i8/i16 frame-slot access
      - `eliminateFrameIndex` scavenging block removed — all frame instructions now use the
        standard base+offset path
      - `KlaussCPUISelDAGToDAG.cpp` updated: i8/i16 frame LOAD → LDIDX8/16, STORE → STIDX8/16
        (with Off operand), so eliminateFrameIndex handles them normally
      - `requiresRegisterScavenging()=true` kept as a safety net (harmless)
    - **SEXTW** (opcode 0x00000F0) added — sign-extend 32→64; pattern `(sext_inreg GPR:$rs, i32)`
    - **ZEXTW** (opcode 0x00000F1) added — zero-extend 32→64 (no tablegen pattern yet)
    - **SIGN_EXTEND_INREG i32 → Legal** in ISelLowering (was Expand → shift pair)
    - **Tablegen patterns** added for LDIDX8/16 and STIDX8/16 with GPR base + simm32 offset
      (struct/array fields); MEMGET8/16 patterns kept for pure GPR-address (no offset) access
    - SETR64 hardware bug (PC[2] inversion) confirmed fixed April 2026; backend updated in Step 17

---

17. ✅ SETR64 — single-instruction 64-bit constant load
    - `KCInst96` base class added: `field bits<96> Inst`, `let Size = 12`
      (safe: no `-gen-emitter` in CMakeLists, so `getBinaryCodeForInstr` is never generated)
    - `V64_ld` format: word0[31:8]=op24, word0[7:4]=rd, word1=lo32, word2=hi32
    - `SETR64` def: opcode `0x0000FE`, `"setr64\t$rd, $lo, $hi"`, `(ins simm32:$lo, simm32:$hi)`
    - `Select()` ISD::Constant handler: single `SETR64` for any i64 not fitting in simm32
    - Smoke tests:
      ```
      5000000000:   setr64 r12, 705032704, 1
      -5000000000:  setr64 r12, -705032704, -2
      INT64_MAX:    setr64 r12, -1, 2147483647
      ```

---

18. ✅ MCCodeEmitter + MCAsmBackend — `llc -filetype=obj` ELF output
    - **No `-gen-emitter`**: manual encoder avoids `bits<96>` overflow in `getBinaryCodeForInstr()`
    - New files: `MCTargetDesc/KlaussCPUFixupKinds.h`, `KlaussCPUMCCodeEmitter.cpp`,
      `KlaussCPUAsmBackend.cpp`
    - `KlaussCPUFixupKinds.h`: single custom fixup `FK_KlaussCPU_ABS32` (DWARF column 17)
    - `KlaussCPUMCCodeEmitter`: manual per-format encoder for all 12 instruction formats;
      big-endian 32-bit words emitted via `emitBE32()`; SETR64 handled first (12-byte, 3 words);
      branch/call fixups use `MCFixup::create(4, expr, FK_KlaussCPU_ABS32)` (byte offset 4 = word 1)
    - `KlaussCPUAsmBackend`: LLVM v23 API signatures; `applyFixup` writes 32-bit address big-endian
      into `Data[0..3]`; NOP = `0x0000F010`; ELF ELFCLASS32 / ELFDATA2LSB / EM_NONE / R_KCPU_ABS32=1
    - **LLVM v23 gotchas**:
      - `MCFixupKindInfo` is in `MCAsmBackend.h` — no separate `MCFixupKindInfo.h`
      - `applyFixup` signature: `(const MCFragment &, const MCFixup &, const MCValue &, uint8_t *Data, uint64_t, bool)` — `Data` points directly at fixup byte
      - `getNumFixupKinds()` removed; `getRelocType(const MCFixup &, const MCValue &, bool IsPCRel)` (no MCContext)
      - `getFixupKindInfo` returns by value (not `const &`)
      - `mayNeedRelaxation` / `fixupNeedsRelaxation` removed — default impls suffice
    - **DwarfRegNum fix**: registers had no DWARF numbers → `getDwarfRegNum` returned -1 →
      assertion `RAReg <= 255` fired in `MCDwarf.cpp::EmitCIE`.  Fixed by adding `DwarfRegNum<[N]>`
      to R0–R15 (0–15), SP (16), and virtual `RA` pseudo-register (17) in `KlaussCPURegisterInfo.td`.
      `InitKlaussCPUMCRegisterInfo(X, KlaussCPU::RA)` now passes column 17 as the DWARF RA column.
    - Smoke test (`define i64 @add(i64 %a, i64 %b) { ret i64 (add %a %b) }`):
      ```
      llc -march=klausscpu -filetype=obj → ELF 32-bit LSB relocatable, no machine
      .text bytes (6 × 4B big-endian):
        0x0000400F  push  r15
        0x0000403F  getsp r15
        0x00010C01  addr  r12, r0, r1
        0x0000404F  setsp r15
        0x0000401F  pop   r15
        0x00001012  ret
      ```
    - Known pre-existing issue: `CALL_I` missing implicit-def of `$r12` → regalloc crash when
      call return value is used; does not affect -filetype=asm; fixed in Step 19.

---

19. ✅ Fix CALL_I implicit defs + ELF relocation emission
    - **CALL_I / CALLR_R `Defs`**: added `Defs = [R0,R1,R2,R3,R8,R9,R10,R11,R12,R13,R14]` and
      `Uses = [SP]`; without this regalloc crashed with "using an undefined physical register $r12"
      when a call's return value was used
    - **`applyFixup` missing `maybeAddReloc()`**: every backend must call
      `maybeAddReloc(F, Fixup, Target, Value, IsResolved)` at the end of `applyFixup` to emit
      RELA entries; without it, the bytes were zeroed but no `.rela.text` section was generated
    - **Incremental build fix**: `.ninja_deps` was persistently corrupted from earlier `pkill ninja`
      kills; fixed by deleting `.ninja_deps` + `.ninja_log` and doing one clean rebuild.
      Future builds touching only KlaussCPU `.td` files now take ~20–90 steps, not 1300+.
      **Never use `pkill ninja` — use Ctrl+C (SIGINT) instead.**
    - Smoke test: `foo(i64 %a) → call bar(i64 %a); ret i64` compiles to ELF with:
      - `.rela.text` section, entry: `offset=0x14 type=1(R_KCPU_ABS32) sym=bar addend=0`
      - Correct call encoding: word[4]=`0x00001009` (opcode), word[5]=`0x00000000` (to-be-relocated)

20. ✅ LLD port — `EM_KLAUSSCPU = 0x4B43` / `R_KLAUSSCPU_ABS32 = 1`
    - `llvm/include/llvm/BinaryFormat/ELF.h`: added `EM_KLAUSSCPU = 0x4B43` and
      `#include "ELFRelocs/KlaussCPU.def"` section
    - `llvm/include/llvm/BinaryFormat/ELFRelocs/KlaussCPU.def`: `R_KLAUSSCPU_NONE=0`,
      `R_KLAUSSCPU_ABS32=1`
    - `lld/ELF/Arch/KlaussCPU.cpp` (new): `getRelExpr` → `R_ABS`; `relocate` → `write32be`
    - `lld/ELF/Target.h/.cpp`: `setKlaussCPUTargetInfo` declared + dispatched from `EM_KLAUSSCPU`
    - `lld/ELF/Driver.cpp`: `"elf32klausscpu"` emulation wired in
    - `lld/ELF/CMakeLists.txt`: `Arch/KlaussCPU.cpp` added
    - **Also fixed**: `KlaussCPUMCCodeEmitter::encode64` for `SETR` now handles both
      immediate and MCExpr operands.  Global address loads (`setr r12, sym`) add a
      `FK_KlaussCPU_ABS32` fixup at byte offset 4 (same slot as call targets) so the
      linker fills in the 32-bit symbol address.
    - Smoke test:
      ```
      llc -filetype=obj → .o with EM_KLAUSSCPU; ld.lld -m elf32klausscpu → ELF 32-bit LSB
      ```

---

21. ✅ Linker script + crt0 — flat ELF for 128 MiB RAM
    - `runtime/klausscpu.ld`: ENTRY(_start); RAM 0x0–0x7FFFFFF (128 MiB); layout:
      `.text` → `.data` → `.bss`; exports `__bss_start`, `__bss_end`, `_stack_top = 0x08000000`
    - `runtime/crt0.c`: BSS clear (byte loop), `call main(0,NULL)`, infinite loop halt.
      `_start` placed in `.text._start` so it lands at address 0.
    - `runtime/hello.c`: minimal UART test (memory-mapped TX byte at `0xF0000000`).
    - `runtime/Makefile`: one-line `make hello.elf` / `make hello.bin` build.
    - `ISD::STACKSAVE` / `ISD::STACKRESTORE` lowered to GETSP / SETSP via
      `CopyFromReg` / `CopyToReg` on `KlaussCPU::SP` (enables `__builtin_stack_restore`
      for optional SW stack init).
    - **Stack init note**: `_start`'s prologue (PUSH R15) runs before the function body.
      The hardware Verilog reset must set SP to a valid RAM address (recommended:
      `0x08000000`) before fetching the first instruction.  If SP = 0x08000000 on reset:
      - PUSH R15 → SP = 0x07FFFFF8, writes old R15 to [0x07FFFFF8..0x07FFFFFF] ✓
      - Remaining stack grows down from there.
    - Smoke test:
      ```
      clang -target klausscpu-unknown-elf -O1 -nostdlib -nostdinc -fno-builtin \
            -ffreestanding -c crt0.c hello.c
      ld.lld -T klausscpu.ld -o hello.elf crt0.o hello.o
      → ELF 32-bit LSB executable, *unknown arch 0x4b43*
        _start @ 0x00000000, main @ 0x0000008c
        __bss_start/__bss_end @ 0x00000110, _stack_top = 0x08000000
      ```

---

22. ✅ UART intrinsics + clang builtins — end-to-end hello world
    - **`IntrinsicsKlaussCPU.td`** (new): 7 LLVM intrinsics with `TargetPrefix = "klausscpu"` —
      `int_klausscpu_txr`, `txmemr`, `txcharmemr`, `txstrmemr` (void, side-effects),
      `rxrb`, `rxrnb` (i64 result, side-effects), `newline` (void, side-effects)
    - **`Intrinsics.td`**: `include "llvm/IR/IntrinsicsKlaussCPU.td"` added
    - **`llvm/include/llvm/IR/CMakeLists.txt`**: added `tablegen(LLVM IntrinsicsKlaussCPU.h -gen-intrinsic-enums -intrinsic-prefix=klausscpu)` — **critical**: including `.td` is not enough; each target needs an explicit entry to generate its per-target header
    - **`BuiltinsKlaussCPU.def`** (new): 7 `BUILTIN()` entries mapping `__builtin_klausscpu_*`
    - **`TargetBuiltins.h`**: added `namespace KlaussCPU { enum { ... } }` following XCore pattern
    - **`clang/lib/Basic/Targets/KlaussCPU.h/.cpp`**: `getTargetBuiltins()` wired via `InfosShard` table
    - **`clang/lib/CodeGen/TargetBuiltins/KlaussCPU.cpp`** (new): `EmitKlaussCPUBuiltinExpr()` emits
      LLVM intrinsic calls for all 7 builtins
    - **`CGBuiltin.cpp`**: dispatch `case llvm::Triple::klausscpu:` added
    - **`CodeGenFunction.h`**: `EmitKlaussCPUBuiltinExpr` declared
    - **`KlaussCPUISelDAGToDAG.cpp`**: INTRINSIC_VOID handler (txr/txmemr/txcharmemr/txstrmemr/newline)
      and INTRINSIC_W_CHAIN handler (rxrb/rxrnb) using `ReplaceUses` + `RemoveDeadNode`
    - **`uart_stubs.c`** (new in runtime/): C UART API using `__builtin_klausscpu_*`
      - `uart_putc` uses `static volatile char _uart_char_buf` global (not stack local) so
        TXCHARMEMR_R gets a SETR-resolved address, not a FrameIndex that eliminateFrameIndex
        cannot handle for R-format instructions
      - `typedef unsigned long long uint64_t` instead of `<stdint.h>` (unavailable with -nostdinc)
    - **`hello.c`** updated: calls `uart_puts()` + `uart_newline()` via uart_stubs API
    - **Makefile** updated: `uart_stubs.o` compiled and linked; `OBJCOPY` uses `$(BUILD_DIR)/bin/llvm-objcopy`
    - Smoke test:
      ```
      make hello.elf  → ELF 32-bit LSB executable, *unknown arch 0x4b43*
                         _start @ 0x00000000, .text = 402 bytes
      make hello.bin  → 402-byte flat binary for FPGA loader
      uart_stubs.s emits: txr, txcharmemr, txstrmemr, newline, rxrb, rxrnb
      ```
    - RXRNB zero_flag note: hardware sets zero_flag=1 when FIFO empty but does NOT write
      the destination register; the returned value is undefined when FIFO empty.
      Documented in uart_stubs.c; use `uart_getc_blocking()` for reliable receive.
    - Hardware step remaining: load `hello.bin` onto FPGA, verify UART output.

---

23. ✅ Hardware bring-up — first working C program on real KlaussCPU silicon
    - **Linker script**: `.text` moved from `0x00000000` to `0x00000020` — first 32 bytes of RAM
      are the hardware heap header; code must start at 0x20. Binary is loaded at 0x0020 by the
      hardware loader. All JMP/CALL targets and global address constants were 0x20 too low before
      this fix.
    - **ISD::TRAP → HALT_I**: `setOperationAction(ISD::TRAP, MVT::Other, Legal)` +
      C++ `Select()` handler emits `HALT_I`; `__builtin_trap()` in crt0.c now halts the CPU
      instead of calling `abort`.
    - **LED/7-seg/delay builtins** (Step 22b): `LEDR_R`, `SEG7R_R`, `SEG7BLANK_I`, `DELAYV_I`,
      `DELAYR_R` instructions; 6 new `__builtin_klausscpu_*` builtins; `io_stubs.c` adds
      `leds()`, `seg7()`, `seg7blank()`, `delay_hw()` C functions.
      - `DELAYV_I` takes a constant immediate — DAG handler checks `isa<ConstantSDNode>` and
        falls back to `DELAYR_R` for variable cycle counts.
      - `int_klausscpu_delayv` uses `llvm_i64_ty` (not i32) to avoid PromoteIntegerOperand crash.
    - **SETR64 encoding bug fixed**: word0 was `(0x0000FE << 8) | (rd << 4)` = 0x0000FE10 for
      r1; hardware expects R-format `(0x00000FE << 4) | rd` = 0x00000FE1. Fixed in
      `KlaussCPUMCCodeEmitter.cpp`. The V64_ld format comment in CLAUDE.md was wrong.
    - **Hardware memory model** (discovered during bring-up; see Step 25 for subsequent fix):
      - `MEMGET8` correctly coded per spec: byte N → bits[8(N mod 8)+7 : 8(N mod 8)].
      - `MEMGET32` returns the 32-bit word with the **lowest-address byte in bits[31:24]**
        (MSB-first / big-endian word value). Extracting bytes with `(word >> 0) & 0xFF` gives
        the LAST byte; use `(word >> 24) & 0xFF` for the FIRST byte.
      - `STIDX8/STIDX16/LDIDX8/LDIDX16` had a **byte-ordering bug** at Step 23 time: lane 0
        mapped to the MSByte (big-endian) instead of the LSByte. They were self-consistent with
        each other but inconsistent with MEMGET8/MEMSET8. Fixed in hardware (see Step 25).
      - `STIDX8` + `TXCHARMEMR` pair: both use the same lane mapping, so the store→transmit
        pattern for `_uart_char_buf` is self-consistent regardless.
      - `uart_puts()` uses `MEMGET32` + MSB-first extraction to read string literals from
        .rodata (unaffected by the LDIDX8 bug since .rodata is written by the linker, not STIDX8).
      - `TXSTRMEMR`: reads 32-bit words and sends MSB-first — correct for rcc strings
        but wrong for C strings. Use the `uart_puts()` MEMGET32 loop instead.
    - **hello.c** checkpoints: `leds(0x0001)` / `seg7(0x0001)` on entry to main; `0x0003` after
      puts; `0x0007` after newline. Confirmed working on hardware.
    - **Final result**: `Hello, KlaussCPU!\r\n` printed correctly; LEDs show 0x0007; CPU halts. ✅

24. ✅ Runtime library + test programs — adventure.elf, test_64bit.elf
    - **libc.c** (runtime/): putchar, print_str, newline, getchar, print_int, print_hex_prefix,
      strlen, strcmp, strcpy, memset, memcpy, malloc, calloc, realloc, free, heap_get_top, heap_words_used.
      Heap block header: 24 bytes (size, flags, pad). Heap start written to address 0 for rcc compat.
    - **adventure.c** and **test_64bit.c** updated to use UART/IO functions from uart_stubs.c/io_stubs.c.
    - **Makefile** updated: RUNTIME_OBJS = crt0.o uart_stubs.o io_stubs.o; added libc.o; adventure.elf,
      test_64bit.elf targets.
    - **Bug fixes during this step**:
      - **Jump tables**: switch statements compiled to jump tables (BR_JT+BRIND) — no indirect branch
        in KlaussCPU. Fixed: `setMinimumJumpTableEntries(INT_MAX)` + `setOperationAction(ISD::BRIND, Expand)`.
      - **Tail call assertion**: `LowerCall emitted return value for tail call` — KlaussCPU has no TCO.
        Fixed: `CLI.IsTailCall = false` at top of `LowerCall`.
      - **FrameIndex in CopyToReg (critical)**: passing a stack-local array address to a function call
        creates `CopyToReg(chain, R0, TargetFrameIndex)`. `InstrEmitter::getVR` asserts because TFI
        is not a value node. Fixed (RISC-V pattern): `ISD::FrameIndex` handler in `Select()` now emits
        `SETR rd, TFI` (machine node with virtual register output). `eliminateFrameIndex` detects
        `SETR rd, <FrameIndex>` and expands to `SETR rd, offset; ADDR rd, R15, rd`.
      - **FrameIndex in ADDR (variable-index array access)**: `buf[i]` creates `ADDR(TFI, var)`.
        With the SETR approach above, TFI is now inside SETR — ADDR sees a GPR (SETR output) and never
        has a FrameIndex operand → no scavenging needed for this case.
      - **eliminateFrameIndex scavenging code** kept as safety net for unexpected FI positions in
        non-standard instructions.
    - **Build**: `make adventure.bin test_64bit.bin` → 9389 / 8875 byte flat binaries.

---

25. ✅ CPU hardware fixes (April 2026)
    - **LDIDX8/STIDX8/LDIDX16/STIDX16 byte-ordering corrected**: lane N now maps to
      `bits[8N+7:8N]` for bytes and `bits[16N+15:16N]` for halfwords (little-endian, matching
      CPU_ARCHITECTURE.md §1 and all other load/store instructions). Previously lane 0 mapped to
      the MSByte (big-endian). The four instructions were self-consistent with each other so
      LDIDX8/STIDX8 round-trips worked, but mixing with MEMGET8/MEMSET8 or LDIDX32 on the same
      address produced byte-swapped values within each 8-byte doubleword.
      Byte-enables: STIDX8 now `0000_0001 << lane`, STIDX16 now `0000_0011 << lane`,
      matching MEMSET8/MEMSET16.
    - **SETR64 PC[2]-inversion bug fixed**: earlier silicon inverted address bit 2 during the
      hi32 fetch of the 12-byte encoding; now functions correctly. Backend already updated in
      Step 17; no code changes needed.
    - **MEMGET8 confirmed correct**: per spec `byte N → bits[8(N mod 8)+7 : 8(N mod 8)]`.
      No backend changes required.
    - **No backend changes needed**: LDIDX8/16/STIDX8/16 are already emitted correctly by the
      backend — the hardware now matches what the compiler expects.

---

26. ✅ Fix uart_puts after HW LDIDX8/TXSTRMEMR byte-ordering fix
    - **Root cause**: after the April 2026 HW fix, both TXSTRMEMR and LDIDX8/MEMGET8 scan bytes
      LSB-first (bits[7:0] first) within each 32-bit big-endian physical word.  Since the first
      char 'H' is at bits[31:24], these instructions send/read the LAST char first → reversed
      4-byte groups ("lleH" instead of "Hell").
    - **The only correct instruction for C string output**: MEMGET32, which returns the raw
      32-bit physical word with the first (lowest-address) byte at bits[31:24].  Extract with
      `(word >> 24) & 0xFF` for char 0, `(word >> 16) & 0xFF` for char 1, etc.
    - **Why not just `const uint32_t *`**: `unsigned int` = 64 bits on KlaussCPU
      (`IntWidth=64` in `KlaussCPUTargetInfo`), so `*w++` generates LDIDX64, not MEMGET32.
    - **Fix**: added `__builtin_klausscpu_memget32(const void *ptr)` intrinsic:
      - `IntrinsicsKlaussCPU.td`: `int_klausscpu_memget32` with `[IntrReadMem, IntrArgMemOnly]`
        → lowers as `ISD::INTRINSIC_W_CHAIN` (chain-aware load)
      - `BuiltinsKlaussCPU.def`: `BUILTIN(__builtin_klausscpu_memget32, "ULLivC*", "n")`
      - `clang/lib/CodeGen/TargetBuiltins/KlaussCPU.cpp`: emits `klausscpu_memget32` intrinsic
      - `KlaussCPUISelDAGToDAG.cpp` `INTRINSIC_W_CHAIN` handler: emits `getMachineNode(MEMGET32,
        DL, {MVT::i64, MVT::Other}, {Ptr, Chain})` — chain threads correctly (same pattern as
        LDIDX64 manual selection)
    - **uart_puts rewritten** (`runtime/uart_stubs.c`): uses `__builtin_klausscpu_memget32`
      in a `while(1)` / `for(shift=24; shift>=0; shift-=8)` loop; pointer advances by 4 each
      outer iteration.  `uart_println` inlined as `uart_puts` + `newline`.
    - **Generated assembly verified**: `memget32 r8, r0` appears in uart_puts; inner loop emits
      `shrr / andr / cmprv / memset8 / txcharmemr` — byte extraction + transmission correct.
    - **Binaries rebuilt**: hello.bin (898 B), adventure.bin (9333 B), test_64bit.bin (8787 B).
    - **Regression**: adventure.c strings were partially printed (e.g. "y.", "e.", "g") because
      .rodata strings are NOT guaranteed 4-byte aligned — strings are packed end-to-end.  An
      unaligned string pointer (addr & 3 ≠ 0) caused MEMGET32 to read from the aligned word
      BELOW the pointer, hitting a null terminator from the preceding string and stopping early.
    - **Final fix — unaligned-aware uart_puts** (same Step 26 rebuild):
      Compute `misalign = addr & 3`; if non-zero, call MEMGET32 on `addr & ~3` and start the
      extraction loop from `shift = 24 - 8 * misalign` (skipping the bytes before the string),
      then advance p to the next 4-byte boundary and continue with the aligned loop.
      Generated assembly verified: `setr r9, -4; andr r9, r0, r9; memget32 r8, r9` for the
      aligned address computation; `setr r9, 32; subr r10, r9, r10` for the pre-decremented
      initial shift; both loops emit `memget32`.
    - **Binaries rebuilt**: hello.bin (1226 B), adventure.bin (9661 B), test_64bit.bin (9115 B).

---

27. ✅ LE physical memory CPU fix + backend update
    - **CPU fix applied**: physical memory changed to little-endian — byte at address X now at
      `bits[8*(X mod 4)+7 : 8*(X mod 4)]` of the 32-bit word.  LDIDX8/STIDX8, MEMGET8/MEMSET8,
      and TXSTRMEMR all use LE-lane and are now consistent with physical memory.
    - **`__builtin_klausscpu_memget32` intrinsic removed** — no longer needed:
      - `IntrinsicsKlaussCPU.td`: `int_klausscpu_memget32` removed
      - `BuiltinsKlaussCPU.def`: `BUILTIN(__builtin_klausscpu_memget32, ...)` removed
      - `clang/lib/CodeGen/TargetBuiltins/KlaussCPU.cpp`: case removed
      - `KlaussCPUISelDAGToDAG.cpp`: `INTRINSIC_W_CHAIN` handler for `klausscpu_memget32` removed
    - **`uart_stubs.c` simplified**: `uart_puts` → single `txstrmemr` call;
      `uart_println` → `txstrmemr` + `newline`.  All MEMGET32 loop and unalignment
      handling removed.  `uart_putc` unchanged (still uses `_uart_char_buf` global + TXCHARMEMR).
    - **`libc.c` comment updated**: removed references to MEMGET32/scrambled addressing.
    - **`test_64bit.c` comment updated**: note updated to reflect LE physical memory.
    - **Expected**: T12 strings now passes (`strcpy`/`strlen`/`strcmp` work correctly with
      standard C byte access via LDIDX8 with LE-lane matching LE physical memory).
    - **Binaries rebuilt** after LLVM + Clang rebuild.

---

28. ✅ C `int` migrated from 64-bit to 32-bit (standard C type model)
    - **Motivation**: third-party C code (Embench, test-suite kernels, file-format code)
      assumes `sizeof(int)==4`; arrays of `int` halve in memory; first-class i32 codegen
      paths (LDIDX32/STIDX32, MEMGET32/MEMSET32, SEXTW/ZEXTW) get exercised on every
      ordinary `int` operation rather than as corner cases.
    - **`clang/lib/Basic/Targets/KlaussCPU.h`**: `IntWidth = IntAlign = 32`; DataLayout
      changed to `e-m:e-p:64:64-i64:64-i128:128-n32:64` (`n32:64` advertises both i32 and
      i64 as native widths). `long`, `long long`, and pointers stay 64-bit.
    - **`KlaussCPUTargetMachine.cpp`**: matching DataLayout string update + comment block.
    - **`KlaussCPUCallingConv.td`**: `CC_KlaussCPU` and `RetCC_KlaussCPU` now begin with
      `CCIfType<[i8, i16, i32], CCPromoteToType<i64>>` so sub-word arguments and returns
      are promoted to GPR-width before register/stack assignment. Without this, calls
      passing or returning `int` would fail CC analysis since the GPR class is i64-only.
    - **No `KlaussCPUISelLowering.cpp` change required**: the GPR class is still i64-only,
      so the type legalizer naturally promotes i32 ALU ops to i64.  Existing `setLoadExtAction`
      / `setTruncStoreAction` / `SIGN_EXTEND_INREG i32 = Legal` settings already cover the
      i32-as-memory-only model.
    - **Runtime updates**:
      - `runtime/libc.c`: `heap_get_top` returns `long long` (was `int` — now too narrow);
        `heap_init` writes the heap-start pointer to address 0 as `uint64_t *` rather than
        `int *` (the hardware reserves 8 bytes there).
      - `runtime/test_64bit.c`: rewritten so each test uses the right type — T1–T7 retyped
        to `long long` since they specifically exercise 64-bit shifts/multiplies/comparisons;
        new T3b checks signed `int` wrap-around at INT32_MAX; T10 split into existing-
        homogeneous-int (T10a–c) and new mixed-width T10d (`sum6_mixed`) which exercises
        i32→i64 promotion at call boundaries; T14 split into T14a (32-bit globals via
        `g_val`) and T14b (64-bit globals via `g_val64`); T15 expanded with both an `int[]`
        (sizeof=4, dist=12) and a `long long[]` (sizeof=8, dist=24) walk; T16 retyped to
        `long long *` since the libc heap is 8-byte-slotted; new `check_eq64` helper.
    - **Files NOT requiring changes**:
      - `runtime/adventure.c` — uses `int` only for small enums + `print_int(long long)`.
      - `runtime/hello.c` — `unsigned long long` everywhere.
      - `runtime/uart_stubs.c`, `io_stubs.c`, `crt0.c` — explicit fixed-width types.
    - **Codegen-corner risks worth verifying after rebuild**:
      - `int add(int a, int b)` smoke: expect SEXTW on each arg post-load, R12 holds
        promoted i64 return.
      - `long load_int(int *p) { return *p; }` — should emit MEMGET32 / LDIDX32 (zero-
        extending) followed by SEXTW for sign-extended widening, or no SEXTW for the
        unsigned variant.
      - Functions returning `int` whose result is then stored back to memory — should emit
        MEMSET32/STIDX32 (truncating store), not STIDX64.

---

29. ✅ Tailored runtime test programs + minimal soft-FP runtime
    - **Five new programs** under `runtime/`:
      - `expr.c` — recursive-descent expression evaluator (`+ - * / % ( )` and unary minus,
        precedence + parens).  Stresses deep recursion, char parsing, and large
        if/else ladders (no jump tables on KlaussCPU).
      - `bst.c` — binary search tree workout: insert / find / in-order walk / delete
        (with in-order-successor for two-child nodes) / post-order free.  Stresses the
        libc heap allocator (malloc/free/coalesce) and recursion with pointer args.
      - `crypto.c` — CRC32 (zlib / IEEE 802.3), SHA-256 (FIPS 180-4 vectors for "",
        "a", "abc", and the 56-byte test message), and Base64 encode/decode round-trip
        (RFC 4648 §10 vectors plus a binary round-trip).  Heavy 32-bit modular
        arithmetic, bit rotates, byte-level memory access via static tables.
      - `queens.c` — N-queens backtracker; counts solutions for N=1..9 against
        OEIS A000170 [1, 0, 0, 2, 10, 4, 40, 92, 352] and prints the first N=8 board.
        Deep recursion with backtracking + tight inner loops.
      - `test_fp.c` — `float` arithmetic smoke test (T1–T9): conversions, add/sub/mul/div,
        exact cancellation, comparisons, negation, and a geometric-series partial sum.
    - **`softfp.c`** — minimal IEEE 754 single-precision soft-FP runtime providing the
      compiler-rt-ABI symbols LLVM emits when expanding `float` ops:
      `__floatsisf`, `__floatunsisf`, `__fixsfsi`, `__fixunssfsi`, `__addsf3`, `__subsf3`,
      `__mulsf3`, `__divsf3`, `__negsf2`, `__eqsf2`, `__nesf2`, `__ltsf2`, `__lesf2`,
      `__gtsf2`, `__gesf2`.  Round-to-nearest-even, NORMAL values only — subnormals
      flushed to zero, Inf/NaN not handled (operations on them yield garbage).  Suitable
      for the test program; vendor compiler-rt for full IEEE conformance.
    - **`Makefile`** updated: new per-program rules; `make all` builds every `.bin`.
    - **No backend changes required** — the existing `setOperationAction(ISD::FADD, MVT::f32, Expand)`
      etc. in `KlaussCPUISelLowering.cpp` already drives LLVM to emit libcalls; we just
      provided implementations.

---

## Hardware memory model (authoritative, post Fix-1)

**Physical memory**: little-endian — byte at address X is at `bits[8*(X mod 4)+7 : 8*(X mod 4)]`
of the 32-bit word.  The DataLayout `e` (little-endian) now matches the hardware.

| Instruction | Byte ordering | Use for C strings? |
|---|---|---|
| LDIDX8 / MEMGET8 | lane N = bits[8N+7:8N] (LE) — correct | ✅ YES — `*s++` works |
| STIDX8 / MEMSET8 | lane N = bits[8N+7:8N] (LE) — correct | ✅ YES |
| TXSTRMEMR | scans lane 0 first = lowest address first | ✅ YES — `txstrmemr(s)` works |
| MEMGET32 | first byte at bits[7:0], last at bits[31:24] | use with shift=0,8,16,24 |
| LDIDX64 | 8 bytes, LE lane order | ✅ standard 64-bit load |

---

## Next steps

All five FPGA hardware fixes have shipped in silicon (see
`FPGA_FIXES_HISTORY.md`).  Backend support is in place for jump tables
(`BR_JT` → `MEMGET32` of `EK_Custom32` table → `JMPR_R`), function-pointer
calls (`CALLR_R`), one-instruction frame address materialisation (`ADDI`),
and one-instruction signed sub-word loads (`LDIDX8_S`/`LDIDX16_S`).

### Step 30 — Hardware test: run all .bin programs on board
- hello.bin / adventure.bin / test_64bit.bin
- expr.bin / bst.bin / crypto.bin / queens.bin (post int=32 + tailored tests)
- test_switch.bin (BR_JT → JMPR_R end-to-end, post Fix 2)
- test_fp.bin (links softfp.o)

### Step 31 — Inline assembly support (KlaussCPUAsmParser)
- enables `asm volatile (...)` and `__asm__` blocks in C
- prerequisite for hand-written critical sections (e.g. UART driver primitives,
  context save/restore in a future RTOS port)

### Step 32 — Vendor compiler-rt builtins for full soft-FP conformance
- replace hand-written `softfp.c` with compiler-rt's `addsf3.c` / `mulsf3.c` /
  etc., either by file-vendoring (Option A in the conversation history) or
  full compiler-rt build integration (Option B)
- enables correct subnormal / Inf / NaN handling
- adds double-precision (`__adddf3` etc.) and integer-divide builtins
  (`__udivdi3`, `__umoddi3`) if needed for larger programs

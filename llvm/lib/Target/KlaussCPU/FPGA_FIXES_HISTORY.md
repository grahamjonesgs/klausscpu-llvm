# KlaussCPU FPGA Hardware Fixes — History

**Status: all six fixes shipped in silicon (Fixes 1–5 April 2026, Fix 6 June 2026).** This document is
kept as a record of the hardware/software co-evolution during LLVM-backend
bring-up.  It describes each issue, the workaround the backend or runtime
needed before the fix landed, and the final hardware/software state once the
fix shipped.

The CPU is a 64-bit RISC processor implemented in Verilog/SystemVerilog on a
Digilent Nexys A7 (Artix-7) FPGA.  The LLVM backend targets it as
`klausscpu-unknown-elf` with DataLayout `e-m:e-p:64:64-i64:64-i128:128-n32:64`
(little-endian, 64-bit pointers, C `int` = 32 bits).  16 GPRs (R0–R15),
32-bit PC/SP, 128 MiB RAM.

---

## Fix 1 — Physical memory byte ordering ✅

### Original behaviour

Physical memory was big-endian: within every 32-bit word, the lowest-address
byte sat in `bits[31:24]` (MSByte).  The byte-access instructions (`LDIDX8`,
`STIDX8`, `LDIDX16`, `STIDX16`, `MEMGET8`, `MEMSET8`, `TXSTRMEMR`) used LE-lane
mapping (`lane N = bits[8N+7:8N]`).  These two were inconsistent — every
byte-level read of data placed in memory by the linker produced reversed
4-byte groups.

### Software workaround (Step 26)

`uart_puts` could not use `TXSTRMEMR`.  A new LLVM intrinsic
`__builtin_klausscpu_memget32` was added that called `MEMGET32` directly,
plus manual alignment handling and an MSB-first byte-extraction loop
(shift = 24, 16, 8, 0).  All `libc` string functions (`strcmp`, `strlen`,
`strcpy`, `memcpy`) were broken for `.rodata` strings; T12 of `test_64bit`
failed.

### Final state (Step 27)

Physical memory is little-endian: byte at address X is at `bits[8*(X mod 4)+7
: 8*(X mod 4)]`.  `LDIDX8`/`MEMGET8`/`TXSTRMEMR` and physical memory now agree.

- `uart_puts` is a single line: `__builtin_klausscpu_txstrmemr(s)`.
- `__builtin_klausscpu_memget32` was removed.
- `strcpy`/`strcmp`/`strlen`/`memcpy` work with standard C byte access.
- T12 of `test_64bit` passes.

---

## Fix 2 — JMPR: indirect (register) branch ✅

### Original behaviour

There was no instruction that jumped to an address held in a register.
`JMP/JMPE/...` only took fixed addresses; `CALL_I` was direct-only.

### Software workaround

The LLVM backend set `setMinimumJumpTableEntries(INT_MAX)` and
`setOperationAction(ISD::BRIND, Expand)`, disabling jump tables entirely.
Switch statements compiled to a linear chain of compares.  C function pointers
could not be called.

### Final state

`JMPR rs` — unconditional branch to the address in register `rs` — and
`CALLR rs` (call to register address) are now implemented.

- `KlaussCPUISelLowering.cpp`: `BR_JT` Custom-lowered (`LowerBR_JT` loads a
  4-byte `EK_Custom32` table entry, threads it through ADDR, and emits BRIND);
  `BRIND` is Legal.
- `KlaussCPUISelDAGToDAG.cpp`: `ISD::BRIND` selects to `JMPR_R`.
- Switch statements with enough cases dispatch via jump table in O(1).
- Function-pointer calls work.
- `runtime/test_switch.c` exercises the BR_JT path end-to-end.

---

## Fix 3 — MEMGET32 unaligned access ✅

### Original behaviour

`MEMGET32 rd, rs` aligned the address in `rs` down to the nearest 4-byte
boundary before reading.  An unaligned string pointer caused the read to
return bytes from the previous 4-byte word.

### Software workaround (Step 26 final)

`uart_puts` computed `misalign = addr & 3` and, if non-zero, read the
aligned word containing the start, skipped the first `misalign` bytes of the
result, then continued with aligned reads.  ~10 instructions of prefix
handling per call.

### Final state

`MEMGET32` reads four bytes starting at the exact address in `rs`,
regardless of alignment.  Combined with Fix 1, the unalignment workaround
was removed entirely — `uart_puts` is a one-line `TXSTRMEMR`, and the
`MEMGET32` path is no longer in `uart_stubs.c`.

---

## Fix 4 — ADDI: register + immediate ✅

### Original behaviour

There was no instruction that added a sign-extended 32-bit immediate to a
register.  `ADDSP` only targeted SP; `ADDR` was register-register; `SETR`
loaded an immediate but did not add.

### Software workaround

Taking the address of a local variable required two instructions:
```asm
setr  rd, <frame_offset>
addr  rd, r15, rd
```
`eliminateFrameIndex` detected `SETR rd, <FrameIndex>` and expanded it to
this pair.

### Final state

`ADDI rd, rs, simm32` — `rd = rs + sign_extend(imm32)` — is now in the ISA.
Frame address materialisation is one instruction:
```asm
addi  rd, r15, <frame_offset>
```

- `KlaussCPUInstrInfo.td` defines `ADDI` (RV format, 8-byte encoding).
- `eliminateFrameIndex` emits `ADDI rd, R15, offset`.
- The `ISD::FrameIndex → SETR(TFI)` workaround in `KlaussCPUISelDAGToDAG.cpp`
  was replaced with the standard RISC-V `ADDI(TFI, 0)` pattern.

---

## Fix 5 — Sign-extending byte/halfword loads ✅

### Original behaviour

`LDIDX8`/`LDIDX16` and `MEMGET8`/`MEMGET16` zero-extended their loaded value
into the 64-bit destination register.  There were no sign-extending variants.

### Software workaround

The backend lowered `SEXTLOAD i8/i16` via `SIGN_EXTEND_INREG`, which emitted
`SEXTB`/`SEXTH` after a zero-extending load — two instructions per signed
sub-word load.

### Final state

`LDIDX8_S` and `LDIDX16_S` are in the ISA.  `SEXTLOAD i8`/`SEXTLOAD i16` are
Legal in `KlaussCPUISelLowering.cpp`, and tablegen `Pat` rules match
`(sextloadi8 ...)` / `(sextloadi16 ...)` → `LDIDX8_S` / `LDIDX16_S`.  Signed
sub-word loads are one instruction.

---

## Fix 6 — CMPRV immediate not sign-extended ✅

### Original behaviour

`CMPRV` (compare register vs. `simm32` immediate) did **not sign-extend** its
32-bit immediate to 64 bits before computing the compare flags — it
zero-extended/truncated it.  Any compare against a **negative** immediate was
therefore wrong.  `CMPRR` (register vs. register) was correct, because both
operands are already full 64-bit registers.

Confirmed on silicon (unsigned compare via `JMPULT`):
```
val = 0xFFFFFFFFFFFFFFC6   (sign-extended -58)
CMPRV val, -10  → ult = 0  (WRONG: -10 compared as 0x00000000FFFFFFF6)
CMPRR val, r    → ult = 1  (correct: r = 0xFFFFFFFFFFFFFFF6)
```
`0xFFFFFFFFFFFFFFC6 <u -10` must be true.

### Symptom / software impact

Exposed by the `int`=32 migration (Step 28): `ctype` range checks compile to
`(unsigned)(c - K) <u N`, which LLVM canonicalises into forms like
`cmprv (c-58), -10 ; jmpult`.  For any char `< '0'` (notably `'\0'`) the negative
immediate was mis-handled, so `isdigit('\0')` returned true and **`strtol` ran
off the end of the string**, returning garbage.  That broke `net_addr_pton`,
`getaddrinfo` (numeric port parse), and the net-shell `parse_arg` — so
`net ping <ip>`, `ntp`, and any IPv4-literal / connect-by-port path failed, while
DNS-by-hostname and every reg-reg compare worked.  It stayed hidden because the
backend almost always materialises constants with `SETR/SETR64` + `CMPRR`; only
an optimiser fold emitted `cmprv` with a negative immediate.

### Software workaround (interim)

Bypassed the broken `strtol` at two call sites in the vendored Zephyr tree:
`net_addr_pton` (manual IPv4 octet parse) and `getaddrinfo` (manual port parse).
Both reverted once the hardware fix shipped.

### Final state (June 2026)

`CMPRV` sign-extends its `simm32` immediate to 64 bits before the comparison,
matching the `SETR`+`CMPRR` reference for all flags (`equal / less / ult / sign`).
Verified on hardware: `strtol("192")==0xc0`, `strtol("0")==0`, and
`CMPRV (sx -58) <u -10 == 1`.  No backend change required; the interim
`net_addr_pton` / `getaddrinfo` workarounds were reverted to vanilla.

---

## Summary

| # | Fix | Status |
|---|---|---|
| 1 | LE physical memory (byte lane ordering) | ✅ shipped |
| 2 | JMPR / CALLR indirect branch & call | ✅ shipped |
| 3 | MEMGET32 unaligned read | ✅ shipped |
| 4 | ADDI rd, rs, imm32 | ✅ shipped |
| 5 | LDIDX8_S / LDIDX16_S sign-extending loads | ✅ shipped |
| 6 | CMPRV sign-extend `simm32` immediate | ✅ shipped (Jun 2026) |

---

## Architecture reference (post-fixes)

| Property | Value |
|---|---|
| GPRs | R0–R15, 64-bit; R15 = frame pointer |
| PC / SP | 32-bit (128 MiB RAM) |
| DataLayout | `e-m:e-p:64:64-i64:64-i128:128-n32:64` |
| C type model | `int`=32; `long`/`long long`/`void*`=64 |
| Calling convention | Args: R0–R3; Return: R12; Callee-saved: R4–R7, R15 |
| Code load address | 0x00000020 (first 32 bytes are heap header) |
| Stack top | 0x08000000 (set by hardware reset) |
| ELF machine | `EM_KLAUSSCPU = 0x4B43` |
| LLVM target triple | `klausscpu-unknown-elf` |

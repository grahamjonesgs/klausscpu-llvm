# KlaussCPU FPGA Hardware Fixes Required

This document is a briefing for the FPGA/Verilog design session.  It describes
five hardware issues discovered during LLVM backend development and hardware
bring-up, ordered by impact.  Each section describes the current hardware
behaviour, the required behaviour, and what the software change looks like once
the fix is in place.

The CPU is a 64-bit RISC processor implemented in Verilog/SystemVerilog on a
Digilent Nexys A7 (Artix-7) FPGA.  The LLVM backend targets it as
`klausscpu-unknown-elf` with DataLayout `e-m:e-p:64:64-i64:64-i128:128-n32:64`
(little-endian, 64-bit pointers, C `int` = 32 bits).  16 GPRs (R0–R15),
32-bit PC/SP, 128 MiB RAM.

---

## Fix 1 — Physical memory byte ordering (CRITICAL, highest impact)

### Current behaviour (broken)

Physical memory is **big-endian**: within every 32-bit word, the
lowest-address byte occupies `bits[31:24]` (the MSByte).

So for a C string `"Hello"` placed at address `0x3000` by the linker:

```
Physical word at 0x3000:
  bits[31:24] = 'H'   ← address 0x3000
  bits[23:16] = 'e'   ← address 0x3001
  bits[15:8]  = 'l'   ← address 0x3002
  bits[7:0]   = 'l'   ← address 0x3003
```

The byte-access instructions (`LDIDX8`, `STIDX8`, `LDIDX16`, `STIDX16`,
`MEMGET8`, `MEMSET8`, `TXSTRMEMR`) were fixed in April 2026 to use
**LE-lane mapping**: lane N = `bits[8N+7:8N]`.  This means:

- `LDIDX8(0x3000)` reads lane 0 = `bits[7:0]` = **'l'** (wrong — should be 'H')
- `TXSTRMEMR(0x3000)` transmits 'l', 'l', 'e', 'H' — reversed within each 4-byte group

The byte-lane mapping is LE but the physical memory layout is BE.  These are
inconsistent.  The result is that **every byte-level memory operation produces
wrong results** for data placed in memory by the linker/compiler.

### Consequences observed in software

- `uart_puts` required a major workaround: a new LLVM intrinsic
  `__builtin_klausscpu_memget32` that calls `MEMGET32` directly, plus manual
  alignment handling, reading 32-bit words and extracting bytes MSB-first
  (shift = 24, 16, 8, 0).
- `TXSTRMEMR` cannot be used for C strings at all.
- `strcpy(buf, "hello")` reads the null terminator **one byte late** (null is
  at physical `bits[23:16]` but LE-lane reads `bits[15:8]` at that address),
  so it copies 6 chars instead of 5.  `strlen` and `strcmp` both give wrong
  results.  This is why **T12 strings FAILS** in `test_64bit`.
- All libc string functions (`strcmp`, `strlen`, `strcpy`, `memcpy`) are
  broken for string literals from `.rodata`, because `.rodata` bytes are placed
  in BE order by the linker but read in LE-lane order by the hardware.

### Required behaviour

Change physical memory to **little-endian**: within every 32-bit word, the
lowest-address byte occupies `bits[7:0]` (the LSByte).

```
Physical word at 0x3000 (after fix):
  bits[7:0]   = 'H'   ← address 0x3000  (was bits[31:24])
  bits[15:8]  = 'e'   ← address 0x3001
  bits[23:16] = 'l'   ← address 0x3002
  bits[31:24] = 'l'   ← address 0x3003
```

This makes LE-lane `LDIDX8(0x3000)` = lane 0 = `bits[7:0]` = 'H' — correct.

### What this means for the Verilog

The fix is in the **memory controller / bus interface** — the byte-enable and
data-lane wiring for 8-bit and 16-bit read/write operations.  For 32-bit and
64-bit aligned accesses the data is returned as a full word and the ordering
question does not arise (both BE and LE return the same 32/64-bit register
value for aligned word reads).  The change is only in how the memory subsystem
maps **sub-word byte addresses to bit positions** within the 32-bit word.

Byte-enable signals for writes must also change: `STIDX8` at address X should
assert byte-enable `1 << (X mod 4)` (LE lane), not `1 << (3 - X mod 4)`.
This is already the post-April-2026 behaviour for `STIDX8`/`STIDX16` — the
write side is correct.  The **read side** (how the memory returns data for a
byte-addressed read) needs to match.

### What changes in the LLVM backend / runtime after the fix

- `uart_puts` becomes a single line: `__builtin_klausscpu_txstrmemr(s)`
  (TXSTRMEMR transmits in address order, which is now correct).
- The `__builtin_klausscpu_memget32` intrinsic, the alignment prefix code, and
  the `MEMGET32`-based extraction loop in `uart_stubs.c` can all be removed.
- `strcpy`, `strcmp`, `strlen`, `memcpy` in `libc.c` work correctly without
  any changes — they already use the standard C idiom.
- `MEMGET32` used for `uart_puts` would still work, but shift order changes:
  first byte is now at `bits[7:0]` so shifts would be 0, 8, 16, 24 (or just
  use `TXSTRMEMR`).
- `test_64bit` T12 strings will pass.

---

## Fix 2 — Add JMPR: indirect (register) branch instruction

### Current behaviour (missing instruction)

There is no instruction that jumps to an address held in a register.  The
available branch instructions are:

- `JMP label` — unconditional branch to a fixed address
- `JMPE/JMPNE/...` — conditional branches to a fixed address
- `CALL_I label` — direct call to a fixed address
- `RET` — return (pops return address)

### Consequences observed in software

The LLVM backend has to set `setMinimumJumpTableEntries(INT_MAX)` to **disable
jump tables entirely**.  Switch statements with many cases are compiled to a
linear chain of comparisons (O(N) in the number of cases) instead of a jump
table dispatch (O(1)).  For the text adventure game with 10+ commands this
makes switch statements significantly slower.

Function pointers also cannot be called efficiently — they require a CALL
instruction that the backend currently does not support for register targets.

### Required behaviour

Add `JMPR rs` — unconditional branch to the address in register `rs`.

Suggested encoding (3-reg / RR1 format):
```
bits[31:8] = opcode (choose an unused 24-bit opcode, e.g. 0x000090)
bits[7:4]  = 0 (unused, no destination register)
bits[3:0]  = rs
```

Optionally also add `CALLR rs` — call to address in register (push return
address, jump to `rs`).  This is needed for C function pointers.

### What changes in the LLVM backend after the fix

- Remove `setMinimumJumpTableEntries(INT_MAX)` and
  `setOperationAction(ISD::BRIND, Expand)` from `KlaussCPUISelLowering.cpp`.
- Add `JMPR` and optionally `CALLR` instruction definitions to
  `KlaussCPUInstrInfo.td`.
- Wire `ISD::BRIND` → `JMPR` in `KlaussCPUISelDAGToDAG.cpp`.
- Wire `ISD::CALL` with register target → `CALLR`.

---

## Fix 3 — MEMGET32 unaligned access

### Current behaviour

`MEMGET32 rd, rs` appears to **align the address in `rs` down to the nearest
4-byte boundary** before reading.  If `rs` points to a string that starts at
byte offset 1, 2, or 3 within a 32-bit word, `MEMGET32` reads from the aligned
word below the pointer, returning bytes that belong to the previous string.

### Consequences observed in software

`uart_puts` has to compute `misalign = addr & 3` and, if non-zero, read the
aligned word containing the string start, skip the first `misalign` bytes of
the result, then continue with aligned reads.  This adds ~10 instructions of
prefix handling to every `uart_puts` call.

### Required behaviour

`MEMGET32 rd, rs` reads the **4 bytes starting at the exact address in `rs`**,
regardless of alignment.  The hardware either:

- Performs a true unaligned 32-bit read (reads across a word boundary if
  necessary), or
- Returns the 4-byte window `[rs, rs+1, rs+2, rs+3]` correctly assembled from
  one or two physical words.

**Note**: if Fix 1 (LE physical memory) is implemented, `uart_puts` can simply
use `LDIDX8` for sequential byte reads and the unaligned `MEMGET32` issue
becomes irrelevant for string output.  Fix 3 is then low priority.

### What changes in the LLVM backend / runtime after the fix

- The alignment prefix block in `uart_puts` can be removed — the main loop
  `while (1) { word = memget32(p); ...; p += 4; }` works for any starting
  address.

---

## Fix 4 — Add ADDI: register + immediate instruction

### Current behaviour (missing instruction)

There is no instruction that adds a sign-extended 32-bit immediate to a
register and stores the result in a (possibly different) destination register.
The available options are:

- `ADDSP imm32` — add immediate to SP only (not a GPR destination)
- `ADDR rd, rs1, rs2` — register + register
- `SETR rd, imm32` — load 32-bit immediate into `rd` (does not add)

### Consequences observed in software

Taking the address of a local variable (e.g. `int x; foo(&x)`) requires **two
instructions**:

```asm
setr  rd, <frame_offset>    ; rd = offset of x from frame pointer
addr  rd, r15, rd           ; rd = R15 + offset  (frame address)
```

`eliminateFrameIndex` in `KlaussCPURegisterInfo.cpp` detects a
`SETR rd, <FrameIndex>` machine node and expands it to this two-instruction
sequence.  With `ADDI`, this would be one instruction:

```asm
addi  rd, r15, <frame_offset>
```

### Required behaviour

Add `ADDI rd, rs, simm32` — `rd = rs + sign_extend(imm32)`.

Suggested encoding (new 8-byte RV format, similar to existing immediate
instructions):
```
Word 0:
  bits[31:16] = upper 16 of opcode (choose unused, e.g. 0x0001)
  bits[15:8]  = lower 8 of opcode
  bits[7:4]   = rd
  bits[3:0]   = rs
Word 1 (bytes 4–7):
  bits[31:0]  = simm32 (signed 32-bit immediate)
```

### What changes in the LLVM backend after the fix

- Add `ADDI` instruction to `KlaussCPUInstrInfo.td`.
- Simplify `eliminateFrameIndex` in `KlaussCPURegisterInfo.cpp` to emit a
  single `ADDI rd, R15, offset` instead of `SETR + ADDR`.
- Remove the `ISD::FrameIndex → SETR(TFI)` special-case workaround in
  `KlaussCPUISelDAGToDAG.cpp` and replace with `ADDI(TFI, 0)` (the standard
  RISC-V pattern).

---

## Fix 5 — Add sign-extending byte/halfword load variants

### Current behaviour

`LDIDX8` and `LDIDX16` zero-extend the loaded byte/halfword into the 64-bit
destination register.  There are no sign-extending variants.  `MEMGET8` and
`MEMGET16` also zero-extend.

### Consequences observed in software

Every `int8_t` or `int16_t` load that needs sign-extension requires two
instructions:

```asm
ldidx8  rd, rs, offset    ; zero-extend byte into rd
sextb   rd                ; sign-extend bits[7:0] → bits[63:0]
```

The backend currently expands `SEXTLOAD` and uses `SIGN_EXTEND_INREG` which
lowers to `SEXTB`/`SEXTH` for i8/i16 (Legal) and a shift pair for i32 (since
`SEXTW` exists separately).

### Required behaviour

Add sign-extending variants:
- `LDIDX8_S  rd, rs, simm32` — load byte, sign-extend to 64 bits
- `LDIDX16_S rd, rs, simm32` — load halfword, sign-extend to 64 bits

These can reuse the same RRV encoding as `LDIDX8`/`LDIDX16` with a different
opcode.

### What changes in the LLVM backend after the fix

- Add `LDIDX8_S`, `LDIDX16_S` to `KlaussCPUInstrInfo.td`.
- Change `SEXTLOAD i8/i16` from `Expand` to `Legal` in
  `KlaussCPUISelLowering.cpp`.
- Add tablegen `Pat` patterns matching `(sextloadi8 ...)` and
  `(sextloadi16 ...)` → `LDIDX8_S`/`LDIDX16_S`.
- Low priority — the current two-instruction sequence is functionally correct.

---

## Summary table

| # | Fix | Verilog complexity | Impact |
|---|---|---|---|
| 1 | LE physical memory (byte lane ordering) | Medium | Eliminates uart_puts workaround, fixes T12 strings, fixes all libc string functions |
| 2 | JMPR indirect branch | Low (new instruction) | Enables jump tables (O(1) switch), function pointers |
| 3 | MEMGET32 unaligned read | Low (memory bus change) | Simplifies uart_puts; low priority if Fix 1 done |
| 4 | ADDI rd, rs, imm32 | Low (new instruction) | 2→1 instruction for frame address materialisation |
| 5 | LDIDX8_S / LDIDX16_S sign-extending loads | Low (new opcodes) | 2→1 instruction for signed byte/halfword loads |

**Fix 1 is by far the most important.** It resolves the fundamental
inconsistency between the linker's byte-address model and the hardware's
byte-lane mapping, and unblocks correct operation of all C string handling
without any workarounds in the compiler or runtime library.

---

## Architecture reference

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

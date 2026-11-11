# KlaussCPU C ABI (version 3)

This is the calling convention of KlaussCPU code built with this toolchain:
what the backend (`KlaussCPUCallingConv.td`, `KlaussCPUISelLowering.cpp`),
clang (`clang/lib/CodeGen/Targets/KlaussCPU.cpp`) and the hand-written
assembly in klausscpu-runtime agree on.  The numbers shared by the tools
(registers, offsets, the version) live in
`llvm/include/llvm/TargetParser/KlaussCPUABI.h`.  The model is RISC-V LP64
(integer only) with KlaussCPU's register file and frame instructions.

Version 2 replaced the original convention (version 0) in phase 4 of the
backend work.  Version 3 (phase 9) keeps version 2's calling convention and
makes `__int128` 8-aligned (see "`__int128` alignment"), which changes the
layout of every struct holding one.  The versions do not mix: every object records its version in
`e_flags`, and lld refuses to link code of different versions (see
"Versioning").  Rebuild everything (picolibc, the compiler-rt builtins,
wolfSSL/wolfSSH, Zephyr, the programs) with the new toolchain.

## Summary

| | Versions 2 and 3 | Version 0 (before) |
|---|---|---|
| Argument registers | R0–R3 | R0–R3 |
| Return registers | R12, then R11 (second half of a 9–16 byte value) | R12, R11 (i128 only) |
| Callee-saved | R4–R7, R15 (frame pointer), SP | same |
| Caller-saved | R0–R3, R8–R14, FLAGS | same |
| `int`/`unsigned` argument or result | sign-extended to 64 bits by the producer | upper half undefined |
| `char`/`short`/`_Bool` | extended by signedness (zero for `_Bool`) | same (clang already did) |
| Aggregate ≤ 16 bytes | in 1–2 GPRs / stack slots, returned in R12/R11 | by reference (copy), sret |
| Aggregate > 16 bytes | by reference to a caller-made copy; returned through sret | same |
| Call frame of a non-variadic call | its stack arguments only (often nothing) | 24-byte home area + stack arguments |
| Variadic call | 32-byte home area for R0–R3, then stack arguments | 24-byte home area for R1–R3 (every call) |
| `f(...)` with no named parameter | supported | diagnosed |
| `va_list` | `void *` walking 8-byte slots | same |

## Registers

| Register | Role |
|---|---|
| R0–R3 (A–D) | arguments 0–3, caller-saved |
| R4–R7 (E–H) | callee-saved; R7 is also the base pointer of a function that realigns its stack |
| R8–R11, R13, R14 | temporaries, caller-saved; R11 is the second return register |
| R12 (M) | return value (bits 63:0) |
| R15 (P) | frame pointer, saved by the prologue (`ENTER`) of every function that has a frame; a function without one leaves it alone |
| SP | stack pointer (32-bit, separate register), 8-byte aligned at calls |
| FLAGS | clobbered by calls |

A call's register mask (`CSR_KlaussCPU_Call`) preserves R4–R7, R15 and SP;
the call instructions themselves only `Def` FLAGS.  (Version 0 also listed
every caller-saved GPR as an implicit def of the call instructions, which
made the register mask meaningless: any change of the callee-saved set needs
the mask.)

## Scalars

- Every argument and return value occupies a 64-bit GPR or an 8-byte stack
  slot.
- Integers narrower than 64 bits are extended to 64 bits by the producer —
  the caller for an argument, the callee for a return value — and the
  consumer may rely on it (IR `signext`/`zeroext`, which become
  `AssertSext`/`AssertZext`):
  - every 32-bit integer is **sign-extended, also `unsigned int`** (RV64's
    rule): the W instructions (`ADDW`, `ADDIW`, `MULW`, …) and `LDIDX32_S`
    produce this form, and an unsigned 32-bit compare or zero-extension
    works on it unchanged (sign extension preserves the unsigned order of
    32-bit values);
  - 8- and 16-bit integers are extended by their signedness, `_Bool`
    zero-extended.
- Pointers, `long`, `long long` are 64-bit; `__int128` is two GPRs (low
  half first) or two slots, never aligned beyond 8.
- `float` (soft float) is passed in the low 32 bits of a GPR with the upper
  half undefined; `double` and `long double` (also 64-bit) in a GPR.
- The backend's own library calls follow the same rules
  (`shouldSignExtendTypeInLibCall`: an i32 is sign-extended; a softened
  `float` is not extended; a softened `__fp16` is zero-extended, because
  without `_Float16` compiler-rt's half-precision functions take a
  `uint16_t`), and so do the library calls the optimiser
  creates (`TargetLibraryInfo` marks i32 parameters and results `signext`
  for klausscpu, as for riscv64).
- The `__sync_*_N` atomics libcalls are the exception: the type legaliser
  promotes the operation to i64 before it becomes a call, so their 1/2/4-byte
  values go in any-extended and come back zero-extended; the runtime's
  `src/atomic.c` takes and returns `uint64_t` and truncates itself.

## Aggregates

Structs, unions, `_Complex` values and GNU vectors (`isAggregateTypeForABI`
or a vector type in clang):

- **Up to 16 bytes**: passed directly, coerced to an integer of the
  aggregate's size (≤ 8 bytes, e.g. `i24` for a 3-byte struct), to `i128` if
  it is 16-byte aligned (only an explicit `aligned(16)`/`_Alignas(16)` gives
  that since version 3), or else to `[2 x i64]`; a struct holding an
  `__int128` is `[2 x i64]`.  The registers are the same either way: one or two GPRs; an
  argument of two parts can take R3 and the first stack slot.  Returned in R12
  (bytes 0–7) and R11 (bytes 8–15).  Bytes are in memory order (little
  endian), padding bytes are undefined.
- **Larger**: passed as a pointer to a copy the caller makes (clang
  `getNaturalAlignIndirect(..., ByVal=false)`; the callee may modify it), and
  returned through a hidden pointer argument (`sret`, in R0) to memory the
  caller provides; R12 is undefined after such a call.
- A record of size 0 (GNU C `struct {}`) takes no register or slot; a C++
  empty class has size 1 and is passed as an `i8`.  A C++ class with a
  non-trivial copy constructor or destructor is always passed by reference.
- IR from other frontends: `byval` arguments still work (the backend passes
  a pointer to a copy it makes), and the backend demotes a return value that
  does not fit R12/R11 to `sret` (`CanLowerReturn`).

## Stack frame and stack arguments

CALL pushes the return address: at the callee's entry `[SP]` is the return
address and `SP + 8` is the caller's SP.  Stack arguments are 8-byte slots
in order, from:

- `[entry SP + 8]` for a non-variadic call (`IncomingArgOffset`); the caller
  stores them from its own `SP + 0` (`OutgoingArgOffset`);
- `[entry SP + 40]` for a variadic call: the caller first reserves the
  32-byte home area of R0–R3 at `[entry SP + 8 .. + 39]`
  (`VarArgHomeSize`).

A call without stack arguments that is not variadic reserves no call frame.
The prologue (`ENTER`) pushes R15 below the return address and points R15 at
it, so the incoming stack arguments are at `R15 + 16 + n×8` (variadic:
`R15 + 48 + n×8`).

### Frames, frameless functions and tail calls

None of this changes what a caller or callee may assume; it is how this
compiler builds frames, which the unwinders depend on.

- Every function that calls another has a frame: `ENTER`, so `[R15]` is the
  caller's R15 and `[R15 + 8]` the return address.  The R15 chain seen from
  any call site is therefore complete (`__builtin_frame_address(N)` walks
  it).  The exception is the MachineOutliner (-Oz, or `-moutline`): its
  `OUTLINED_FUNCTION_<n>` have no frame and use no stack beyond the 8-byte
  return address their CALL pushes (CFA = SP+8, the return address at
  `[SP]`), and a frameless function may call one; seen from inside such a
  function the R15 chain skips the frameless caller.  So the worst-case
  stack depth of -Oz code is 8 bytes deeper wherever an outlined function
  is called (a frameless leaf that now calls one: size thread stacks with
  that in mind).  An
  outlined epilogue (it ends in `LEAVERET`) is jumped to, and runs in the
  frame of the function it came from (its CFI says so): it returns to that
  function's caller, as a tail call does.  None of this is visible to code
  outside the module: outlined functions are internal and follow no
  calling convention.
- A leaf without stack objects has no frame: R15 still holds its caller's
  frame and `[SP]` the return address throughout.  clang's default is
  `-fno-omit-frame-pointer -momit-leaf-frame-pointer`
  (`"frame-pointer"="non-leaf"`); `-mno-omit-leaf-frame-pointer`
  (`"frame-pointer"="all"`) gives every function a frame from its entry to
  its return (it also turns shrink-wrapping off).  R15 is never allocated.
- Shrink-wrapping (-O1 and up, but not at -Os/-Oz) can set the frame up
  after the entry and tear it down before the return, so code on a path
  that needs no frame runs frameless.
- A sibling call whose arguments are all in registers (to a function that
  is not variadic, without byval or sret arguments, none of them the
  address of the caller's own stack object such as the copy of an aggregate
  over 16 bytes) can be a tail call: the
  caller tears its frame down (`LEAVE`) and jumps; the callee is entered
  with the caller's return address at `[SP]` and returns to the caller's
  caller.  So a callee must not assume that `[SP]` at entry points into its
  own caller's code: it points where the callee will return.
- Unwinding: the CFI (`.debug_frame` with `-g`) describes every instruction
  (CFA = SP+8 in a frameless stretch, R15+16 in a frame), and lldb uses it.
  Without CFI, lldb's fallback plan (the ABI plugin) follows the R15 chain,
  which is right for every frame that made a call; a frame 0 without a frame
  is then shown as its caller's frame (the caller itself is skipped).  A frame
  entered by an interrupt rather than a call breaks that rule: the hardware
  pushes one IRET word and saves nothing anywhere else, and the handler pushes
  as many registers as its own code does, so no fixed rule finds the
  interrupted PC or SP.  An interrupt handler needs CFI to be unwound through
  (the Zephyr and FreeRTOS ones have it: `.cfi_signal_frame`, every saved
  register described).  The function it interrupted is then unwound with its
  own CFI, which covers every instruction; without CFI, that function is
  unwound with the R15 chain too, so if it had no frame at the interrupted
  instruction (a leaf, or outside a shrink-wrapped frame) it is shown and its
  caller is skipped, as for a frame 0 without a frame.  A
  function left by a tail call is not on the stack (with `-g`, the
  `DW_AT_call_tail_call` call sites let lldb show it as an artificial frame).
- Crash dumps (hardware `CRASH_DUMP.md`: the registers and `S0`–`S3`, the
  doublewords at SP): if the faulting function has no frame (PC in a leaf
  whose disassembly starts without `ENTER`, or before a shrink-wrapped
  `ENTER`), the return address is `S0` unless the leaf pushed something
  (it does not: it has no stack objects), and R15 is its caller's frame
  record; otherwise the return address is `[R15 + 8]`.  The chain from
  R15 on is complete either way.  A PC in an `OUTLINED_FUNCTION_<n>`
  (-Oz) is a frameless function in this sense, unless that function ends in
  `LEAVERET` (an outlined epilogue): then it is in the frame of the function
  that jumped to it, and the return address is `[R15 + 8]`.  klausscc prints
  dumps raw and has no unwinder to update.

## Variadic functions

The unnamed arguments are assigned exactly like named ones (R0–R3, then the
stack).  The caller of a variadic function reserves the home area; the
variadic callee stores the argument registers that hold unnamed arguments in
their home slots, `Rn` at `[entry SP + 8 + n×8]`, which puts every unnamed
argument in consecutive 8-byte slots ending where the stack arguments begin.
`va_list` is a `void *` to the next slot: `va_start` is the first unnamed
argument's slot, and `va_arg` reads the value in place and advances by its
size rounded up to 8 (never realigned; an aggregate over 16 bytes is a slot
holding its address).  Clang emits `va_arg` as IR
(`emitVoidPtrVAArg`); the backend's `LowerVAARG` handles the IR `va_arg` of
other frontends.  Because R0 has a home slot too, a function with no named
parameter (C23 `int f(...)`) works.

Calling a variadic function through a non-variadic prototype (or the
reverse) is undefined behaviour in C and breaks with this ABI: the call
would not reserve the home area.

A call through a declaration without a prototype (K&R `long f();`, valid C
up to C17) is made as a variadic call when all its arguments, counted in
GPR words with the `sret` pointer, fit in R0–R3
(`isNoProtoCallVariadic` in clang): it reserves the home area, so a
variadic callee (undefined behaviour, but it worked with version 0 and
works on RISC-V and x86-64) stores its register arguments there instead of
over the caller's frame, and a non-variadic callee, which then reads
nothing from the stack, is unaffected.  A call with stack arguments stays
non-variadic: they are where a non-variadic callee, the only kind C allows
here, expects them, and a variadic callee would read them 32 bytes too
high.

## `__int128` alignment (version 3)

`__int128`, `unsigned __int128` and LLVM `i128` are 8-aligned: the DataLayout
(`TargetDataLayout.cpp`, `e-m:e-p:64:64-i64:64-n32:64`) has no `i128` entry,
so LLVM takes the alignment of the largest integer entry (`i64:64`); clang
sets `Int128Align = 64`; the Rust target spec has the same data layout and
rustc takes the `i128`/`u128` alignment from it.  Version 2 had `i128:128`,
which disagreed with everything else on the target (the ABI stack is 8-aligned,
malloc guarantees 8, `__BIGGEST_ALIGNMENT__` and `max_align_t` are 8): the
optimiser could assume a heap `__int128 *` was 16-aligned, and every function
holding an `__int128` in memory reserved R7 and realigned its frame.  Decided
rule by rule:

- **Layout**: a struct of a `char` and an `__int128` has the latter at offset
  8 (was 16); `struct { __int128; char; }` is 24 bytes (was 32).
- **Argument and return registers**: unchanged.  An `__int128` was already
  two i64 parts that may take R3 and the first stack slot, with no even-register
  rule, so alignment never mattered.  Returned in R12 (low) and R11 (high).
- **Aggregates up to 16 bytes**: coerced to `i128` only if the aggregate is
  16-byte aligned, i.e. through an explicit attribute; a struct holding an
  `__int128` is now `[2 x i64]`.  Same registers, a different IR type; clang
  and the rustc module apply the same test, so they agree.
- **`va_arg`**: slots are 8 bytes and never realigned, as before; an
  `__int128` takes two slots.  An explicitly 16-aligned aggregate also takes
  its size in slots with no padding (`AllowHigherAlign = false`).
- **Explicit alignment** (`aligned(16)`, `_Alignas(16)`) is still honoured,
  with the frame realignment (R7) it needs.
- **Atomics**: 16-byte atomics are generic `__atomic_*` libcalls, which the
  runtime does not provide (`src/atomic.c`; the link fails, with either
  alignment).

## Thread-local storage

`__thread` / `_Thread_local` exist for Linux user programs
(`klausscpu-*-linux-*`; bare metal sets up no thread pointer and clang rejects
them there).  This section is the static executable, where the only TLS model
is local exec, whatever the source or `-ftls-model` asks for; "Dynamic
linking" below adds initial exec and general dynamic for PIE and shared
libraries, on the same layout and thread pointer:

- **Thread pointer**: TP, system register 7 (`mfsr rd, tp`), user readable and
  writable.  The kernel switches it with the thread and sets it on
  `clone(CLONE_SETTLS)`.
- **Layout**: TLS variant II, musl's default (no `TLS_ABOVE_TP`), as on
  x86-64: TP points at the thread's `struct pthread`, aligned to the TLS
  segment's `p_align`; the static TLS block ends just below it, padded so that
  its start is congruent to `p_vaddr` modulo `p_align`.  A variable's offset
  from TP is negative: `S - p_memsz - ((-p_vaddr - p_memsz) & (p_align - 1))`
  with S its offset in the segment (lld `getTlsTpOffset`; musl
  `static_init_tls`).
- **Code**: `mfsr rd, tp` then `addi rd, rd, %tprel(var)` for the address, or
  the offset in the access itself (`ldidx32 rd, rtp, %tprel(var+4)`).
  `%tprel` is the 32-bit signed relocation `R_KLAUSSCPU_TPREL32` (7) in the
  word-1 immediate of `addi`, `setr` and the indexed loads and stores.  A
  static executable has no dynamic TLS relocations.

## Dynamic linking

**Status: design agreed** (milestone M1 of klausscpu-runtime
`DYNLINK_PLAN.md`, reviewed); **compiler and assembler implemented** (M2:
clang's `dso_local` rule, the backend's code generation, MC's specifiers
and relocations 8–11, `KlaussCPU.def` 8–18); **lld and the driver
implemented** (M3: `-shared`, `-pie`, the GOT, PLT, TLS and dynamic
relocations below, `lld/test/ELF/klausscpu-dynlink-*.s`); the runtime
(musl's dynamic linker, M4) and lldb not yet.  It defines
shared libraries and position-independent executables (PIE) for
`klausscpu-*-linux-*` with musl's dynamic linker,
`/lib/ld-musl-klausscpu.so.1`.  There are no dynamically linked executables
that are not PIE.

The one structural fact: KlaussCPU is **LP64 in ELFCLASS32 files**.  The ELF
structures are the 32-bit ones (`Elf32_Dyn`, `Elf32_Sym`, `Elf32_Rela`,
`Elf32_Phdr`), but **every field the dynamic linker writes is 64 bits**: GOT
entries, `.got.plt` slots, relocated data pointers, TLS offsets and module
IDs.  A C pointer is 8 bytes, and the code loads all of them with
`ldidx64`.  The same split holds for the C structures that describe loaded
modules (`<link.h>`, below).

### What stays the same

- **Everything outside Linux.**  Bare metal, Zephyr (LLEXT), FreeRTOS's PIC
  loader and Rust (`klausscpu-unknown-none-elf`) keep today's `-fPIC`:
  position-independent *code* in which every symbol is local, with absolute
  (`R_KLAUSSCPU_ABS32`) jump tables and `R_KLAUSSCPU_ABS64` pointer
  initialisers that their loaders apply.  The rules below that change code
  are keyed on the Linux triple, in clang and in the backend (the backend
  checks the triple, not only `dso_local`, so IR from other frontends with
  the PIC model does not grow GOT accesses), and output for other triples is
  byte-identical.
- **Non-PIC code**, Linux included: the same code and relocations.  That
  covers the kernel (`klausscpu-unknown-linux-gnu`, `-fno-pic -fno-PIE`) and
  today's static programs.  A future vDSO is Linux PIC code and follows the
  rules below.
- **Static links**: the same lld layout (page-size defaults unchanged,
  below).  What changes is only the code of objects built with `-fPIC` or
  `-fPIE` on Linux ("Code generation"): they use the GOT, and TLS initial
  exec or general dynamic.  They still link statically: lld fills the GOT
  at link time (an address, a TP offset, or module 1 and an offset for
  general dynamic, which needs musl's `__tls_get_addr`, present in the
  static libc too) and emits no dynamic relocations into a static
  executable.
- **The calling convention.**  The PLT's scratch register (R14) is already
  caller-saved, so ABI version 3 stays, with no `e_flags` change.
- **Relocation format**: RELA, `Elf32_Rela` (12 bytes), `r_info = sym << 8 |
  type`, `r_addend` a signed 32-bit value.

### Relocations

Notation: S the symbol's value, A the addend (`r_addend`, an `Elf32_Sword`,
**sign-extended to 64 bits for every type**, static and dynamic), P the
address of the field (`r_offset`), I the address of the instruction the field
belongs to (P − 4 for the word-1 immediate), B the load base of the module,
G the address of the symbol's GOT entry, GT that of its TLS offset GOT entry,
GD that of its general-dynamic GOT pair, L the symbol's PLT entry if it has
one, else S.  TPOFF(S) is the variant II offset from TP of "Thread-local
storage" (negative).

**Static relocations**, new.  Each is the signed 32-bit word-1 immediate of
a 2-word instruction, relative to the instruction itself, as
`R_KLAUSSCPU_PCREL32` is (the hardware's PC is the instruction):

| # | Name | Instruction | Field | Assembler |
|---|---|---|---|---|
| 8 | `R_KLAUSSCPU_GOTPCREL32` | `leapc` | G + A − I | `leapc rD, %gotpcrel(sym)` |
| 9 | `R_KLAUSSCPU_PLT32` | `callrel`, `jmprel`/`tail` (2-word forms) | L + A − I | `callrel %plt(sym)` |
| 10 | `R_KLAUSSCPU_TLS_GOTTPREL32` | `leapc` | GT + A − I | `leapc rD, %gottprel(sym)` |
| 11 | `R_KLAUSSCPU_TLS_GD32` | `leapc` | GD + A − I | `leapc rD, %tlsgd(sym)` |

The four specifiers take a bare symbol (`%gotpcrel(x+8)` and
`%gotpcrel(x)+8` are assembler errors; the assembler writes A = 0): an
offset into the object is added after the load.  A `leapc` with one of them is never compressed to `leapc.s`.

**Existing static relocations in a dynamic link** (lld):

- `R_KLAUSSCPU_CALL18` (6, the 1-word short call and the short tail jump)
  is resolved to L, not S (`R_PLT_PC`): a call to a preemptible function
  goes to its PLT entry.  The range check and the thunk decision use the PLT
  entry's address (`getPltVA`, as ARM's `needsThunk` does), and a thunk
  jumps to the PLT entry.  So calls need no new relocation: the compiler's
  calls are already `callrel`/`tail` with CALL18, and every existing
  object can call into a shared library.  `R_KLAUSSCPU_PLT32` is only for the
  2-word forms (`-mllvm -klausscpu-short-calls=false`, hand-written
  assembly).
- `R_KLAUSSCPU_ABS64` (2): a link-time constant in a static executable; in
  a shared library or PIE, `R_KLAUSSCPU_RELATIVE` for a symbol that cannot
  be preempted, else a dynamic `R_KLAUSSCPU_ABS64` (the dynamic linker's
  symbolic relocation, lld `symbolicRel`).  In a read-only section a dynamic
  relocation is an error ("recompile with -fPIC"); the compiler puts such
  data in `.data.rel.ro` under PIC.
- `R_KLAUSSCPU_PCREL32` (3), `R_KLAUSSCPU_ABS32` (1), `R_KLAUSSCPU_PC32`
  (4), `R_KLAUSSCPU_PC64` (5) against a symbol that can be preempted: an
  error in a shared library ("recompile with -fPIC").  In a PIE, against a
  symbol a shared library defines (code that reached it directly:
  `-fdirect-access-external-data`, hand-written assembly), lld gives a data
  symbol an `R_KLAUSSCPU_COPY` and a function a canonical PLT entry (the
  function's address in the program becomes its PLT entry, the `st_value`
  of its undefined `.dynsym` entry, which the libraries then resolve to;
  musl supports it).  Neither is possible for a **protected** symbol of a
  shared library (lld: "cannot preempt symbol"): an executable reaches those
  only through the GOT, which `-fPIE` code does by default.  There are no
  32-bit dynamic relocations, so an `R_KLAUSSCPU_ABS32` against any
  non-absolute symbol in a shared library or PIE fails too.
- `R_KLAUSSCPU_TPREL32` (7, local exec): executables only, PIE included; an
  error in a shared library (as today) and against a TLS symbol a shared
  library defines ("recompile with -fPIE").

**Dynamic relocations**, in `.rela.dyn` and `.rela.plt` (`DT_JMPREL`).
Every field is 64 bits, little-endian, 8-aligned, computed in 64 bits:

| # | Name | Value | Where |
|---|---|---|---|
| 2 | `R_KLAUSSCPU_ABS64` | S + A | data pointers to symbols that may be preempted (existing type) |
| 12 | `R_KLAUSSCPU_COPY` | `st_size` bytes copied from the definition | PIE: library data reached directly (above) |
| 13 | `R_KLAUSSCPU_GLOB_DAT` | S + A | GOT entries |
| 14 | `R_KLAUSSCPU_JUMP_SLOT` | S + A | `.got.plt` slots (the only `.rela.plt` type) |
| 15 | `R_KLAUSSCPU_RELATIVE` | B + A | anything that only moves with the module |
| 16 | `R_KLAUSSCPU_TPOFF64` | TPOFF(S) + A | initial-exec GOT entries (negative) |
| 17 | `R_KLAUSSCPU_DTPMOD64` | module ID of S's module | first entry of a GD pair |
| 18 | `R_KLAUSSCPU_DTPOFF64` | S + A, S the offset in its module's TLS segment (no bias) | second entry of a GD pair |

S, an address, is zero-extended; A is sign-extended (a negative addend
occurs: a `RELATIVE` for `&arr[-1]` with `arr` at the start of a PIE).  User
addresses lie below `0x8000_0000` (Linux `TASK_SIZE`), so the result for an
address is the 64-bit value `leapc` or `setru` builds for it.  Module IDs are
musl's: the executable is module 1 if it has a `PT_TLS`.

Not provided: `IRELATIVE` (musl has no IFUNC; `STT_GNU_IFUNC` is a link
error), TLSDESC, local-dynamic relocations (local dynamic is lowered as
general dynamic), 32-bit dynamic relocations, REL, and RELR: ELF32 RELR
(`-z pack-relative-relocs`) describes 4-byte words, so lld rejects it for
KlaussCPU.

### GOT

- Entries are 8 bytes, 8-aligned (lld `gotEntrySize = 8`; x86-64's ELF32
  x32 ABI does the same), holding an address zero-extended to 64 bits
  (`GLOB_DAT`, `RELATIVE`, or a link-time constant), a sign-extended TP
  offset (`TPOFF64`), or for general dynamic a pair {module ID, offset}
  (`DTPMOD64`, `DTPOFF64`), which is musl's `tls_mod_off_t` of two
  `size_t`.
- No reserved entries in `.got` or `.got.plt`: there is no lazy binding, so
  nothing reads GOT[0..2].  `.got.plt` slots hold 0 in the file; their
  `JUMP_SLOT`s fill them at load.  `DT_PLTGOT` is the start of `.got.plt`.
  lld defines `_GLOBAL_OFFSET_TABLE_` (its default place), but no code
  sequence, PLT entry or loader uses it.  The dynamic linker finds its own
  `_DYNAMIC` with `leapc` (a hidden reference).
- Bound at load and read-only afterwards: `-z now` is lld's default for
  KlaussCPU shared libraries and PIE (not only the driver's), so `.got.plt`
  lies in `PT_GNU_RELRO` and musl never has to write it after `mprotect`, even
  for `RTLD_LAZY`.

### Code generation (`klausscpu-*-linux-*`)

**Which symbols are local.**  On Linux, clang follows the ELF rules:
`-fPIC` makes default-visibility definitions preemptible (not `dso_local`);
`-fPIE` keeps definitions `dso_local` and leaves external declarations not
`dso_local`.  (`shouldAssumeDSOLocal` treats `-fPIC` as `-fPIE` for
KlaussCPU today; that stays for every other OS.)  For `-fPIC` the clang
driver passes `-fhalf-no-semantic-interposition`, also under
`-fno-semantic-interposition`, because its `SupportsLocalAlias` list
(`Clang.cpp`) does not name KlaussCPU, although the backend emits and uses
`.L<sym>$local` aliases on Linux PIC (adding Linux KlaussCPU to that list
would make `-fno-semantic-interposition` work): function definitions are not `dso_local`
either, so calls to them stay CALL18 that lld may send through the PLT,
though the optimiser may still inline them.  (`cc1` on its own defaults to
full `-fno-semantic-interposition`, where function definitions are
`dso_local` and referenced through their local alias.)  Hidden, protected and
`static` symbols are always local.  GlobalMerge merges only `dso_local`
globals, so it follows by itself.  External symbols the backend names itself
(`LowerExternalSymbol`: libcalls such as `memcpy`, whose address it may take)
are not local under Linux PIC.

**Address of a local symbol**: as today,

```
leapc   rD, sym                 # R_KLAUSSCPU_PCREL32 (or leapc.s)
```

**Address of a symbol that is not local** (data, or a function whose address
is taken):

```
leapc   rD, %gotpcrel(sym)      # 2 words, R_KLAUSSCPU_GOTPCREL32
ldidx64 rD, rD, 0               # 1 word: the GOT entry
```

Three words and a load.  The ISA has no PC-relative load, and `leapc` is
cheaper than a GOT base register (a callee-saved register taken in every
function, set up in every prologue).  `sym + off` adds `off` after the load
or folds it into the access (`ldidx32 rX, rD, off`).  The GOT load is
invariant and dereferenceable (its memory operand says so), so MachineCSE
and MachineLICM share and hoist it.

**Calls**: unchanged, `callrel sym` / `tail sym` with
`R_KLAUSSCPU_CALL18`.  lld sends them to the PLT entry when the callee can be
preempted.  With short calls off, `callrel %plt(sym)` / `tail %plt(sym)`
with `R_KLAUSSCPU_PLT32`.  Indirect calls are `callr` as today.

**Jump tables** (`-fPIC`/`-fPIE` on Linux only): entries are 32-bit offsets
from the table (`EK_LabelDifference32`, `R_KLAUSSCPU_PC32` when the table is
not in the function's section), dispatched as `leapc` base, `ldidx32sr`
(r + r, sign-extending), `addr`, `jmpr`: one `addr` more than the absolute
table, which would need a 32-bit dynamic relocation per entry.  Other
triples keep the absolute tables.

**TLS models** (`TargetMachine::getTLSModel`; `-ftls-model` and the
`tls_model` attribute can only choose a faster model):

| | `dso_local` variable | other variable |
|---|---|---|
| no PIC | local exec | local exec (as today) |
| `-fPIE` | local exec | initial exec |
| `-fPIC` | general dynamic (local dynamic lowered as GD) | general dynamic |

So non-PIC code stays byte-identical; it can only be linked into a static
executable or a PIE whose TLS is all its own (a reference to a library's TLS
from local-exec code is an lld error: recompile with `-fPIE`).

Initial exec:

```
leapc   rT, %gottprel(var)      # R_KLAUSSCPU_TLS_GOTTPREL32
ldidx64 rT, rT, 0               # TP offset (negative)
mfsr    rD, tp
addr    rD, rD, rT              # or the access: ldidx32r rX, rD, rT
```

A shared library that uses it (`-ftls-model=initial-exec`) gets
`DF_STATIC_TLS` from lld and, by musl's rule, cannot be `dlopen`ed after
start-up.

General dynamic:

```
leapc   r0, %tlsgd(var)         # R_KLAUSSCPU_TLS_GD32: the GOT pair
callrel __tls_get_addr          # R_KLAUSSCPU_CALL18, through the PLT
                                # R12 = the variable's address
```

`__tls_get_addr` is musl's ordinary C function, `void
*__tls_get_addr(tls_mod_off_t *)`, called with the normal convention (the
call clobbers what any call clobbers).  It returns `dtv[module] + offset`;
the DTP offset has no bias (RISC-V's is 0x800).

**TLS in debug info** stays as today, `DW_OP_const4u` of the variable
(`.long var`, `R_KLAUSSCPU_ABS32`) and `DW_OP_GNU_push_tls_address`: lld
resolves a TLS symbol to its offset in its module's TLS segment, which is
the DTP offset, in shared libraries too.  No new relocation is needed.

**Exception tables**, changed in M2 for all Linux code (PIC or not): the
personality is `indirect|pcrel|sdata4` (through an 8-byte hidden weak
`DW.ref.<personality>` in a writable COMDAT section
`.data.DW.ref.<personality>`, LLVM's generic placement), the LSDA
`pcrel|sdata4`, type info `indirect|pcrel|sdata4` (8-byte `.L<sym>.DW.stub`
in `.data`).  FDE addresses are `pcrel|sdata4` already
(augmentation `zR`, `R_KLAUSSCPU_PC32`).  Today the three fall into LLVM's
default `absptr`, which MC writes as 4 bytes and libunwind reads as 8, so
this also fixes static C++ exception handling; C code (no personality) is
unaffected.

**Hand-written assembly** that may end up in a shared library or PIE calls
functions in other modules with `callrel` / `tail` (which the assembler
makes short calls, `R_KLAUSSCPU_CALL18`), not `jmprel`: a branch to another
section is `R_KLAUSSCPU_PCREL32`, which fails against a preemptible symbol,
as a 2-word `callrel` without `%plt` does.  Such code reaches another
module's data with `%gotpcrel`, and keeps no value in R14 across a call.

### PLT

No PLT header (PLT0): musl binds every `R_KLAUSSCPU_JUMP_SLOT` at load.
Each entry is 16 bytes, 16-aligned (one fetch line); `.plt` has
`sh_entsize` 16 (without it lldb assumes a PLT0 and makes no `@plt`
symbols):

```
+0   8B800E00 dddddddd   leapc   r14, slot − (entry + 0)   # this entry's .got.plt slot
+8   5B300EE0            ldidx64 r14, r14, 0               # the short LDIDX64A (address & ~7)
+12  6080000E            jmpr    r14
```

- **R14 (O) is the scratch register**: caller-saved and neither an argument
  (R0–R3) nor a return register (R12, R11).  Nothing in the calling
  convention keeps a value in R14 across a call, so a callee reached through
  a PLT entry sees the same state as with a direct call.  `CALL` has already
  pushed the return address and `JMPR` touches neither SP nor `[SP]`, so no
  link register is needed (RISC-V's PLT uses t1 and t3 the same way).
  `ldidx64` is LLVM's name for the force-aligned load (klausscc
  `LDIDX64A.S`); the slot is 8-aligned anyway.
- **Unwinding**: the entry changes neither SP nor R15, so the CIE's rule
  (CFA = SP + 8, return address at `[SP]`) is right on every instruction of
  it.  `.plt` needs no FDE.
- The cost of a call through the PLT, against a direct call, is three
  instructions, one of them a load the `jmpr` waits for.  M6 measures it on
  the board.  `-fno-plt` (a GOT load and `callr` at the call site, 4 words
  instead of 1) is not provided.

### `<link.h>` and the loader's structures

The ELF types of `<link.h>` are the ELFCLASS32 ones: `ElfW(Phdr)`,
`ElfW(Dyn)`, `ElfW(Sym)` and `ElfW(Half)` are `Elf32_*`.  Every pointer and
every address-typed (`ElfW(Addr)`) field of the structures the loader
publishes is 8 bytes, because it holds a C pointer or a load address in
memory, not an ELF field:

| Structure | Layout (offset: field) | Size |
|---|---|---|
| `struct dl_phdr_info` | 0 `dlpi_addr` (8), 8 `dlpi_name`, 16 `dlpi_phdr` (→ `Elf32_Phdr`), 24 `dlpi_phnum` (`Elf32_Half`), 32 `dlpi_adds`, 40 `dlpi_subs`, 48 `dlpi_tls_modid` (`size_t`), 56 `dlpi_tls_data` | 64 |
| `struct link_map` | 0 `l_addr` (8), 8 `l_name`, 16 `l_ld` (→ `Elf32_Dyn`), 24 `l_next`, 32 `l_prev` | 40 |
| `struct r_debug` | 0 `r_version` (`int`), 8 `r_map`, 16 `r_brk` (8), 24 `r_state` (`int`), 32 `r_ldbase` (8) | 40 |

`r_debug` matches musl's internal `struct debug { int ver; void *head; void
(*bp)(void); int state; void *base; }`, and `link_map` the head of its
`struct dso`.  `DT_DEBUG`'s `d_val` (4 bytes) points at it.  Today's musl
`<link.h>` picks `Elf64_*` from `UINTPTR_MAX`, so even the static
`dl_iterate_phdr` misreads `PT_TLS` / `PT_GNU_EH_FRAME`; M4 fixes it.  The
auxiliary vector has 8-byte entries (`a_type`, `a_val`), whose values
(`AT_PHDR`, `AT_BASE`, `AT_ENTRY`) are addresses of ELF32 structures.

### Images

- **ELF header**: ELFCLASS32, `EM_KLAUSSCPU`, `e_flags` = the ABI version,
  `ET_DYN` for shared libraries and PIE.  lld checks the version of the
  shared libraries it links against too (the rule of "Versioning"); musl's
  `map_library` checks `e_machine` and the version when it loads one.
- **Dynamic section**: `Elf32_Dyn`.  `DT_NEEDED`, `DT_SONAME`, `DT_RUNPATH`,
  `DT_SYMTAB`/`DT_STRTAB`/`DT_STRSZ`/`DT_SYMENT`, `DT_HASH` and
  `DT_GNU_HASH`, `DT_RELA`/`DT_RELASZ`/`DT_RELAENT` (12)/`DT_RELACOUNT`,
  `DT_JMPREL`/`DT_PLTRELSZ`/`DT_PLTREL` (= `DT_RELA`), `DT_PLTGOT`,
  `DT_INIT_ARRAY`/`DT_FINI_ARRAY` and their sizes (arrays of 8-byte
  pointers, each with a `RELATIVE` relocation), `DT_FLAGS` (`BIND_NOW`,
  `STATIC_TLS`), `DT_FLAGS_1` (`NOW`, `PIE`), and `DT_DEBUG` in executables.
  No `DT_TEXTREL`: a text relocation is a link error (lld's `-z text`
  default).
- **Hash tables**: `.hash` has `Elf32_Word` entries; `.gnu.hash` has 32-bit
  Bloom words (ELFCLASS32), so musl's `size_t` Bloom-word reads must become
  32-bit.
- **Interpreter**: `PT_INTERP` = `/lib/ld-musl-klausscpu.so.1`.
- **Pages: 64 KiB** (the TLB's only size).  Every `PT_LOAD` of a shared
  library or PIE has `p_align` = 0x10000, and the common page size is
  64 KiB too, so `PT_GNU_RELRO` ends on a page boundary and `mprotect`
  covers all of it.  lld's KlaussCPU defaults (`max-page-size`,
  `common-page-size`) are 64 KiB for `-shared` and `-pie` outputs, and the
  driver passes both too.  Static links keep their current defaults and
  layout: the driver already passes `max-page-size=0x10000` for static Linux
  links, and bare-metal images must not move.  Each library's writable
  segment costs at least 64 KiB per process.
- `PT_TLS`, `PT_GNU_STACK` (not executable), `PT_GNU_RELRO`,
  `PT_GNU_EH_FRAME` (`--eh-frame-hdr`, for `dl_iterate_phdr` unwinders).
- **Base address**: 0 (`ET_DYN`); the kernel puts a PIE at
  `ELF_ET_DYN_BASE` (randomised once the kernel enables it) and the
  interpreter elsewhere.

### Executables

- **PIE** (`-fPIE -pie`, `Scrt1.o`, `ET_DYN`): definitions are
  `leapc`-relative, external data and the addresses of external functions
  come through the GOT, and TLS is local exec or initial exec.  It becomes
  the default executable form once dynamic linking works (see Driver).
- **Static executables** (`-static`) as today.  A static PIE (`-static-pie`,
  musl's `rcrt1.o`, only `RELATIVE`) would work with the same relocations;
  it is not planned.
- A dynamically linked executable that is not PIE is not supported: the
  driver never makes one, and no test covers one.

### Building shared libraries

- **compiler-rt builtins for the Linux triple**, compiled by a compiler with
  these rules, `-fPIC -fvisibility=hidden`: every module links its own hidden
  copy, as with `libgcc.a`, and none of them exports or preempts another's.
  The bare-metal (`klausscpu-unknown-elf`) archive that musl's build uses
  today has absolute jump tables and default-visibility symbols, so it
  cannot go into a shared library.  The Linux archive's place is the one the
  driver already prefers for the Linux triple, `<build>/lib/clang/<version>/
  lib/klausscpu-unknown-linux-musl/libclang_rt.builtins.a` (`clang
  --target=klausscpu-unknown-linux-musl -print-libgcc-file-name`).
- **`libc.so`** is linked with musl's own flags: `--dynamic-list`,
  `--exclude-libs=ALL`, `--no-undefined`, and symbolic binding of libc's
  calls to itself.  Then libc's internal calls, including the `memcpy` /
  `memset` the compiler makes in `_dlstart_c`, are bound at link time and
  never go through a PLT, which is not relocated yet at that point.
- **C++**: each module has its own hidden `__dso_handle` (for
  `__cxa_atexit`), from the crtbegin of the C++ runtime; C needs none.

### Driver (`klausscpu-*-linux-*`)

A Linux link is `-static` (`crt1.o`) unless it asks otherwise; `-static`
wins over `-pie`.  `-shared` links a shared library (no start file), and
`-pie` a PIE (`Scrt1.o`) with `-dynamic-linker /lib/ld-musl-klausscpu.so.1`
(a shared library has no `PT_INTERP`).  Both get `-z now`,
`--hash-style=both`, `--eh-frame-hdr` and both 64 KiB page sizes, and
`-rdynamic` becomes `--export-dynamic`; both take the builtins built for
the Linux triple (the bare-metal archive, a static link's fallback, cannot
go into either).  `-pie` is a link option: the compiler still needs
`-fPIE`.  Static stays the default until the dynamic linker works on the
emulator (M5); then the default becomes dynamic PIE, with `-static` as on
other Linux targets (M7).

### Not in the first version

- lld relaxations: a GOT access to a symbol that cannot be preempted made
  into a plain `leapc` (the load is a separate instruction, so this needs a
  marker relocation in the style of x86's `GOTPCRELX`), and GD/IE turned into
  LE in executables.  Worth it only if measurements show the loads matter.
- `-fno-plt`, TLSDESC, RELR, IFUNC, lazy binding, `dlopen` of an
  initial-exec library, dynamically linked executables that are not PIE.
- A PC-relative 64-bit load in the ISA would make a GOT access one
  instruction.  It is a candidate for `HARDWARE_PROPOSALS.md` once the
  loads are measured.

### Consumers

| Consumer | What it does |
|---|---|
| clang | Linux `dso_local` rule; the driver (above) |
| Backend | GOT, PLT and TLS sequences under the Linux triple; external symbols through the GOT; PC-relative jump tables; exception-table encodings |
| MC | `%gotpcrel`, `%plt`, `%gottprel`, `%tlsgd`; fixups; relocations 8–11; `KlaussCPU.def` 8–18 |
| lld | `-shared`, `-pie`, `.dynamic`, GOT (8-byte entries) and `.got.plt` (no header, zero-filled), PLT (`sh_entsize` 16), COPY, canonical PLT, the dynamic relocations, TLS IE/GD, CALL18 through the PLT with thunks to the PLT entry, `-z now` and 64 KiB pages for dynamic outputs, `e_flags` of shared libraries |
| compiler-rt | builtins for `klausscpu-unknown-linux-musl`, `-fPIC -fvisibility=hidden` |
| musl (runtime) | `reloc.h` (`REL_SYMBOLIC` 2, `REL_GOT` 13, `REL_PLT` 14, `REL_RELATIVE` 15, `REL_COPY` 12, `REL_TPOFF` 16, `REL_DTPMOD` 17, `REL_DTPOFF` 18, addends sign-extended); `dynlink.c` reading ELF32 structures (`Elf32_Dyn`, 12-byte `Elf32_Rela` with `r_info >> 8` / `& 0xff`, `Elf32_Sym`, 32-bit `.gnu.hash` Bloom words) while writing 64-bit fields; `<link.h>` as above; `_dlstart`, `__tls_get_addr`, `Scrt1.o`, `libc.so`'s link flags |
| Kernel | `PT_INTERP`, `ET_DYN`, 64 KiB `p_align` |
| lldb | the POSIX-DYLD plugin reads `r_debug`/`link_map` with 8-byte pointers and addresses (the layout above), 8-byte auxv entries, and 4-byte ELF addresses (later); `.plt` entries from `sh_entsize` |
| `check-toolchain-constants.py` | relocation numbers in musl's `reloc.h` against `KlaussCPU.def` |
| Runtime ELF loaders, Zephyr LLEXT | unaffected: no object built for them carries the new types (Zephyr's relocator rejects unknown types; the bare-metal and FreeRTOS loaders apply `ABS32`/`ABS64` and skip the rest) |

## Versioning

- `KlaussCPUABI::ABIVersion` is 3.  The assembler writes it into the low byte
  of every object's `e_flags` (`EFlagsABIMask`; a target streamer in
  `KlaussCPUMCTargetDesc.cpp`), for compiled C and hand-written assembly
  alike; objects from older toolchains have 0 or 2.
- lld refuses to link objects with code (an executable section) of different
  versions: `KlaussCPU ABI version 2 cannot be linked with ABI version 3 …;
  rebuild it with the current toolchain`.  This catches a stale object a build
  system did not rebuild after a toolchain update (make does not track the
  compiler) and a C library built for the old ABI.  Objects without code
  (`-b binary`, data) are not checked.  The output's `e_flags` is the version.
- clang predefines `__klausscpu_abi__` to the version, for code that depends
  on the convention (assembly can `#if __klausscpu_abi__ != 3` / `#error`).
- The runtime's PIC program loaders (bare-metal `loader.c`, FreeRTOS
  `pic_loader.c`) refuse a program of another version (an ABI 2 program too) (`klausscpu_elf.h`
  `KLAUSSCPU_ABI_VERSION`, compared with `ABIVersion` by
  `check-toolchain-constants.py`).  The Zephyr LLEXT loader checks it as well
  (`arch/klausscpu/core/elf.c`, the SD test `abi_mismatch`); klausscc does not.
  The version is also the guard against a stale library: picolibc, the
  builtins, wolfSSL and the rest must be rebuilt, or lld refuses the link.

## Consumers

| Consumer | What depends on the ABI | Status for version 3 |
|---|---|---|
| Backend | `KlaussCPUCallingConv.td`, `LowerCall`/`LowerFormalArguments`/`LowerReturn`, `shouldSignExtendTypeInLibCall`, the call register mask | updated |
| clang | `KlaussCPUABIInfo` (classification, `va_arg`), `__klausscpu_abi__` | updated; tests `klausscpu-abi.c`, `klausscpu-varargs.c`, `Preprocessor/klausscpu-target-defines.c` |
| `TargetLibraryInfo` | `signext` on i32 parameters/results of library functions it creates | klausscpu added to the riscv64 rule |
| MC / lld | `e_flags` version, link check | `MC/KlaussCPU/elf-abi-version.s`, `lld/test/ELF/klausscpu-abi-version.s` |
| lldb ABI plugin | argument/return/callee-saved registers (from `KlaussCPUABI.h`), return values; fallback unwind plans (CIE state at entry, R15 chain) | aggregates of up to 16 bytes read from R12/R11; a written return value is extended like the ABI (`unsigned int` sign-extended); frameless functions: see "Frames" |
| klausscpu-runtime assembly | `setjmp.S` (R4–R7, R15, SP, PC), `context_switch.S`, FreeRTOS `port.S`, Zephyr `swap.S` (save all 16 GPRs; R4 holds the saved SP across C calls), ISR wrappers that save the caller-saved set, `crt0.c` (`main`'s result in R12) | unchanged: the register roles did not change, and none of them passes stack or variadic arguments or relies on the old home area |
| klausscpu-runtime libraries | picolibc, compiler-rt builtins, wolfSSL/wolfSSH archives | rebuilt (`build-picolibc.sh`, `build-builtins.sh`, `freertos/wolfssl/build-*.sh`) |
| klausscc | gdbserver target XML (r0–r3 = arguments, r15 = fp) | unchanged; it has no other ABI knowledge (no unwinder: crash dumps are printed raw) |
| Rust | rustc does its own argument classification (`callconv/klausscpu.rs`, rustc patch) | updated (`rust/rustc-patches/0003-*`); test `rust/abi-test/run_rust_abi_test.sh` — see below |
| CPU_ARCHITECTURE.md §2 (hardware repo) | "R4–R7 and R15 are callee-saved" | still true |

### Rust

The target spec (`rust/klausscpu-unknown-none-elf.json`) has nothing
ABI-specific; the classification is rustc's
`compiler/rustc_target/src/callconv/klausscpu.rs`, added by
`rust/rustc-patches/0001-*.patch` and brought to version 2 by `0003-*.patch`
(before that it implemented version 0, and Rust and C disagreed on `u32` and
on aggregates by value; lld cannot tell, since every object rustc emits
carries the version of LLVM's MC layer in `e_flags`).  A rustc without
the patch cannot build for this target at all: its LLVM has no KlaussCPU
backend, and with a JSON spec of an architecture it does not know
(`Arch::Other`) `adjust_for_foreign_abi` panics ("no lowering implemented")
on the first `extern "C"` function and `va_arg` stops with `bug!`.  The module mirrors
`KlaussCPUABIInfo` (clang) rule for rule, following rustc's RISC-V module
(`callconv/riscv.rs`, integer-only, XLEN = 64):

- an aggregate (`ScalarPair` or `Memory` layout, a SIMD vector) of up to 16
  bytes is cast to an integer of its size (`Reg { Integer, size }`, e.g.
  `i24`), to `i128` if it is 16-byte aligned, else to `[2 x i64]`
  (`Uniform`); a larger one is indirect (a pointer to a copy the caller makes,
  not `byval`; returned through `sret`).  Arguments and the return value use
  the same rules;
- an integer scalar narrower than 64 bits is extended: 32 bits sign-extended
  (also `u32`, `char`), narrower ones by their signedness (`bool`
  zero-extended); floats and pointers are passed as they are;
- `#[rustc_pass_indirectly_in_non_rustic_abis]` types are indirect, a
  zero-sized argument is ignored (as GNU C `struct {}`), and an unsized one is
  left alone;
- the Rust ABI (`extern "Rust"`) keeps rustc's generic aggregate handling but
  extends integers the same way (`compute_rust_abi_info`, as RISC-V does),
  which also makes `compiler_builtins` (its intrinsics are `extern "C"`) agree
  with the backend's libcall convention (`shouldSignExtendTypeInLibCall`);
- c-variadic definitions (`#![feature(c_variadic,
  c_variadic_experimental_arch)]`): rustc's `va_arg` for this target is the
  void-pointer walk clang emits (`emit_ptr_va_arg`: 8-byte slots, never
  realigned, over 16 bytes by reference; a patch in
  `rustc_codegen_llvm/src/va_arg.rs`, which otherwise stops with `bug!` for a
  JSON-spec architecture).  Calls to variadic C functions need nothing
  special: LLVM reserves the home area.

`rust/abi-test/run_rust_abi_test.sh` checks it like the C conformance test:
generated callers and callees in C and in Rust, every Rust/C pairing at
every optimisation level, against host gcc.

## Design and measurements

The changes were chosen on the bare-metal corpus (klausscpu-runtime
`baremetal`, 38 programs including picolibc and compiler-rt, all rebuilt per
variant), emulator instruction counts of the programs that halt, `.text` of
all ELFs, and FPGA board cycles of `dhrystone` and `perf_baseline` at -O1
(deterministic; run twice).  Variants were selected with temporary switches
that were removed afterwards.

**Varargs.**  Two schemes without the per-call 24-byte home area:

- *all unnamed arguments on the stack* (Apple arm64 style; `va_list` a plain
  pointer to the stack slots): the caller stores every unnamed argument.  It
  grew `.text` by 1.8% (+20 KB at -O1): a `printf` call needs a GETSP and one
  store per argument instead of a register move, and lwIP's assertion
  `printf`s are everywhere;
- *home area only in variadic calls* (chosen): no instruction or size cost
  over version 0, frees 24 bytes of stack in every non-variadic call frame
  (`fib` frames: `enter 5` → `enter 2`), and supports `f(...)`.

| -O1 | instructions | .text |
|---|---|---|
| version 0 | 269,066,829 | 1,113,696 |
| v2, unnamed args on the stack | −0.10% | +1.82% |
| v2, home area in variadic calls (chosen) | −0.10% | −0.76% |

**Callee-saved registers** (with the home-area scheme; 6 = R4–R9, 8 =
R4–R10 + R13; the second return register stays R11):

| | -O1 instr | -O2 instr | -O1 .text | -O2 .text | board dhrystone | board perf_baseline |
|---|---|---|---|---|---|---|
| version 0 | 269,066,829 | 268,186,783 | 1,113,696 | 1,153,064 | 299,983,358 | 74,494,477 |
| 4 (R4–R7, chosen) | −0.10% | −0.51% | −0.76% | −0.76% | +0.00% | +0.18% |
| 6 | −0.02% | +0.99% | −1.95% | −1.79% | −0.00% | +0.16% |
| 8 | −0.08% | +1.02% | −2.09% | −2.05% | −0.00% | +0.16% |
| 6, RA CSR first-use cost 5 | −0.02% | +1.04% | −0.27% | −0.27% | | |

(`test_flags` excluded: its generated assembly uses R8–R10 as scratch.)
More callee-saved registers make the code smaller but slower: without
shrink-wrapping every entry saves them, also on early-exit paths —
`perf_baseline`'s recursive `fib` at -O2 saves two more registers per call
and runs 6.8% more instructions — and at -O1 the dynamic gain elsewhere is
nil.  The board showed no difference between 4, 6 and 8.  Four stays, which
also leaves every hand-written register-save sequence valid.  (Shrink-
wrapping, added in the 'frame' step, moves few frames: `fib`'s accumulator
lives in a callee-saved register from the entry block on, so its base case
still saves them.)

**Argument registers.**  Of the 1,701 call sites in the corpus' 1,337
distinct functions, 93 (5.5%) pass a stack argument (a GETSP in front of the
call), so more argument registers would buy little and cost temporaries.
Four stay.

**Where the gain comes from** (ablations of the final ABI):

| -O1 | instructions | .text |
|---|---|---|
| version 2 | −0.10% | −0.76% |
| version 2 without signext on i32 | +0.00% | +0.01% |
| version 2 without aggregates in registers | −0.10% | −0.76% |

The sign extension is all of the measurable gain (queens −3.9%,
test_divshadow −1.4%, perf_baseline −1.9% at -O2): a callee no longer
re-extends an `int` parameter, a caller no longer re-extends an `int`
result, and an `int` index or compare uses the 64-bit form.  The corpus
passes almost no small aggregates by value; that change is for the code that
does (Zephyr's `k_timeout_t`, Csmith).  It costs where a producer must now
extend: `fib(n - 1)` with `unsigned n` is `ADDIW` (2 words) instead of
`DECR` (1 word), and on the board `perf_baseline`'s `calls_fib` kernel takes
0.40% more cycles (more I-fetch misses from the longer loop, fewer memory
waits from the smaller frames).

**Totals** (the final build against version 0 at the same LLVM revision):
emulator instructions −0.10% at -O1, −0.51% at -O2; `.text` −0.78% at both;
board: dhrystone 299,983,362 cycles (±0), perf_baseline +0.18% (calls_fib
+0.40%, ptr_chase +0.16%, the rest within ±0.02%).

**Frames** (the 'frame' step, after version 2; same corpus, picolibc and
the builtins rebuilt; against version 2 as above):

| -O1 | instructions | .text |
|---|---|---|
| frameless leaves | −0.002% | −0.29% |
| + shrink-wrapping | −0.002% | −0.18% |
| + tail calls (final) | −0.002% | −0.21% (-O2: −0.16%) |

Every program runs the same or fewer instructions (the dynamic gain is
small: -O1 inlines most small leaves, and a tail call executes as many
instructions as the call and LEAVERET it replaces).  Shrink-wrapping costs
0.11% of `.text` (a second return per function; up to two return-block
instructions copied in front of a LEAVERET).  Letting CodeGenPrepare
duplicate returns for more tail calls cost another 0.15% for no measurable
cycles, so it is off.  Board: dhrystone and `calls_fib` unchanged (`fib`
keeps its accumulator in callee-saved registers from the entry block, so
its base case still has a frame); `perf_baseline` +0.03%, all of it
`ptr_chase` reading a malloc'd buffer that starts 56 bytes lower (with
`_end` padded back, +0.001%).  A board microbenchmark of the patterns
(20,000 calls each): a frameless leaf reached by a tail call −37% cycles, an
early exit −6%, soft-float `float` −11%, `double` −3%.  At -Os/-Oz
shrink-wrapping is off: there it only grew `.text` (the runtime's 44
objects, +44 bytes at -Os, +40 at -Oz).

## Validation

- `klausscpu-runtime/abi-test/run_abi_test.sh`: generated caller/callee
  pairs (random mixes of signed/unsigned 8–64-bit integers, pointers,
  float/double, `__int128`, structs and unions of 1–24 bytes, variadic
  functions with and without named parameters) in separate translation
  units, every combination of -O0/-O1/-O2/-Os for the two, run on the
  emulator against host gcc: seeds 1–20 with 80 functions each, 320 runs,
  all identical.  A version-0 caller against a version-2 callee differs in
  52 of 61 tests, so the test does see a mismatch.
- Csmith (it passes structs by value) at -O0/-O1/-O2/-Os: 600 seeds, 531
  match at every level, 69 time out on the host; with `--float` 300 seeds,
  246 match, 49 host time-outs, 5 host-detected UB; no mismatch.
- The emulator corpus (UART output unchanged at -O1 and -O2), FreeRTOS
  (`test_rtos`, `test_sync` on the emulator and the board, the demo), Zephyr
  `hello`, `irq_soak`, `llext_demo` on the emulator and the board, the lldb
  smoke tests (`debug/run_debug_smoke.sh` now checks a returned struct and an
  `unsigned int`; `debug/run_rtos_smoke.sh`).
- Rust (phase 5, rustc 1.98-dev with `rust/rustc-patches/0001`–`0003`):
  `rust/abi-test/run_rust_abi_test.sh` seeds 1–60 with 80 functions each —
  C caller/Rust callee, Rust caller/C callee and Rust/Rust, C at
  -O0/-O1/-O2/-Os and Rust at opt-level 0/1/2/s, 2,880 runs on the emulator,
  all identical to host gcc; three of the ELFs also on the board, identical.  The rustc with the
  version-0 module differs in 53 of 80 tests of the same seeds.  With
  `i128`/`u128` and `bool` record fields and records of up to 32 bytes
  added to the generator: seeds 1–30, 1,440 runs, all identical, and three
  ELFs of seed 3 on the board, identical.  The
  `rust/crates/examples/ffi-check` self-check (structs of 3–24 bytes, a
  16-byte-aligned one and a union by value both ways, `u32` arguments and
  results, atomics of every width shared with C, `INT_MASK` and the interrupt-enable bit restored, a
  timer-interrupt race) passes on the emulator at opt-level 0/1/2/3/s/z with
  C at -O0..-Os, and on the board.

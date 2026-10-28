# KlaussCPU C ABI (version 2)

This is the calling convention of KlaussCPU code built with this toolchain:
what the backend (`KlaussCPUCallingConv.td`, `KlaussCPUISelLowering.cpp`),
clang (`clang/lib/CodeGen/Targets/KlaussCPU.cpp`) and the hand-written
assembly in klausscpu-runtime agree on.  The numbers shared by the tools
(registers, offsets, the version) live in
`llvm/include/llvm/TargetParser/KlaussCPUABI.h`.  The model is RISC-V LP64
(integer only) with KlaussCPU's register file and frame instructions.

Version 2 replaced the original convention (version 0) in phase 4 of the
backend work.  The two do not mix: every object records its version in
`e_flags`, and lld refuses to link code of different versions (see
"Versioning").  Rebuild everything (picolibc, the compiler-rt builtins,
wolfSSL/wolfSSH, Zephyr, the programs) with the new toolchain.

## Summary

| | Version 2 | Version 0 (before) |
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
| R15 (P) | frame pointer, saved by every prologue (`ENTER`) |
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
  it is 16-byte aligned, or else to `[2 x i64]`.  That is one or two GPRs; an
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

## Versioning

- `KlaussCPUABI::ABIVersion` is 2.  The assembler writes it into the low byte
  of every object's `e_flags` (`EFlagsABIMask`; a target streamer in
  `KlaussCPUMCTargetDesc.cpp`), for compiled C and hand-written assembly
  alike; objects from older toolchains have 0.
- lld refuses to link objects with code (an executable section) of different
  versions: `KlaussCPU ABI version 0 cannot be linked with ABI version 2 …;
  rebuild it with the current toolchain`.  This catches a stale object a build
  system did not rebuild after a toolchain update (make does not track the
  compiler) and a C library built for the old ABI.  Objects without code
  (`-b binary`, data) are not checked.  The output's `e_flags` is the version.
- clang predefines `__klausscpu_abi__` to the version, for code that depends
  on the convention (assembly can `#if __klausscpu_abi__ != 2` / `#error`).
- The ELF loaders (klausscc, Zephyr LLEXT, the runtime's `loader.c`) do not
  check it.

## Consumers

| Consumer | What depends on the ABI | Status for version 2 |
|---|---|---|
| Backend | `KlaussCPUCallingConv.td`, `LowerCall`/`LowerFormalArguments`/`LowerReturn`, `shouldSignExtendTypeInLibCall`, the call register mask | updated |
| clang | `KlaussCPUABIInfo` (classification, `va_arg`), `__klausscpu_abi__` | updated; tests `klausscpu-abi.c`, `klausscpu-varargs.c`, `Preprocessor/klausscpu-target-defines.c` |
| `TargetLibraryInfo` | `signext` on i32 parameters/results of library functions it creates | klausscpu added to the riscv64 rule |
| MC / lld | `e_flags` version, link check | `MC/KlaussCPU/elf-abi-version.s`, `lld/test/ELF/klausscpu-abi-version.s` |
| lldb ABI plugin | argument/return/callee-saved registers (from `KlaussCPUABI.h`), return values | aggregates of up to 16 bytes read from R12/R11; a written return value is extended like the ABI (`unsigned int` sign-extended) |
| klausscpu-runtime assembly | `setjmp.S` (R4–R7, R15, SP, PC), `context_switch.S`, FreeRTOS `port.S`, Zephyr `swap.S` (save all 16 GPRs; R4 holds the saved SP across C calls), ISR wrappers that save the caller-saved set, `crt0.c` (`main`'s result in R12) | unchanged: the register roles did not change, and none of them passes stack or variadic arguments or relies on the old home area |
| klausscpu-runtime libraries | picolibc, compiler-rt builtins, wolfSSL/wolfSSH archives | rebuilt (`build-picolibc.sh`, `build-builtins.sh`, `freertos/wolfssl/build-*.sh`) |
| klausscc | gdbserver target XML (r0–r3 = arguments, r15 = fp) | unchanged; it has no other ABI knowledge |
| Rust | rustc does its own argument classification | **not updated** — see below |
| CPU_ARCHITECTURE.md §2 (hardware repo) | "R4–R7 and R15 are callee-saved" | still true |

### Rust

The target spec (`rust/klausscpu-unknown-none-elf.json`) has nothing
ABI-specific; the classification is rustc's
`compiler/rustc_target/src/callconv/klausscpu.rs`, added by
`rust/rustc-patches/0001-*.patch`.  It still implements version 0
(DefaultABIInfo: aggregates indirect, integers extended by their own
signedness), and objects rustc emits carry version 2 in `e_flags` anyway
(the flag comes from LLVM's MC layer), so lld does not catch the mismatch.
**Calls between Rust and C disagree for `u32` arguments/results (zero-
vs sign-extended) and for aggregates passed or returned by value** until
the patch is updated.  Rust code is not quite consistent with itself
either: once rustc is built on this LLVM, the backend's library calls
assume an i32 result is sign-extended (`shouldSignExtendTypeInLibCall`),
while Rust's `compiler_builtins`, classified by the version-0 module,
zero-extends a `u32` result (today no libcall the backend emits returns an
unsigned 32-bit value that needs it: 32-bit division is in hardware, and
`fptoui` to i32 goes through the 64-bit `__fixuns*di`).  What
the callconv module needs, following rustc's RISC-V implementation
(`callconv/riscv.rs`, integer-only, XLEN = 64, without the FP and vector
cases); a sketch, not built (there is no rustc checkout here):

```rust
fn classify<'a, Ty, C>(cx: &C, arg: &mut ArgAbi<'a, Ty>, is_ret: bool)
where Ty: TyAbiInterface<'a, C> + Copy, C: HasDataLayout {
    if !is_ret && arg.layout.pass_indirectly_in_non_rustic_abis(cx) {
        arg.make_indirect();
        return;
    }
    let size = arg.layout.size;
    if arg.layout.is_aggregate() {
        if size.bits() > 128 {
            arg.make_indirect();           // by reference / sret
        } else if size.bits() <= 64 {
            arg.cast_to(Reg { kind: RegKind::Integer, size });
        } else {
            let align = arg.layout.align.abi.bytes();
            arg.cast_to(if align == 16 { Reg::i128() } else {
                Uniform::new(Reg::i64(), size).into()  // [2 x i64]
            });
        }
        return;
    }
    // Scalars: every 32-bit integer sign-extended (also u32), narrower ones
    // by signedness (riscv.rs extend_integer_width).
    if let BackendRepr::Scalar(s) = arg.layout.backend_repr {
        if let Primitive::Int(i, _) = s.primitive() {
            if i.size().bits() == 32 {
                if let PassMode::Direct(ref mut attrs) = arg.mode {
                    attrs.ext(ArgExtension::Sext);
                    return;
                }
            }
        }
    }
    arg.extend_integer_width_to(64);
}
```

(called for the return value and each argument; a zero-sized argument is
ignored as now.)  rustc's `extern "C" fn(...)` variadic calls need nothing
special: LLVM reserves the home area.

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
also leaves every hand-written register-save sequence valid.  Worth
re-measuring once shrink-wrapping exists.

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

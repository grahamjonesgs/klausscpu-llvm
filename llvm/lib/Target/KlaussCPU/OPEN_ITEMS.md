# KlaussCPU toolchain: open items

Status as of 2026-10-29: klausscpu-llvm `main` 1691420aeb82, klausscpu-runtime
`main` 7873d57, klausscc `master` 4c9c464, and the board on klaussCPU
`b383104` (the 9ad6614 squash fixes and the rebuilt netboot ROM).

Nothing here is a known miscompile.  The list comes from two sources:

- a re-audit of the original 41-item backend review, made after phases 1-7;
- the small leftovers those phases reported.

**Priority** is how much an item matters today: medium, or low.

**Complexity** is the size of the work:

- trivial: an hour or less;
- small: up to half a day;
- medium: about a day or more, or it needs a decision first.

When an item is done, delete it here and say so in the commit message.

## Re-audit of the original review

All 55 entries were re-checked against the code above: the 41 numbered items
plus the verifiers' section D, whose bullets partly repeat numbered items.
Most were checked with a reproducer run on the emulator or through llvm-mc,
not only by reading the fixing commit.

| Status | Count |
|---|---|
| Fixed | 42 |
| Partly fixed, low-priority remainder | 12 (items 13, 17, 20, 22, 25, 26, 31, 33, 34, 37, 40, 41) |
| Open, low priority | 1 (D: VLA size rounding mask) |
| Regressed | 0 |

Every correctness item (1-17) is fixed:

- byval copies;
- FlagReuse with live flags;
- stack realignment;
- PC-relative fixups and data fixups;
- DWARF frame offsets;
- C23 `f(...)`;
- BDEP encoding;
- immediate range checks.

The remainders are listed below with their item numbers.

## Worth doing (medium priority)

| # | Item | Complexity | Notes |
|---|---|---|---|
| 1 | `test_big`'s board baseline contains the SD card's root listing | small | Every change to the card shows up as a false difference in `validate.sh board`. Drop the listing lines in `tools/validate` (`uart_filter`, or a test_big-specific filter), keeping its PASS/FAIL verdicts. |
| 2 | `__int128` alignment: DataLayout `i128:128` (16 bytes) against malloc's 8-byte guarantee and `__BIGGEST_ALIGNMENT__` = 8 | medium (an ABI decision) | No miscompile was found, but the compiler may assume a heap `__int128 *` is 16-aligned. Every function holding an `__int128` in memory, so every one at -O0, reserves R7 and pays the realignment prologue. Choose: `__int128` 8-aligned (DataLayout change, ABI.md), or malloc and `__BIGGEST_ALIGNMENT__` 16 (picolibc, runtime allocators). Item 3 of the review. |
| 3 | Zephyr connects only the timer interrupt | medium | Any device interrupt needs it. It becomes necessary if the VGA RTL is merged, because VGA vsync is interrupt source 3. Dynamic IRQ connect is refused cleanly today (runtime 6c606be). |
| 4 | Documentation and comment drift in the backend | trivial (one batch) | (a) CLAUDE.md gives `-mcpu=no-sched` as a clang A/B switch, but clang rejects `-mcpu` for this target; only `llc -mcpu` and `-Xclang -target-cpu -Xclang no-sched` work. Either document that or let clang accept `-mcpu` (`isValidCPUName`/`setCPU`). (b) Three comments about when `eliminateFrameIndex` uses a scratch register contradict each other: `KlaussCPURegisterInfo.h:37-41` and `KlaussCPURegisterInfo.cpp:114-119` against `KlaussCPURegisterInfo.cpp:184-190`. (c) `test/CodeGen/KlaussCPU/varargs.ll:78-82` still calls MEMGET32 wrong on hardware; that was the pre-pipeline core. (d) The runtime's `src/fp_mode_stub.c` is still linked, but `fp_mode.h` inlines those functions now. (e) Mark the finished handoff and plan documents in this directory as historical (RTOS_NOTES.md, M8_SCHED_MODEL_SPEC.md, PIPELINE_*_HANDOFF.md, PERF_LLVM_HANDOFF.md), or move them out with rust/ (item 41). |

## Optional code-quality and size work (low priority)

| # | Item | Complexity | Gain |
|---|---|---|---|
| 5 | ADC/SBC carry chains: ADDC/ADDE/SUBC/SUBE legal, with patterns, for `__int128` and multiword add/sub (item 31) | medium | crypto bignum code (wolfSSL), compiler-rt i128 |
| 6 | Outgoing stack arguments stored R15-relative under `hasReservedCallFrame`, instead of after a GETSP (item 22, CALL-1) | small | one instruction at about 5.5% of call sites |
| 7 | Constant shrinking: override `targetShrinkDemandedConstant` so masks become 1-2 word ANDI/ANDV (item 20); VLA rounding uses SETR64 `0x1_FFFF_FFF8` where -8 would do (section D); 64-bit powers of two and runs of ones as SETR + SHLV/RORV under optsize (item 26) | small | 1-2 words per occurrence |
| 8 | Tail call through a no-op sign extension: `long f(void) { return int_fn(); }` keeps `callrel; leaveret`, though under ABI v2 the signext i32 result is already the i64 | small | small |
| 9 | Selects: same-condition selects separated by a memory op still get separate diamonds; no zero-extended boolean compare-immediate for uint32 constants above INT32_MAX; no BTST + JMPZ/JMPNZ for single-bit tests (bits 32-63 cost SETR64 + ANDR) (item 25) | small | small |
| 10 | GlobalMerge base for non-PIC code: each merged field gets its own `setru` (item 33) | small | none today, since every runtime build uses `-fPIC` |
| 11 | MIR hygiene: `isLoadFromStackSlot`/`isStoreToStackSlot` are missing; IRET_I lacks isReturn/isTerminator/isBarrier and SP uses/defs; RET_I/LEAVERET_I lack SP uses/defs (the outliner works around it with `usesSP`); CALLcc lack clobber defs (item 13) | small | enables the generic spill and remat hooks; removes workarounds |
| 12 | Redundant `sextw` after `divr` for int division (signext inputs), and an in-loop `copy` in strlen-like loops | small | very small |

## Small correctness and robustness gaps (low priority)

| # | Item | Complexity | Notes |
|---|---|---|---|
| 13 | `[[clang::musttail]]` passing a struct over 16 bytes is a compile error | medium | Clang copies the aggregate into a caller byval-temp, which a tail call cannot keep (klausscpu-llvm d716d7f646a6). Forwarding the incoming pointer needs a generic clang CGCall change. |
| 14 | `jmpo`/`jmpno` have no PC-relative form in the backend (item 17) | small | The RTL accepts JMPOREL/JMPNOREL (0xA1180000/0xA11C0000; pipeline_core.sv:576-579 at b383104). Add them as the long relaxation of JMPO_SH/JMPNO_SH and drop the InstPrinter special case. Only hand-written PIC asm beyond ±512 KiB is affected. |
| 15 | `setr rX, 0x80000000..0xFFFFFFFF` silently loads the sign-extended value | small | Deliberate and documented (CLAUDE.md, the runtime's `setr r0, 0xF00F0000` idiom). An optional warning suggesting `setru` would need the MMIO idiom exempted or moved to `setru`. |
| 16 | No harness mode runs the runtime's RELA PIC loaders (baremetal loader.c, FreeRTOS pic_loader.c) at a non-link address | medium | Needs a PIC-ELF build rule and an SD-card board mode. Their headers name a linker script, `klausscpu_pic.ld`, that is not in the repo. |
| 17 | picolibc has no `machine/setjmp.h` for KlaussCPU; the packed-struct Csmith run (with the target alignment screen) is not automated in `tools/validate` | small | The cloud campaign ran it by hand with a wrapper clang; no failures. |
| 18 | Zephyr's `z_interrupt_stacks` (CONFIG_ISR_STACK_SIZE=2048) is allocated but unused | small | The timer ISR runs on its own `_kernel_stack`, so the thread analyzer reports a false "ISR0 100%" and 2 KiB is wasted. Switch the ISR to `z_interrupt_stacks`, or shrink the option. |
| 19 | The assembler has no RESET or LCDCMDR; SETR64's high half prints signed in llvm-objdump and unsigned in `llvm-mc -show-encoding`; the optional `shouldReduceLoadWidth` re-measure (items 17, 40) | trivial | Cosmetic, plus full coverage of the ISA map. |

## For the hardware (klaussCPU) session

| # | Item | Complexity | Notes |
|---|---|---|---|
| 20 | CPU_ARCHITECTURE.md drift (items 34, 37) | trivial | §5 gives the class-A SGN as bit [20], but the RTL uses op[23]. The §11 pointer row says 4 bytes / MEMGET32, but pointers are 8 bytes / LDIDX64A. Add a §14 row for the ~4-cycle blocking D-cache hit. The intro promises a calling convention: point to klausscpu-llvm ABI.md. ISA_ENCODING_V2_MAP.md's branch table leaves a gap where JMPOREL/JMPNOREL are. |
| 21 | VGA RTL merge | the RTL session's call | The runtime merged the VGA software (runtime 7873d57). It detects the controller and switches itself off on a bitstream without VGA. The controller exists only on klaussCPU's `vga-output` branch. Once it is merged and loaded, test_vga, test_vga_scan, VGA doom and the zero-copy path need a board run with the current toolchain. |
| 22 | Performance proposals | the user's call | The ranked list is in HARDWARE_PROPOSALS.md §1: pipelined D-cache hit about −25% CoreMark, fetch-stage branch prediction about −16%, load hit 4→3 cycles, store buffer, and more. |

## Housekeeping

- `/home/graham/Documents/src/klausscpu-llvm-p5-rust` is 1.5 GB. It holds the
  LLVM sources the ABI-v2 rustc build expects at that path.  Keep it while
  that rustc may need relinking (rust/README.md); otherwise delete it.
- The SD card's macOS `.Trashes/` still holds old ELF and PIC files.  The
  board shell's `rm` cannot remove a non-empty directory.  Delete it from a
  Mac or PC.
- The new boot ROM (klaussCPU b383104) was checked after a JTAG reset, which
  reconfigures from QSPI, but not after a real power cycle.
- The two optional notes from the item 1-9 re-audit are items 2 and 15 above.

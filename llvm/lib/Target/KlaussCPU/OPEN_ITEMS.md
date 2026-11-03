# KlaussCPU toolchain: open items

Status as of 2026-11-03: the board's QSPI holds klaussCPU stq `27439c7`
(master `c57f5f7`), the last of a chain of core changes since `3508f87`:
ID- and fetch-stage branch prediction with a 64-entry BTIC and a return
stack, a pipelined D-cache hit path, one-slot and queued stores,
store-to-load and SP forwarding, a 2-cycle multiplier and a load bypass to
ALU/compare operands; the AMP core 2 is gone.  Board, -O2: CoreMark 205.8
(106.4 on 3508f87), Embench speed 1.067 (0.588).  klausscpu-llvm `main` is
tuned for it (KlaussCPUSchedule.td, CLAUDE.md), and the runtime builds at
-O2 by default.  Every bitstream in the chain passed the board suite,
csmith-board, Zephyr and klausscc's hw_* fixtures before QSPI, and lldb on
the board passed 47/47 with a DEBUG_EN=1 build of master c57f5f7 over JTAG.

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
| 24 (residual) | Alignment NOPs for short fall-through loops | small | Only run-once loops lose their NOPs.  A 2-trip fall-through loop (Dhrystone's copy loop: 7.4M of 156.6M instructions at -O1) still pays them.  A threshold of 2 has not been run on the board (4 measured break-even), and padding that the fall-through path does not execute is not built. |

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
| 25 | The loop-head alignment of 16 bytes is not always the best fetch phase | small | Measured on 8e6b8e5 and earlier: phases 4 and 8 could pay for loops with two or more 2-word instructions, about one cycle per iteration in ~90 branch-entered loops (posted-store-measure REPORT.md section 5).  Since fetch-bypass a 2-word op costs ~1.25 cycles in straight code, not 2, and the part of KlaussCPUFetchLayout that kept wide instructions off line crossings measured neutral and was removed (KlaussCPULoopAlign remains).  Re-measure the head-phase effect on the current core (perf_kern's LOOP kernels) before building anything; CPU_ARCHITECTURE §14 says only to keep loop tops and branch targets off a line's last word. |

## Small correctness and robustness gaps (low priority)

| # | Item | Complexity | Notes |
|---|---|---|---|
| 13 | `[[clang::musttail]]` passing a struct over 16 bytes is a compile error | medium | Clang copies the aggregate into a caller byval-temp, which a tail call cannot keep (klausscpu-llvm d716d7f646a6). Forwarding the incoming pointer needs a generic clang CGCall change. |
| 14 | `jmpo`/`jmpno` have no PC-relative form in the backend (item 17) | small | The RTL accepts JMPOREL/JMPNOREL (0xA1180000/0xA11C0000; pipeline_core.sv:576-579 at b383104). Add them as the long relaxation of JMPO_SH/JMPNO_SH and drop the InstPrinter special case. Only hand-written PIC asm beyond ±512 KiB is affected. |
| 15 | `setr rX, 0x80000000..0xFFFFFFFF` silently loads the sign-extended value | small | Deliberate and documented (CLAUDE.md, the runtime's `setr r0, 0xF00F0000` idiom). An optional warning suggesting `setru` would need the MMIO idiom exempted or moved to `setru`. |
| 16 | No harness mode runs the runtime's RELA PIC loaders (baremetal loader.c, FreeRTOS pic_loader.c) at a non-link address | medium | Needs a PIC-ELF build rule and an SD-card board mode. Their headers name a linker script, `klausscpu_pic.ld`, that is not in the repo. |
| 17 | picolibc has no `machine/setjmp.h` for KlaussCPU; the packed-struct Csmith run (with the target alignment screen) is not automated in `tools/validate` | small | The cloud campaign ran it by hand with a wrapper clang; no failures. |
| 19 | The assembler has no RESET or LCDCMDR; the optional `shouldReduceLoadWidth` re-measure (items 17, 40) | trivial | Cosmetic, plus full coverage of the ISA map. |
| 23 | `LEAPC.S` reaches only targets in the instruction's own section | medium | The assembler resolves (and keeps short) only a same-section target; almost every `leapc` the compiler emits names `.rodata`/`.data`, so 21 of 9447 compress in the -O1 corpus, where HARDWARE_PROPOSALS.md §2.9 counted 4447 within ±32 KiB after linking.  Getting those needs an `R_KLAUSSCPU_PCREL16` and lld relaxation (shrinking `.text`, as RISC-V's linker relaxation does), or a PIC model that keeps `.rodata` next to the code. |

## For the hardware (klaussCPU) session

| # | Item | Complexity | Notes |
|---|---|---|---|
| 20 | CPU_ARCHITECTURE.md drift (items 34, 37) | trivial | Still open at 3508f87, which updated only §13.1's interrupt sources. §5 gives the class-A SGN as bit [20], but the RTL uses op[23]. The §11 pointer row says 4 bytes / MEMGET32, but pointers are 8 bytes / LDIDX64A. (§14, the pipeline cost model, was rewritten for the current core on 2026-11-02/03.) The intro promises a calling convention: point to klausscpu-llvm ABI.md. ISA_ENCODING_V2_MAP.md's branch table leaves a gap where JMPOREL/JMPNOREL are. New with 3508f87: §8.3 still says a misaligned access "never faults" and uses the aligned slot (it traps, ERR=0A), §13.1 says IRET is the only interrupt opcode and gives the frame without bit 43 (DI/EI), and ISA_ENCODING_V2_MAP §8.3 gives LEAPC.S as `[20]`=0 while the RTL also decodes `[20]`=1 with `[3:0]`=0 (its generic short path; harmless, nothing emits it). |
| 21 | VGA board run | small | The VGA controller is in klaussCPU master since 3508f87 (merged in 8e9a477) and in the QSPI bitstream. Left: test_vga, test_vga_scan, VGA doom and the zero-copy path need a board run with the current toolchain; Zephyr already takes source 3 (its `irq_dev` app, 59.5 Hz, no lost interrupts). |
| 22 | Performance proposals | the user's call | Done since 3508f87, in QSPI: branch prediction in ID and fetch (2a, a 64-entry BTIC, a 4-entry return stack), the pipelined D-cache hit (1a stages 2 and 3), one-slot and queued stores, store-to-load and SP forwarding, the 2-cycle multiplier, the 2-entry next-line buffer and the load-to-ALU/compare bypass (HARDWARE_PROPOSALS_RESPONSE §0a-§0m in the hardware repo).  Left, all in the front end's ID-latch cone, the core's tightest timing path: the BTIC taken bubble (~2% of cycles in sim), the I-cache refill latch bubble (~3%), and a better direction predictor (~3% at stake).  Not planned: 1 store per cycle (~0.5%, a new core-cache write protocol), a general EX->EX bypass (parked at -0.6% in sim). |
| 27 | LVGL CPU rendering was slower on 3508f87 than on the bitstream before it, for identical code | small | `gui_lvgl` (render ms/frame, `fill` 63 -> 76, `gradient` 43 -> 58), layout-dependent, suspected DRAM arbitration with the VGA scan-out or the blitter, or the posted-store drain in a fill loop.  The core has changed a great deal since (pipelined D-cache, store queue, store forwarding).  Re-run `gui_lvgl` on the current QSPI image and compare with the old figures; close this if the gap is gone. |
| 31 | Zephyr's -Os is forced to -O2 because of an old -Os miscompile | small | The port's `cmake/compiler/clang/compiler_flags.cmake` maps CONFIG_SIZE_OPTIMIZATIONS (Zephyr's default) to -O2, so every Zephyr app already builds at -O2 (CONFIG_SPEED_OPTIMIZATIONS gives identical binaries).  Its note: real -Os once gave wrong kernel code (the scheduler returned garbage switch_handle values and IRET jumped into .text); -Oz, with and without LTO, passed the suite on 2026-10-28, but plain -Os was not retried.  Build the apps at real -Os and run tools/validate/zephyr on the emulator and the board: either the override is stale (drop it, or keep -O2 on purpose for speed) or there is an -Os miscompile to find. |

## Housekeeping

- `/home/graham/Documents/src/klausscpu-llvm-p5-rust` is 1.5 GB. It holds the
  LLVM sources the ABI-3 rustc build expects at that path.  Keep it while
  that rustc may need relinking (rust/README.md); otherwise delete it.  Its
  tracked files (llvm/, cmake/, third-party/) were synced from `main` for the
  phase 9 relink (the private build in `klausscpu-p5-rust-build/llvm` and the
  x.py run after `touch compiler/rustc_llvm/build.rs`, about 5 minutes); do the same (`git ls-files` into rsync) before the next.
- The new boot ROM (klaussCPU b383104) was checked after a JTAG reset, which
  reconfigures from QSPI, but not after a real power cycle.
- The two optional notes from the item 1-9 re-audit are items 2 and 15 above.

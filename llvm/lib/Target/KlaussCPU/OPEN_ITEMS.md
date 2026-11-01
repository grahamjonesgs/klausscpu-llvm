# KlaussCPU toolchain: open items

Status as of 2026-11-01: klausscpu-llvm `main` at the phase 8 milestone,
klausscpu-runtime `main` and klausscc `master` likewise,
and the board on klaussCPU `3508f87` (master with the hw-proposals and VGA
merges; the `0de5b21` bitstream, in QSPI since 2026-11-01).

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
| 24 | Loop-head alignment pads the fall-through path too: when the loop is entered by falling into its head, the alignment NOPs run once per entry | small | perf_baseline and perf_haz each run 317,811 more emulator instructions (+1.18%) since the phase 8 encodings step, only because shorter code moved `fib()`'s loop head and one NOP now runs per call. Board cycles (phase 8 milestone): `perf_baseline calls_fib` is 25.94M cycles against 26.45M before the posted-store schedule and the fetch layout, so the NOP is not visible net; a direct A/B (`-klausscpu-pref-loop-align=0` on that kernel) has not been run. Dhrystone (-O1) is the largest case: 7.4M of its 156.6M emulator instructions are the three NOPs between the FuseCmpBr loop constant's `setr` and the head of `dhry_run`'s two-iteration copy loop (already so before the codegen step). Then consider skipping the padding, or rotating the loop so it is entered by a branch, when the preheader falls through and the loop runs few iterations per entry. Keep `-klausscpu-pref-loop-align` (3508f87 still charges 1-2 cycles/iteration at line offset 12). |
| 25 | The loop-head alignment of 16 bytes is not always the best fetch phase | small | KlaussCPUFetchLayout (part B) moves or pads wide instructions off line crossings in aligned loops and took `branchy` from 16.03M to its best-offset 15.16M cycles (-5.5%) and LVGL `text` from 267 to 260 ms/frame.  Left: it does not choose the head's offset (the head stays at 0), and it does not know the redirect-line penalty of a mid-loop branch target (picolibc `sqrt` saved about 2 cycles per straddle there, the model charges 1). |
| 26 | FuseCmpBr leaves a loop exit test as `cmp; jmpcc` when the compare reads the counter before its update | small | `for (long i = a; i < N; i += 3)` and `do { ...; x++; } while (x < C)` at -O2 become `cmprv rI, C-3; ...; addi rI, rI, 3; jmpltrel` (LSR compares the old counter with an adjusted constant, and the update sits between the compare and the branch), which FuseCmpBr cannot fuse (the compare operand is redefined before the branch). Compare the new counter instead (needs the `nsw` of the add), or sink the compare behind the update. The fused form exists only for a unit-stride loop. Found writing `test_hwcg`'s range cases (which read the exit value from a load for that reason). |

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
| 23 | `LEAPC.S` reaches only targets in the instruction's own section | medium | The assembler resolves (and keeps short) only a same-section target; almost every `leapc` the compiler emits names `.rodata`/`.data`, so 21 of 9447 compress in the -O1 corpus, where HARDWARE_PROPOSALS.md §2.9 counted 4447 within ±32 KiB after linking.  Getting those needs an `R_KLAUSSCPU_PCREL16` and lld relaxation (shrinking `.text`, as RISC-V's linker relaxation does), or a PIC model that keeps `.rodata` next to the code. |

## For the hardware (klaussCPU) session

| # | Item | Complexity | Notes |
|---|---|---|---|
| 20 | CPU_ARCHITECTURE.md drift (items 34, 37) | trivial | Still open at 3508f87, which updated only §13.1's interrupt sources. §5 gives the class-A SGN as bit [20], but the RTL uses op[23]. The §11 pointer row says 4 bytes / MEMGET32, but pointers are 8 bytes / LDIDX64A. Add a §14 row for the ~4-cycle blocking D-cache hit. The intro promises a calling convention: point to klausscpu-llvm ABI.md. ISA_ENCODING_V2_MAP.md's branch table leaves a gap where JMPOREL/JMPNOREL are. New with 3508f87: §8.3 still says a misaligned access "never faults" and uses the aligned slot (it traps, ERR=0A), §13.1 says IRET is the only interrupt opcode and gives the frame without bit 43 (DI/EI), and ISA_ENCODING_V2_MAP §8.3 gives LEAPC.S as `[20]`=0 while the RTL also decodes `[20]`=1 with `[3:0]`=0 (its generic short path; harmless, nothing emits it). |
| 21 | VGA board run | small | The VGA controller is in klaussCPU master since 3508f87 (merged in 8e9a477) and in the QSPI bitstream. Left: test_vga, test_vga_scan, VGA doom and the zero-copy path need a board run with the current toolchain; with source 3 (vsync) live, item 3 matters for Zephyr. |
| 22 | Performance proposals | the user's call | 3508f87 did HARDWARE_PROPOSALS.md items 1c (posted stores), 3 (no line-crossing bubble), 6 (FBR W / imm32), 7a (DI/EI), 8 (as a trap) and 9 (short CMPRVW/MULV/LEAPC); 2b (EQ/NE FBR in EX) is built but off (no timing closure). The toolchain knows all their encodings and FuseCmpBr uses FBR W / imm32; the runtime's atomics, RTOS locks, FreeRTOS and Zephyr ports, lwIP and klauss-rt use DI/EI (`__builtin_klausscpu_di`/`_ei`; checked on the board by test_atomic's interrupt race, test_rtos, test_sync, test_diei and Zephyr `irq_soak` (viol=0 lost=0); klauss-rt's `sync.rs` calls the same intrinsics; ffi-check and `ext_atomics.llext` pass on the board), and the schedule model knows posted stores (post-RA data-port hazard). Not done: 1a pipelined D-cache hit (about −25% CoreMark), 1b load hit 4→3, 2a branch prediction (start with a return-address stack, HARDWARE_PROPOSALS_RESPONSE.md §5.2), 4 non-blocking MUL (§5.1 suggests MUL stop writing flags), 5 EX→EX forwarding. |
| 27 | LVGL CPU rendering is slower on 3508f87 than on the previous bitstream, for identical code | small | Zephyr `gui_lvgl` (board, `LVGL bench`, render ms/frame): `fill` 63 -> 76, `gradient` 43 -> 58, `rounded` 93 -> 101, `text` 245 -> 267 (after KlaussCPUFetchLayout 260; the first three also with LVGL built by the pre-part-A compiler: 77, 58, 105), `blend50`/`shadow` unchanged; the baseline numbers were identical over four earlier phases. Every other board figure got faster (mem_stream -8%, CoreMark +5%). Suspects: DRAM arbitration with the VGA scan-out or the blitter, or the posted-store drain in a fill loop. Needs a reproducer on the hardware side: the fill is a long run of sequential 32-bit stores into the framebuffer (`lv_draw_sw_blend_fill`). |
| 28 | The QSPI bitstream has no debug module (DEBUG_EN=0) | small | `validate.sh lldb-board` (and `klausscc --gdbserver --board`) fail on every check with "IDCODE 0x13631093 but no KlaussCPU debug module on USER3 ... is the bitstream built with DEBUG_EN=1?"; the lldb-on-board end-to-end of the phase 8 milestone could not run. The emulator lldb e2e passes (47/47). Needs a DEBUG_EN=1 build of master 3508f87 to be loaded (a JTAG load is enough; this session does not flash). |

## Housekeeping

- `/home/graham/Documents/src/klausscpu-llvm-p5-rust` is 1.5 GB. It holds the
  LLVM sources the ABI-v2 rustc build expects at that path.  Keep it while
  that rustc may need relinking (rust/README.md); otherwise delete it.  Its
  tracked files (llvm/, cmake/, third-party/) were synced from `main` for the
  phase 8 relink; do the same (`git ls-files` into rsync) before the next.
- The SD card's macOS `.Trashes/` still holds old ELF and PIC files.  The
  board shell's `rm` cannot remove a non-empty directory.  Delete it from a
  Mac or PC.
- The new boot ROM (klaussCPU b383104) was checked after a JTAG reset, which
  reconfigures from QSPI, but not after a real power cycle.
- The two optional notes from the item 1-9 re-audit are items 2 and 15 above.

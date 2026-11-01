# KlaussCPU toolchain: open items

Status as of 2026-11-02: klausscpu-llvm `main` at the phase 9 milestone (C ABI version 3),
klausscpu-runtime `main` and klausscc `master` likewise,
and the board on klaussCPU `3508f87` (master with the hw-proposals and VGA
merges; the `0de5b21` bitstream, in QSPI since 2026-11-01).  Everything that runs on the board was rebuilt for ABI 3 and validated at the milestone (the lldb-on-board end-to-end could not run; item 28).

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
| 25 | The loop-head alignment of 16 bytes is not always the best fetch phase | small | KlaussCPUFetchLayout (part B) moves or pads wide instructions off line crossings in aligned loops and took `branchy` from 16.03M to its best-offset 15.16M cycles (-5.5%) and LVGL `text` from 267 to 260 ms/frame.  Left: it does not choose the head's phase (the head stays at line offset 0).  Phase 4 and 8 can pay when a loop has two or more 2-word instructions (posted-store-measure REPORT.md section 5: `setr; setr` 11.02 cycles at phase 4 against 13.02 at 0, `ALU; 2xsetr; ALU` the reverse, 12.02 against 17.02), and the model, run over CoreMark, Embench and the benchmark programs, finds a cheaper phase for about 90 loops that are entered by a branch (so a padding block ahead of the head, with the NOPs behind an unconditional jump, would be dead code), by about one cycle per iteration each (a few percent of those loops' modelled cost).  Not built: the model's phase terms are not validated (head-phase effects are not in the REPORT's tables beyond the two loops above), and it needs a new unreachable pad block per loop. The premise that a mid-loop branch target charges 2 where the model has 1 did not reproduce: a grid of 60 loops with a `jmprel` to a mid-loop target (target phase 8/12/0/4 x the 2-word `setr` 0-5 instructions after it, against two 1-word ALUs in its place) shows a straddle costs 1 wherever it sits and nothing extra in the target's own line, so picolibc `sqrt`'s 2 cycles per straddle is not the redirect line (a different reason, unfound; the 60 kernels are a few lines of assembly each: `decr; jmprel Lmid; nop x s; Lmid: k ALU; setr; 3 ALU; fbri`).  Also left, from the fall-through padding: a loop that runs twice per entry (Dhrystone's copy loop) is break-even by arithmetic and was +1% cycles unaligned, so it is aligned; the NOPs there are 7.4M of Dhrystone's 156.6M instructions. |

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
| 20 | CPU_ARCHITECTURE.md drift (items 34, 37) | trivial | Still open at 3508f87, which updated only §13.1's interrupt sources. §5 gives the class-A SGN as bit [20], but the RTL uses op[23]. The §11 pointer row says 4 bytes / MEMGET32, but pointers are 8 bytes / LDIDX64A. Add a §14 row for the ~4-cycle blocking D-cache hit. The intro promises a calling convention: point to klausscpu-llvm ABI.md. ISA_ENCODING_V2_MAP.md's branch table leaves a gap where JMPOREL/JMPNOREL are. New with 3508f87: §8.3 still says a misaligned access "never faults" and uses the aligned slot (it traps, ERR=0A), §13.1 says IRET is the only interrupt opcode and gives the frame without bit 43 (DI/EI), and ISA_ENCODING_V2_MAP §8.3 gives LEAPC.S as `[20]`=0 while the RTL also decodes `[20]`=1 with `[3:0]`=0 (its generic short path; harmless, nothing emits it). |
| 21 | VGA board run | small | The VGA controller is in klaussCPU master since 3508f87 (merged in 8e9a477) and in the QSPI bitstream. Left: test_vga, test_vga_scan, VGA doom and the zero-copy path need a board run with the current toolchain; Zephyr already takes source 3 (its `irq_dev` app, 59.5 Hz, no lost interrupts). |
| 22 | Performance proposals | the user's call | 3508f87 did HARDWARE_PROPOSALS.md items 1c (posted stores), 3 (no line-crossing bubble), 6 (FBR W / imm32), 7a (DI/EI), 8 (as a trap) and 9 (short CMPRVW/MULV/LEAPC); 2b (EQ/NE FBR in EX) is built but off (no timing closure). The toolchain knows all their encodings and FuseCmpBr uses FBR W / imm32; the runtime's atomics, RTOS locks, FreeRTOS and Zephyr ports, lwIP and klauss-rt use DI/EI (`__builtin_klausscpu_di`/`_ei`; checked on the board by test_atomic's interrupt race, test_rtos, test_sync, test_diei and Zephyr `irq_soak` (viol=0 lost=0); klauss-rt's `sync.rs` calls the same intrinsics; ffi-check and `ext_atomics.llext` pass on the board), and the schedule model knows posted stores (post-RA data-port hazard). Not done: 1a pipelined D-cache hit (about −25% CoreMark), 1b load hit 4→3, 2a branch prediction (start with a return-address stack, HARDWARE_PROPOSALS_RESPONSE.md §5.2), 4 non-blocking MUL (§5.1 suggests MUL stop writing flags), 5 EX→EX forwarding. |
| 27 | LVGL CPU rendering is slower on 3508f87 than on the previous bitstream, for identical code | small | Zephyr `gui_lvgl` (board, `LVGL bench`, render ms/frame): `fill` 63 -> 76, `gradient` 43 -> 58, `rounded` 93 -> 101, `text` 245 -> 267 (after KlaussCPUFetchLayout 260; the first three also with LVGL built by the pre-part-A compiler: 77, 58, 105), `blend50`/`shadow` unchanged; the baseline numbers were identical over four earlier phases. Every other board figure got faster (mem_stream -8%, CoreMark +5%). The Zephyr interrupt rework then moved `text` 260 -> 238 and `gradient` 58 -> 64 (copy 4 -> 9 ms; the blitter's own cycles per frame +15%) with identical compiled render code: unchanged by uniform layout shifts, the ISR stack size, INT_MASK, DI in the entry stub or the cycle-counter fix, and partly by where the ISR stack sits (a private stack: 245 / 63), so the figures are deterministic (the old ELF still gives 58 / 259, the new one 64 / 238, rerun on the board) but depend on the build's layout, outside the compiled render code. Suspects: DRAM arbitration with the VGA scan-out or the blitter, or the posted-store drain in a fill loop. Needs a reproducer on the hardware side: the fill is a long run of sequential 32-bit stores into the framebuffer (`lv_draw_sw_blend_fill`). |
| 28 | Self-modifying-code hole in 3508f87 (the QSPI bitstream) | small (hardware) | Found by the hardware session: a store that rewrites an instruction already latched from the fetch buffer (for example the very next instruction) can let the OLD instruction execute. Compiled code never stores into its own `.text`, and the runtime's loaders (LLEXT, PIC loaders, netboot) never write the line they are about to run, so nothing is known to hit it; a JIT that patches and runs at once could. Fixed on klaussCPU branch bp-ras (in the hw-m9 and mem-early candidates); close this when that reaches QSPI. lldb on the board: `validate.sh lldb-board` passes 47/47 with the DEBUG_EN=1 build of 3508f87 (~/.cache/klausscpu-debug-build/KlaussCPU_DEBUG_EN_3508f87.bit) loaded over JTAG; it stays out of QSPI until its timing margin grows (+0.024 ns). |

## Housekeeping

- `/home/graham/Documents/src/klausscpu-llvm-p5-rust` is 1.5 GB. It holds the
  LLVM sources the ABI-3 rustc build expects at that path.  Keep it while
  that rustc may need relinking (rust/README.md); otherwise delete it.  Its
  tracked files (llvm/, cmake/, third-party/) were synced from `main` for the
  phase 9 relink (the private build in `klausscpu-p5-rust-build/llvm` and the
  x.py run after `touch compiler/rustc_llvm/build.rs`, about 5 minutes); do the same (`git ls-files` into rsync) before the next.
- The SD card's macOS `.Trashes/` still holds old ELF and PIC files (ABI 1 or 2; the live `.llext` set on the card was replaced with ABI 3 builds at the phase 9 milestone).  The
  board shell's `rm` cannot remove a non-empty directory.  Delete it from a
  Mac or PC.
- The new boot ROM (klaussCPU b383104) was checked after a JTAG reset, which
  reconfigures from QSPI, but not after a real power cycle.
- The two optional notes from the item 1-9 re-audit are items 2 and 15 above.

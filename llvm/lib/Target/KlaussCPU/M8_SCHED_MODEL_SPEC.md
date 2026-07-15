# M8 — KlaussCPU LLVM Scheduling Model + TTI re-tune (spec)

**Status:** spec / ready to implement. Latencies are board-final (M5–M7 pipeline on
silicon, 2026-07-13). Cross-repo: RTL is `fpga/KlaussCPU`; this is the `klausscpu-llvm`
half. Gate behind a subtarget feature for A/B (see §7).

## 1. Why now — the measured lever

Before M7 the core was fetch-bound (IF_MISS 33–73 % of cycles), so the scheduler had
nothing to gain and the TTI *disabled* all unrolling. M7a (in-core fill-through I-cache,
board-verified) collapsed IF_MISS and dropped CPI 22–44 %. That **exposed the next
bottleneck**, and `perf_haz` (real compiled kernels, board) quantifies it:

| kernel | M7a CPI | **DATA (GPR RAW)** | notes |
|---|--:|--:|---|
| ptr_chase | 4.57 | **61.2 %** | serial dependent loads, now fetch-cheap |
| alu       | 2.38 | **21.1 %** | dependent ALU chain |
| branchy   | 2.37 | **18.6 %** | xorshift dep chain |
| muldiv    | 2.68 | **17.6 %** | a*=i chain around the mul |

`DATA` = the M6 `perf_stall[0]` counter = a consumer in ID whose producer is 1 slot
ahead (in EX), or a load in MEM. The current `SchedMachineModel` is a stub
(`CompleteModel=0`, no per-op latencies), so llvm's generic scheduler assumes **latency 1
for every op** and never spreads a dependent chain — every back-to-back dependent pair
pays the 1-cycle producer-in-EX bubble. **Telling the scheduler the true latencies is the
fix**, and unrolling (now that fetch is cheap) gives it the independent work to fill the
gaps with.

## 2. Pipeline latencies (derived from `pipeline_core.sv`, M6 forwarding)

Stages IF→ID→EX→MEM→WB; operands latch at ID→EX. Forwarding (`pipeline_core.sv`
`mem_fwd_ok`/`b_wb`, RAW-stall `raw_stall`/`hit1..hitd`):
- **ALU (single-cycle) → ALU:** producer-in-EX ⇒ 1 bubble; then forwards from MEM
  (`mem_fwd_ok = b_mem && !load`). A consumer at distance ≥2 sees no stall ⇒ **latency 2**.
- **Load → use:** loads do *not* forward from MEM (`mem_fwd_ok` excludes loads); a
  dependent use stalls through EX **and** MEM and forwards from WB ⇒ **latency 3**
  (matches the stub's `LoadLatency`).
- **MUL:** EX-busy `mul_cnt==3` = 4 cycles; measured avglat 5.0c ⇒ **latency 5**.
- **DIV/MOD:** iterative to 64 steps after a CLZ-skip `DIVIDE_PREP`; operand-dependent,
  worst ~65c, small-operand avg ~2–10c. Model a single conservative **latency 20**
  (it EX-serializes the pipe regardless; exact value barely moves scheduling).
- **Flag writer → flag reader** (branch cond, ADC/SBC/rotate-through-carry): flag-writer
  in EX blocks the reader 1 bubble ⇒ **flag latency 2** (`flag_busy`, `perf_stall[2]`).
- **SP writer → SP reader** (CALL/PUSH/POP/class-9): ⇒ **latency 2** (`sp_busy`,
  `perf_stall[3]`).
- **Store:** no GPR result; 1 issue slot. `MEM_WAIT` (miss penalty) is microarchitectural,
  not schedulable — do **not** inflate store latency to model misses.
- **Branch mispredict:** static predict-not-taken; a taken redirect refills the IFB via a
  ~2-cycle I-cache lookup ⇒ set **`MispredictPenalty = 3`** (was 8, tuned for the old
  DDR-fetch core — now stale).

## 3. `KlaussCPU.td` — model + resources + writes

Replace the stub `KlaussCPUSchedModel` block:

```tablegen
def KlaussCPUModel : SchedMachineModel {
  let IssueWidth        = 1;      // single-issue in-order
  let MicroOpBufferSize = 0;      // in-order, no OoO buffer
  let LoadLatency       = 3;      // load→use (forwards from WB only)
  let MispredictPenalty = 3;      // taken-redirect I-cache refill (post-M7)
  let CompleteModel     = 1;      // every instr gets a SchedRW (§4); flip to 0 if noisy
  let PostRAScheduler   = 1;      // enable the post-RA list scheduler (spreads deps)
}

// one in-order pipe; MUL and DIV are the same EX slot held busy (EX-serializing)
let SchedModel = KlaussCPUModel in {
  def KCUnit : ProcResource<1>;          // the single EX/issue slot

  def : WriteRes<WriteALU,   [KCUnit]> { let Latency = 2; }
  def : WriteRes<WriteShift, [KCUnit]> { let Latency = 2; }
  def : WriteRes<WriteCmp,   [KCUnit]> { let Latency = 2; }
  def : WriteRes<WriteLoad,  [KCUnit]> { let Latency = 3; }
  def : WriteRes<WriteStore, [KCUnit]> { let Latency = 1; }
  def : WriteRes<WriteMul,   [KCUnit]> { let Latency = 5;  let ResourceCycles = [5]; }
  def : WriteRes<WriteDiv,   [KCUnit]> { let Latency = 20; let ResourceCycles = [20]; }
  def : WriteRes<WriteFlags, [KCUnit]> { let Latency = 2; }
  def : WriteRes<WriteSP,    [KCUnit]> { let Latency = 2; }
  def : WriteRes<WriteBranch,[KCUnit]> { let Latency = 1; }
  // reads have no extra advance — the latencies above already encode the bubble
  def : ReadAdvance<ReadDefault, 0>;
}

def : ProcessorModel<"generic", KlaussCPUModel, []>;
```

Define the `SchedWrite`/`SchedRead` symbols once (new `KlaussCPUSchedule.td`, included
from `KlaussCPU.td`):

```tablegen
def WriteALU   : SchedWrite;  def WriteShift : SchedWrite;  def WriteCmp   : SchedWrite;
def WriteLoad  : SchedWrite;  def WriteStore : SchedWrite;  def WriteMul   : SchedWrite;
def WriteDiv   : SchedWrite;  def WriteFlags : SchedWrite;  def WriteSP    : SchedWrite;
def WriteBranch: SchedWrite;  def ReadDefault: SchedRead;
```

`ResourceCycles=[5]/[20]` on MUL/DIV models the EX-serialization (the unit is held, so the
scheduler won't pack another op behind a multiply — matches the RTL busy interlock).

## 4. `KlaussCPUInstrInfo.td` — attach `SchedRW`

Attach on the base classes so it inherits, override on the outliers:
- `RRR` arithmetic/bitwise (ADDR/SUBR/ANDR/ORR/XORR): `let SchedRW = [WriteALU]`.
  Flag-setting arithmetic additionally writes flags: `[WriteALU, WriteFlags]`.
- Shifts (SHLR/SHRR/…): `[WriteShift]`.
- Compares (CMPRR_I, CMP*R): `[WriteCmp, WriteFlags]`.
- **MUL** (MULR/MULUR/MULHR/MULHUR): override `let SchedRW = [WriteMul]`.
- **DIV/MOD** (DIVR/DIVUR/MODR/MODUR): override `let SchedRW = [WriteDiv]`.
- Loads (MEMGET8/16/32/64, LDIDX*): `[WriteLoad]`.
- Stores (MEMSET8/16/32/64, STIDX*): `[WriteStore]`.
- Branches/jumps (JMP*, Bcc): `[WriteBranch]`; CALL: `[WriteBranch, WriteSP]`;
  PUSH/POP/ADDSP: `[WriteSP]`.
- Imm-immediate ALU forms (ADDI/…): same class as their reg forms.

With `CompleteModel=1` every def must resolve a `SchedRW`; if a corner def is missed the
build errors — start with `CompleteModel=0` if that's noisy, tighten later.

## 5. `KlaussCPUTargetTransformInfo.cpp` — re-tune unrolling (I-cache-aware)

Today `getUnrollingPreferences` hard-disables everything (`MaxCount=1`, `Partial/Runtime=
false`) because the old core was fetch-bound with a 1-line IFB. M7 changed that — fetch
hits are ~cheap — so **moderate** unrolling now pays: it amortizes loop overhead and,
crucially, gives the (now latency-aware) scheduler independent iterations to fill the
2-cycle dependent-chain gaps. Guardrail: the I-cache is **8 KB (512×16 B lines)** — do not
unroll a hot loop past a fraction of that or conflict misses reappear.

```cpp
void KlaussCPUTTIImpl::getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
    TTI::UnrollingPreferences &UP, OptimizationRemarkEmitter *ORE) {
  UP.Partial   = true;     // allow partial unrolling of larger loops
  UP.Runtime   = true;     // and unknown trip counts (memory/pointer loops)
  UP.MaxCount  = 4;        // cap: keep the unrolled body well within the 8 KB I-cache
  UP.Threshold        = 200;   // full-unroll budget (~ a few hundred bytes of code)
  UP.PartialThreshold = 100;   // partial budget — stay < ~1 KB unrolled
  UP.UnrollAndJam = false;     // still off (body duplication, little upside here)
  UP.UpperBound   = true;      // OK to use a trip-count upper bound
  UP.OptSizeThreshold = 0;     // never grow code under -Os
}
```
Leave `getPeelingPreferences` disabled for now (peeling has little to offer these kernels).
Tune the thresholds empirically against the I-cache miss rate (add an `ICACHE_MISS`
counter in M7b/M7c and watch it while sweeping).

## 6. Expected effect

Directly attacks the DATA column: the scheduler, knowing WriteALU=2 / WriteLoad=3, will
interleave independent work (from unrolling or adjacent code) between producer and
consumer. Best cases are the dependent-chain kernels (alu, branchy, muldiv-chain) where
DATA is 18–21 %; ptr_chase (DATA 61 %) is largely *irreducible* serial pointer-chasing —
scheduling can only hide the head/tail, so its real fix is memory (D-cache / non-blocking
loads), not the sched model. Realistic target: claw back a meaningful fraction of the
18–21 % DATA on the compute kernels; UART must stay bit-identical (this is a pure perf
transform).

## 7. Verify / A-B (the discipline)

1. Gate behind a subtarget feature (`+sched-model`) so A/B is one flag, mirroring the
   flag-day pattern in `PIPELINE_MASTER_PLAN.md` §8.
2. **Correctness:** the full baremetal + `klatest` suites must be **UART-bit-identical** —
   scheduling changes issue order, never semantics. Re-run `--emulate` goldens + board
   `run_m5e_board` 7/7.
3. **Perf:** rebuild the ELFs, run **`perf_haz`** on the M7 bitstream, diff the `HAZ,`
   lines vs `fpga/KlaussCPU/perf/m7/haz_real_m7a.csv`. Success = DATA % down on the
   compute kernels with **no** IF_MISS/MEM_WAIT regression and CPI down. (`perf_baseline`
   stays pinned; `perf_haz` is the hazard A/B vehicle — it's its own ELF.)
4. Watch code size / I-cache: if `perf_haz` shows IF_MISS creeping back up, the unroll
   thresholds are too high — back them off.

## 8. Independent LLVM wins (not this spec, but adjacent — PERFORMANCE_REVIEW)

These don't need the sched model and stack with it:
- **A3** compare/flag reuse (`optimizeCompareInstr` + count-down-loop flag reuse) — no
  flag-liveness modeling exists today; biggest pure-software CPI win on loop-heavy code.
- **E3** frame-pointer elimination via `LDIDXSP/STIDXSP` (RTL + backend) — deletes the
  unconditional PUSH/POP R15 on every function and frees a 16th GPR.
Do A3 alongside M8 — it reduces the branch/compare instruction stream the scheduler works.
```

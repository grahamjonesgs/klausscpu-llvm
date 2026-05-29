# KlaussCPU performance baseline

Reference numbers from [programs/perf_baseline.c](programs/perf_baseline.c) for
tracking CPU/cache improvements over time. Re-run `perf_baseline` after a change
and diff the `CSV,` lines against the table below.

**Comparability:** numbers are only apples-to-apples when the `#define` tunables
in `perf_baseline.c` (`ALU_ITERS`, `MEM_WORDS`, `MEM_REPS`, `CHASE_STEPS`,
`BR_ITERS`, `FIB_N`, `MD_ITERS`) and the build flags are unchanged. The counters
are global and free-running, so capture on an **idle** system.

---

## Baseline — 2026-05-29

- Build: `-O1` runtime build, `i_Clk` = 100 MHz, L1 = 2-way / 2048 sets / 16 B line / 64 KB.
- System: idle (`int=0% idle=0%` in all kernels → no concurrent ISR/network).
- Working set: 256 KiB (> 64 KB L1).

### CSV anchor (name, ms, cycles, instr, cpi_milli, missrate_bp, taken_bp)

```
CSV,alu,725,72501653,6500025,11154,0,0
CSV,mem_stream,316,31546085,1966143,16044,500,4999
CSV,ptr_chase,384,38382359,2400022,15992,478,0
CSV,branchy,1506,150620587,12687118,11871,0,5627
CSV,calls_fib,2109,210838408,16455338,12812,0,4999
CSV,muldiv,151,15150709,750021,20200,0,0
```

### Key derived metrics

| kernel | CPI | fetch% | exec% | div% | cache stall | avg miss pen | notes |
|---|---|---|---|---|---|---|---|
| alu | 11.15 | 80.0 | 20.0 | – | 4.7k | – | compute-bound; i-fetch hits cache (rdMiss=50) |
| mem_stream | 16.04 | 53.2 | 46.8 | – | 8.61M (27%) | 65.6c | writeback-heavy (wb=69k) |
| ptr_chase | 15.99 | 58.4 | 41.6 | – | 9.16M (24%) | 53.1c | dependent random reads |
| branchy | 11.87 | 78.4 | 21.6 | – | ~0 | – | taken rate 56.3% |
| calls_fib | 12.81 | 70.2 | 29.8 | – | ~0 | – | CALL 3.1% / IND(ret) 3.1% |
| muldiv | 20.20 | 42.2 | 13.2 | 42.9 | ~0 | – | divide 65c/op, multiply 5c/op |

### Conclusions (see also memory note `project_perf_baseline`)

1. **Fetch-bound.** ~70–80% of cycles on compute code are in the fetch/decode FSM,
   not execute, and instruction fetches hit the cache — so it is multicycle-FSM
   overhead, not memory. Overlapping fetch with execute (pipelining) is the
   ~3.4–5× lever (ideal CPI ≈ CPI × exec% ≈ 2.2–3.8).
2. **Memory stall ~25%** on memory-bound code; ~50–65 cyc DDR miss penalty;
   low miss rate (~5%). Secondary.
3. **Divide = 65 cyc/op** (multiply 5). Niche.
4. **Loop codegen** emits a redundant unconditional back-jump (`BR==JMP==iters,
   taken=1`); a single conditional back-branch would cut ~7–8% of instructions
   in loop-heavy code. Backend-fixable; measurable here.

### Measurement note

The per-kernel `(mix sum vs instr)` self-check is off by a constant **+29** every
run — a harness artifact (the counter-read loads retire between the non-atomic
INSTR and mix-counter snapshots), not a hardware counter bug. Benign (0.0004%).

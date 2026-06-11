/*
 * board_io.h — KlaussCPU board MMIO: LEDs, switches, 7-seg, and the CPU
 * performance counters.  Shared by the shell commands (shell_cmds.c) and the
 * HTTP control/telemetry API (webapi.c) so the register map lives in one place.
 */
#ifndef KLAUSSCPU_BOARD_IO_H_
#define KLAUSSCPU_BOARD_IO_H_

#include <stdint.h>

#define BOARD_REG(a)    (*(volatile uint32_t *)(unsigned long)(a))
#define BOARD_REG64(a)  (*(volatile uint64_t *)(unsigned long)(a))

#define BOARD_REG_LEDS      BOARD_REG(0xF0040000u)
#define BOARD_REG_SWITCHES  BOARD_REG(0xF0040008u)
#define BOARD_REG_SEG_ALL   BOARD_REG(0xF0030010u)

static inline void     board_leds_set(uint16_t v)  { BOARD_REG_LEDS = v; }
static inline uint16_t board_leds_get(void)         { return (uint16_t)BOARD_REG_LEDS; }
static inline uint16_t board_switches_get(void)     { return (uint16_t)BOARD_REG_SWITCHES; }
static inline void     board_seg_set(uint32_t v)    { BOARD_REG_SEG_ALL = v; }
static inline uint32_t board_seg_get(void)          { return BOARD_REG_SEG_ALL; }

/* CPU performance counters — cumulative since power-on (CPU pipeline block at
 * 0xF00D, cache block at 0xF005).  Reads are non-destructive; sample twice and
 * subtract for rates over a window. */
struct board_perf {
	uint64_t cycles, instr, fetch, exec, mul, div, intc, idle;
	uint64_t mul_ops, div_ops, int_ops;
	uint64_t alu, load, store, branch, taken, jump, call, ind, other;
	uint64_t rh, rm, wh, wm, wb, stall;
};

static inline void board_perf_read(struct board_perf *s)
{
	s->cycles  = BOARD_REG64(0xF00D0008u); s->instr  = BOARD_REG64(0xF00D0010u);
	s->fetch   = BOARD_REG64(0xF00D0018u); s->exec   = BOARD_REG64(0xF00D0020u);
	s->mul     = BOARD_REG64(0xF00D0028u); s->div    = BOARD_REG64(0xF00D0030u);
	s->intc    = BOARD_REG64(0xF00D0038u); s->idle   = BOARD_REG64(0xF00D0040u);
	s->mul_ops = BOARD_REG64(0xF00D0048u); s->div_ops = BOARD_REG64(0xF00D0050u);
	s->int_ops = BOARD_REG64(0xF00D0058u);
	s->alu     = BOARD_REG64(0xF00D0060u); s->load   = BOARD_REG64(0xF00D0068u);
	s->store   = BOARD_REG64(0xF00D0070u); s->branch = BOARD_REG64(0xF00D0078u);
	s->taken   = BOARD_REG64(0xF00D0080u); s->jump   = BOARD_REG64(0xF00D0088u);
	s->call    = BOARD_REG64(0xF00D0090u); s->ind    = BOARD_REG64(0xF00D0098u);
	s->other   = BOARD_REG64(0xF00D00A0u);
	s->rh      = BOARD_REG64(0xF0050040u); s->rm     = BOARD_REG64(0xF0050048u);
	s->wh      = BOARD_REG64(0xF0050050u); s->wm     = BOARD_REG64(0xF0050058u);
	s->wb      = BOARD_REG64(0xF0050060u); s->stall  = BOARD_REG64(0xF0050068u);
}

#endif /* KLAUSSCPU_BOARD_IO_H_ */

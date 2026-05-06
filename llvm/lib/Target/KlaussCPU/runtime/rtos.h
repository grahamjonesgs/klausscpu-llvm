// rtos.h — KlaussCPU minimal preemptive RTOS public API.
//
// Timer interrupt fires every RTOS_TICK_CYCLES cycles (default 1 M = 10 ms
// at 100 MHz) and calls the round-robin scheduler.  Each task has its own
// stack; context (R0–R15 + hardware IRET slot) is saved/restored in assembly.
//
// Usage:
//   rtos_init();
//   task_create("name", entry_fn);
//   ...
//   rtos_start();   // never returns

#ifndef RTOS_H
#define RTOS_H

#include <stdint.h>

// ── Configuration ──────────────────────────────────────────────────────────

#define RTOS_MAX_TASKS      8
#define RTOS_STACK_WORDS    256     // 256 × 8 B = 2 KB per task
#define RTOS_TICK_CYCLES    1000000 // 10 ms @ 100 MHz

// ── Task control block ─────────────────────────────────────────────────────
// sp MUST remain at offset 0 — the assembly handler reads/writes it directly.

typedef enum { TASK_READY = 0, TASK_RUNNING, TASK_BLOCKED, TASK_DEAD } TaskState;

typedef struct {
    uint32_t    sp;           // offset  0: saved stack pointer
    uint32_t    stack_base;   // offset  4: lowest valid stack address (canary)
    uint32_t    stack_top;    // offset  8: initial SP (highest address)
    TaskState   state;        // offset 12
    int         id;           // offset 16
    const char *name;         // offset 20: display name (pointer = 32-bit)
    uint32_t    switches;     // offset 24: times this task was switched in
} TCB;

// ── API ────────────────────────────────────────────────────────────────────

void     rtos_init(void);
int      task_create(const char *name, void (*entry)(void));
void     rtos_start(void);          // installs timer, launches first task, never returns
uint32_t rtos_task_switches(int id); // query context-switch count for task id

// ── Internals (used by context_switch.S — not for application code) ────────

void pick_next_task(void);          // called from timer handler; updates g_current_tcb
extern void timer_handler(void);    // assembly ISR installed as INT_VEC0
extern void rtos_first_task(void);  // assembly trampoline; launches first task

extern TCB     *g_current_tcb;
extern uint32_t g_kernel_stack_top;

#endif // RTOS_H

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
#define RTOS_STACK_WORDS    2048    // 2048 × 8 B = 16 KB per task
#define RTOS_TICK_CYCLES    1000000 // 10 ms @ 100 MHz

// ── Task control block ─────────────────────────────────────────────────────
// sp MUST remain at offset 0 — the assembly handler reads/writes it directly.

typedef enum { TASK_READY = 0, TASK_RUNNING, TASK_BLOCKED, TASK_DEAD } TaskState;

// What synchronisation object (if any) the task is currently blocking on.
typedef enum {
    WAIT_NONE = 0,  // not blocked, or timed sleep (rtos_sleep_ticks)
    WAIT_TICK,      // blocking via rtos_sleep_ticks — woken by tick_count_update
    WAIT_SEM,       // blocking on a semaphore — woken by rtos_sem_signal
    WAIT_FLAG,      // blocking on an event flag — woken by rtos_flag_set
} WaitType;

typedef struct {
    uint32_t    sp;           // offset  0: saved stack pointer (assembly accesses this)
    uint32_t    stack_base;   // offset  4: lowest valid stack address (canary)
    uint32_t    stack_top;    // offset  8: initial SP (highest address)
    TaskState   state;        // offset 12
    int         id;           // offset 16
    const char *name;         // offset 20: display name (pointer = 64-bit on this ABI)
    uint32_t    switches;     // offset 28: times this task was switched in
    uint32_t    wake_tick;    // offset 32: tick count for WAIT_TICK wakeup
    WaitType    wait_type;    // offset 36: which sync object we are waiting on
    void       *wait_obj;     // offset 40: pointer to the RtosSem or RtosFlag (NULL=none)
} TCB;

// ── Mutex (critical section via interrupt mask) ────────────────────────────
//
// On a single-core CPU the only source of preemption is the timer interrupt.
// Disabling it is sufficient for mutual exclusion — no hardware CAS needed.
//
// Usage:
//   rtos_mutex_lock();
//   ... shared resource access ...
//   rtos_mutex_unlock();
//
// Note: do not hold the lock across long operations (e.g. full printf calls)
// as it adds jitter to the RTOS tick.  For output, prefer uart_puts/uart_putc
// which have no shared mutable state.

#include "mmio.h"

static inline void rtos_mutex_lock(void)   { REG_INT_MASK = 0; }
static inline void rtos_mutex_unlock(void) { REG_INT_MASK = 1; }

// ── API ────────────────────────────────────────────────────────────────────

void     rtos_init(void);
int      task_create(const char *name, void (*entry)(void));
void     rtos_start(void);           // installs timer, launches first task, never returns
uint32_t rtos_task_switches(int id); // query context-switch count for task id

// Yield the CPU to the next ready task immediately.  The caller resumes
// normally when re-scheduled.
// PRECONDITION: must be called from within an RTOS task (after rtos_start).
// Calling from main() before rtos_start() is undefined behaviour.
void rtos_yield(void);

// Block the calling task for at least `ticks` timer periods (10 ms each at
// default config), then resume.  Other tasks run while this task sleeps.
// No-op if called before rtos_start() (g_current_tcb == NULL).
void rtos_sleep_ticks(uint32_t ticks);

// ── Semaphore ──────────────────────────────────────────────────────────────
// Counting semaphore.  Blocks the caller when count reaches 0.
// Safe to signal from any task; safe to wait from any task after rtos_start.
//
// Typical use — binary semaphore (mutual exclusion):
//   RtosSem s;  rtos_sem_init(&s, 1);
//   rtos_sem_wait(&s); ... critical section ... rtos_sem_signal(&s);
//
// Typical use — counting semaphore (producer/consumer):
//   RtosSem slots, items;
//   rtos_sem_init(&slots, CAPACITY);  rtos_sem_init(&items, 0);
//   producer: rtos_sem_wait(&slots); put(); rtos_sem_signal(&items);
//   consumer: rtos_sem_wait(&items); get(); rtos_sem_signal(&slots);

typedef struct { volatile int count; } RtosSem;

void rtos_sem_init(RtosSem *s, int count);
void rtos_sem_wait(RtosSem *s);    // P — decrement; block if already 0
void rtos_sem_signal(RtosSem *s);  // V — wake one waiter, or increment if none

// ── Event flag ─────────────────────────────────────────────────────────────
// Binary event flag with auto-clear semantics.
// rtos_flag_set   — wake one waiting task (hand-off), or store pending if none.
// rtos_flag_wait  — take the flag if pending; block until set otherwise.
//
// Typical use — task synchronisation:
//   RtosFlag f;  rtos_flag_init(&f);
//   task A: do_work(); rtos_flag_set(&f);
//   task B: rtos_flag_wait(&f); use_result();

typedef struct { volatile uint32_t bits; } RtosFlag;

void rtos_flag_init(RtosFlag *f);
void rtos_flag_set(RtosFlag *f);   // signal; wake one waiter or set pending
void rtos_flag_wait(RtosFlag *f);  // wait until set; auto-clear on take

// ── Internals (used by context_switch.S — not for application code) ────────

void pick_next_task(void);          // called from timer handler; updates g_current_tcb
void tick_count_update(void);       // increments g_tick_count and wakes sleepers
extern void timer_handler(void);    // assembly ISR installed as INT_VEC0
extern void rtos_first_task(void);  // assembly trampoline; launches first task

extern TCB     *g_current_tcb;
extern uint32_t g_kernel_stack_top;
extern uint32_t g_tick_count;       // incremented once per timer tick

#endif // RTOS_H

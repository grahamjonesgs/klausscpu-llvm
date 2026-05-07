// rtos.c — KlaussCPU minimal preemptive RTOS.
//
// Scheduler is round-robin over TASK_READY tasks.
// Context switch is performed in assembly (context_switch.S); this file
// provides the C-level scheduler and task management.

#include "../rtos.h"
#include "../mmio.h"
#include <string.h>
#include <stdio.h>

#define CANARY 0xDEADC0DEDEADC0DEULL

// ── Kernel stack (scheduler runs here, never on a task stack) ──────────────

#define KERNEL_STACK_WORDS 128
static uint64_t kernel_stack[KERNEL_STACK_WORDS];
uint32_t        g_kernel_stack_top;   // set at runtime — static init broken for addrs

// ── Task table ─────────────────────────────────────────────────────────────

static TCB      tasks[RTOS_MAX_TASKS];
static uint64_t stacks[RTOS_MAX_TASKS][RTOS_STACK_WORDS];
static int      n_tasks;

TCB     *g_current_tcb;
uint32_t g_tick_count;

// ── task_create ────────────────────────────────────────────────────────────

int task_create(const char *name, void (*entry)(void)) {
    if (n_tasks >= RTOS_MAX_TASKS) return -1;

    TCB      *t  = &tasks[n_tasks];
    uint64_t *sk = stacks[n_tasks];

    // Canary at the bottom word of the stack (first to be overwritten on overflow).
    sk[0] = CANARY;

    // Build initial stack frame as if the task was interrupted before starting.
    // Stack grows downward; start from the top.
    uint64_t *sp = sk + RTOS_STACK_WORDS;

    // Fake IRET context slot: bits[31:0] = entry PC, bits[42:39] = INT_MASK=1.
    // IRET will restore PC = entry and re-enable the timer interrupt.
    *--sp = (uint64_t)(uint32_t)(unsigned long)entry | ((uint64_t)1 << 39);

    // Fake saved R0–R15, all zero (16 slots × 8 bytes).
    sp -= 16;
    memset(sp, 0, 16 * 8);

    t->sp         = (uint32_t)(unsigned long)sp;
    t->stack_base = (uint32_t)(unsigned long)sk;
    t->stack_top  = (uint32_t)(unsigned long)(sk + RTOS_STACK_WORDS);
    t->state      = TASK_READY;
    t->id         = n_tasks;
    t->name       = name;
    t->switches   = 0;

    n_tasks++;
    return t->id;
}

// ── tick_count_update ──────────────────────────────────────────────────────
// Called from timer_handler ONLY (not from rtos_yield).
// Increments the global tick counter and wakes any BLOCKED tasks whose
// sleep period has elapsed.

void tick_count_update(void) {
    g_tick_count++;
    for (int i = 0; i < n_tasks; i++) {
        // Only wake tasks that are sleeping on the tick counter (WAIT_TICK).
        // Tasks blocked on semaphores or flags are woken by their respective
        // signal/set functions, not by the timer tick.
        if (tasks[i].state    == TASK_BLOCKED &&
            tasks[i].wait_type == WAIT_TICK   &&
                (int32_t)(g_tick_count - tasks[i].wake_tick) >= 0) {
            tasks[i].state     = TASK_READY;
            tasks[i].wait_type = WAIT_NONE;
        }
    }
}

// ── pick_next_task ─────────────────────────────────────────────────────────
// Called from timer_handler and rtos_yield (assembly), on the kernel stack.
// Marks current RUNNING task READY (unless it is BLOCKED); selects the next
// READY task round-robin; marks it RUNNING and updates g_current_tcb.

void pick_next_task(void) {
    int cur = g_current_tcb ? g_current_tcb->id : -1;

    if (g_current_tcb && g_current_tcb->state == TASK_RUNNING)
        g_current_tcb->state = TASK_READY;

    for (int i = 1; i <= n_tasks; i++) {
        int next = (cur + i) % n_tasks;
        if (tasks[next].state == TASK_READY) {
            g_current_tcb = &tasks[next];
            g_current_tcb->state = TASK_RUNNING;
            g_current_tcb->switches++;
            return;
        }
    }
    // No task ready — stay with current (or idle; should not happen in basic use).
}

// ── Stack canary check ─────────────────────────────────────────────────────

static void check_canaries(void) {
    for (int i = 0; i < n_tasks; i++) {
        uint64_t *sk = stacks[i];
        if (sk[0] != CANARY) {
            // Stack overflow detected — stall with visible error.
            REG_LEDS    = (uint32_t)(1u << i);
            REG_SEG_ALL = 0x0EEEEEEE;   // "EEEEEEEE"
            while (1) {}
        }
    }
}

// ── rtos_init ──────────────────────────────────────────────────────────────

void rtos_init(void) {
    g_kernel_stack_top = (uint32_t)(unsigned long)(kernel_stack + KERNEL_STACK_WORDS);
    n_tasks       = 0;
    g_current_tcb = 0;

    // Diagnostic: verify handler address and first instruction word.
    uint32_t handler_addr = (uint32_t)(unsigned long)timer_handler;
    uint32_t first_word   = *(volatile uint32_t *)(unsigned long)handler_addr;
    printf("timer_handler addr  = 0x%08lx\n", (unsigned long)handler_addr);
    printf("timer_handler[0]    = 0x%08lx (expect 0x00004000 = push r0)\n",
           (unsigned long)first_word);

    // Install handler and set period.  Timer is NOT unmasked here;
    // rtos_first_task (assembly) unmasks it after switching to the first
    // task's stack so no tick can fire before the RTOS is fully live.
    REG_INT_VEC(INT_SRC_TIMER) = handler_addr;
    REG_TIMER_PER = RTOS_TICK_CYCLES;
    printf("INT_VEC[timer]      = 0x%08lx\n",
           (unsigned long)REG_INT_VEC(INT_SRC_TIMER));
}

// ── rtos_start ─────────────────────────────────────────────────────────────

void rtos_start(void) {
    pick_next_task();       // select first task → sets g_current_tcb
    rtos_first_task();      // assembly: switch SP, restore regs, enable timer, IRET
    __builtin_unreachable();
}

// ── rtos_task_switches ─────────────────────────────────────────────────────

uint32_t rtos_task_switches(int id) {
    if (id < 0 || id >= n_tasks) return 0;
    return tasks[id].switches;
}

// ── rtos_sleep_ticks ───────────────────────────────────────────────────────
// Block the calling task for at least `ticks` timer periods.
// Sets state to BLOCKED (so pick_next_task skips us), records the wake tick,
// then calls rtos_yield to immediately hand off the CPU.  tick_count_update
// (called on every timer interrupt) will mark us READY again when the time
// comes; rtos_yield's IRET will then re-enable the timer on return.

void rtos_sleep_ticks(uint32_t ticks) {
    if (!g_current_tcb) return;  // called before rtos_start() — no-op
    rtos_mutex_lock();                            // disable timer atomically
    g_current_tcb->state     = TASK_BLOCKED;
    g_current_tcb->wait_type = WAIT_TICK;
    g_current_tcb->wake_tick = g_tick_count + ticks;
    // rtos_yield (assembly) disables timer again (idempotent), then re-enables
    // it via IRET — so we must NOT call rtos_mutex_unlock() here.
    rtos_yield();
    // Execution resumes here after wake_tick, with timer re-enabled by IRET.
}

// ── Internal: wake the first task blocked on a given sync object ───────────
// Returns 1 if a waiter was found and woken, 0 if none.
// Caller must hold rtos_mutex_lock() across this call.

static int wake_one(void *obj, WaitType type) {
    for (int i = 0; i < n_tasks; i++) {
        if (tasks[i].state     == TASK_BLOCKED &&
            tasks[i].wait_type == type         &&
            tasks[i].wait_obj  == obj) {
            tasks[i].state     = TASK_READY;
            tasks[i].wait_type = WAIT_NONE;
            tasks[i].wait_obj  = NULL;
            return 1;
        }
    }
    return 0;
}

// ── Semaphore ──────────────────────────────────────────────────────────────

void rtos_sem_init(RtosSem *s, int count) {
    s->count = count;
}

// P operation: take one unit.  If count is already 0 block until a signal
// hands the unit directly to us (signal does NOT increment count in that case).
void rtos_sem_wait(RtosSem *s) {
    if (!g_current_tcb) return;
    rtos_mutex_lock();
    if (s->count > 0) {
        s->count--;
        rtos_mutex_unlock();
        return;
    }
    // Count is 0 — block until rtos_sem_signal hands the unit to us.
    g_current_tcb->state     = TASK_BLOCKED;
    g_current_tcb->wait_type = WAIT_SEM;
    g_current_tcb->wait_obj  = s;
    rtos_yield();   // IRET re-enables timer; sem_signal already set us READY
    // On return the unit has been transferred; no decrement needed.
}

// V operation: wake one waiter (hand-off, no count change), or increment.
void rtos_sem_signal(RtosSem *s) {
    rtos_mutex_lock();
    if (!wake_one(s, WAIT_SEM))
        s->count++;     // no waiter — store for the next sem_wait
    rtos_mutex_unlock();
}

// ── Event flag ──────────────────────────────────────────────────────────────

void rtos_flag_init(RtosFlag *f) {
    f->bits = 0;
}

// Set: wake one waiting task (hand-off), or mark pending if none.
void rtos_flag_set(RtosFlag *f) {
    rtos_mutex_lock();
    if (!wake_one(f, WAIT_FLAG))
        f->bits = 1;    // no waiter — store as pending for next flag_wait
    rtos_mutex_unlock();
}

// Wait: take pending flag immediately, or block until signalled.
void rtos_flag_wait(RtosFlag *f) {
    if (!g_current_tcb) return;
    rtos_mutex_lock();
    if (f->bits) {
        f->bits = 0;    // consume the pending flag
        rtos_mutex_unlock();
        return;
    }
    g_current_tcb->state     = TASK_BLOCKED;
    g_current_tcb->wait_type = WAIT_FLAG;
    g_current_tcb->wait_obj  = f;
    rtos_yield();   // IRET re-enables timer; flag_set already set us READY
    // On return the flag was handed off directly; bits stay 0.
}

// ── Idle canary check (call from a low-priority task if needed) ────────────

void rtos_check_stacks(void) { check_canaries(); }

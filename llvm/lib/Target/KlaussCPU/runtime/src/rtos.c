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

TCB *g_current_tcb;

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

// ── pick_next_task ─────────────────────────────────────────────────────────
// Called from timer_handler (assembly) while running on the kernel stack.
// Marks current task READY; selects next READY task round-robin; marks it
// RUNNING and updates g_current_tcb.

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

// ── Idle canary check (call from a low-priority task if needed) ────────────

void rtos_check_stacks(void) { check_canaries(); }

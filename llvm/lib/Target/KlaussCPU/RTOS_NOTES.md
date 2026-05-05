# KlaussCPU RTOS Implementation Notes

Work-in-progress notes for adding preemptive multithreading support.
Parked 2026-05-05 — resume after MMIO / opcode cleanup.

---

## What the hardware already has

| Feature | Status | Opcode |
|---|---|---|
| IRET (interrupt return) | ✅ | `0x00006011` |
| Hardware timer | TBD — confirm with CPU docs |  |
| Interrupt enable / disable | TBD |  |

IRET atomically restores the interrupted PC (and any saved status) and returns
execution to the interrupted code.  It is the hardest piece to implement in
software and we have it for free.

---

## What the hardware still needs

### 1. Interrupt enable / disable (highest priority)

Two instructions — enable and disable all interrupts atomically from C:

```c
static inline void irq_disable(void) { __builtin_klausscpu_irq_disable(); }
static inline void irq_enable(void)  { __builtin_klausscpu_irq_enable();  }
```

Without these, the only way to protect shared data is to poll, which makes
a cooperative scheduler but not a preemptive one.

### 2. Atomic compare-and-swap OR load-linked/store-conditional (nice to have)

Needed for lock-free scheduler queues when multiple interrupt levels exist.
On a single-core CPU with a simple two-level (main + one IRQ priority) design,
irq_disable/enable can substitute for CAS.  Add CAS when a second IRQ priority
level is needed.

---

## Software architecture

```
┌──────────────────────────────────────────────────────┐
│                   User tasks                          │
│            task1()  task2()  task3()                  │
├──────────────────────────────────────────────────────┤
│                   RTOS Kernel (C)                     │
│  Scheduler      pick_next_task()                      │
│  Task API       task_create / suspend / resume        │
│  Sync           mutex / semaphore / queue             │
│  Timer          tick_count, delay, periodic           │
├──────────────────────────────────────────────────────┤
│             Context Switch (assembly)                 │
│   Save R4–R7, R15, SP, PC of current task            │
│   Load R4–R7, R15, SP, PC of next task               │
├──────────────────────────────────────────────────────┤
│             Hardware                                  │
│   IRET ✅  │  Timer IRQ ?  │  IRQ mask ?             │
└──────────────────────────────────────────────────────┘
```

---

## Task Control Block (TCB)

```c
typedef struct tcb {
    // Saved register state — matches context_switch.S layout
    uint64_t r4, r5, r6, r7;   // callee-saved GPRs
    uint64_t r15;               // frame pointer
    uint64_t sp;                // stack pointer at switch point
    uint64_t pc;                // resume address
    // Task metadata
    uint8_t  state;             // READY / RUNNING / BLOCKED / DEAD
    uint8_t  priority;
    uint16_t tick_delay;        // ticks remaining in delay
    char     name[16];
    uint64_t *stack_base;       // for overflow detection
    uint64_t  stack_size;
    struct tcb *next;           // ready-queue linked list
} TCB;
```

---

## Context switch (assembly skeleton)

```asm
; void context_switch(TCB *from, TCB *to)
; r0 = from TCB, r1 = to TCB
context_switch:
    ; --- save current task ---
    stidx64  r4,  r0, 0
    stidx64  r5,  r0, 8
    stidx64  r6,  r0, 16
    stidx64  r7,  r0, 24
    stidx64  r15, r0, 32
    getsp    r12
    stidx64  r12, r0, 40
    ; PC saved by the caller's return address at [SP+0] — store it:
    ldidx64  r12, r15, 8     ; return addr = [incoming_SP+0] = [R15+8]
    stidx64  r12, r0, 48

    ; --- restore next task ---
    ldidx64  r4,  r1, 0
    ldidx64  r5,  r1, 8
    ldidx64  r6,  r1, 16
    ldidx64  r7,  r1, 24
    ldidx64  r15, r1, 32
    ldidx64  r14, r1, 40     ; next task's SP
    ldidx64  r13, r1, 48     ; next task's PC
    setsp    r14
    jmpr     r13             ; resume next task
```

---

## Timer interrupt handler skeleton

```c
// Called by hardware timer interrupt
void __attribute__((interrupt)) timer_isr(void) {
    tick_count++;

    // Unblock any tasks whose delay has expired
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == BLOCKED && tasks[i].tick_delay > 0) {
            if (--tasks[i].tick_delay == 0)
                tasks[i].state = READY;
        }
    }

    // Preempt if a higher-priority task is ready
    TCB *next = scheduler_pick_next();
    if (next != current_task) {
        TCB *old = current_task;
        current_task = next;
        context_switch(old, next);  // returns in next task's context
    }

    iret();  // __builtin_klausscpu_iret() — restores interrupted PC
}
```

---

## setjmp / longjmp

Needed by picolibc internally and for C++ exception stubs.
Implementation lives in `runtime/setjmp.S`.  See that file.

jmp_buf layout (7 × 8 bytes = 56 bytes):
```
offset  0: R4
offset  8: R5
offset 16: R6
offset 24: R7
offset 32: caller's R15 (frame pointer)
offset 40: caller's SP
offset 48: return address
```

---

## Minimum new CPU opcodes needed

| Instruction | Why |
|---|---|
| `irq_disable` | enter critical section (mask all interrupts) |
| `irq_enable`  | exit critical section (unmask) |
| CAS / LL-SC   | lock-free data structures (can defer if single IRQ priority) |

IRET is already present.  The two IRQ masking instructions are the only hard
requirement for a basic preemptive RTOS.

---

## Suggested RTOS to port

**FreeRTOS** (https://www.freertos.org) — the port requires:
- `portSAVE_CONTEXT` / `portRESTORE_CONTEXT` macros (asm)
- `xPortStartScheduler()` — sets up the timer IRQ
- `vPortYield()` — software-triggered context switch
- Roughly 200 lines of KlaussCPU-specific code total

The generic FreeRTOS kernel (~5000 lines of C) needs no changes.

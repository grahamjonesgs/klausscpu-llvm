// context_switch.S — KlaussCPU preemptive RTOS context switch.
//
// timer_handler
//   Fires on every timer tick (installed as INT_VEC0).
//   Hardware has already pushed the 64-bit context slot {INT_MASK,flags,PC}
//   onto the current task's stack and cleared INT_MASK[0].
//
//   Sequence:
//     1. Push R0–R15 onto the current (interrupted) task's stack.
//     2. Save SP → g_current_tcb->sp.
//     3. Switch to the dedicated kernel stack.
//     4. Call pick_next_task() (C, round-robin scheduler).
//     5. Load new g_current_tcb->sp.
//     6. Pop R15–R0 from the new task's stack.
//     7. IRET — restores PC, flags, INT_MASK from the new task's context slot.
//
// rtos_first_task
//   Called once from rtos_start() (C) after pick_next_task() has selected
//   the first task.  Switches SP, restores the fake register frame, enables
//   the timer interrupt, and IRETs into the task entry function.
//
// Register conventions:
//   R0–R3   : args / caller-saved scratch
//   R4–R7   : callee-saved (preserved across pick_next_task)
//   R12     : return value
//   R13–R14 : caller-saved scratch (clobbered in rtos_first_task before IRET)
//   R15     : frame pointer (callee-saved)
//   SP      : 32-bit stack pointer (GETSP/SETSP)

    .text

// ── timer_handler ──────────────────────────────────────────────────────────

    .globl  timer_handler
    .type   timer_handler, @function
timer_handler:
    // ① Save all GP registers on the current task's stack.
    push    r0
    push    r1
    push    r2
    push    r3
    push    r4
    push    r5
    push    r6
    push    r7
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15
    // SP now points to saved R15 (lowest of the 16 saved words).

    // ② Save current SP → g_current_tcb->sp (if g_current_tcb != NULL).
    getsp   r0                      // r0 = current SP
    setr    r1, g_current_tcb       // r1 = address of the g_current_tcb variable
    ldidx32 r1, r1, 0              // r1 = *g_current_tcb (the TCB pointer, 32-bit)
    cmprv   r1, 0
    jmpe    .Lskip_save
    stidx32 r0, r1, 0              // TCB->sp = r0  (sp field is at offset 0)
.Lskip_save:

    // ③ Switch to the kernel stack so pick_next_task() never touches a task stack.
    setr    r0, g_kernel_stack_top  // r0 = address of the g_kernel_stack_top variable
    ldidx32 r0, r0, 0              // r0 = actual kernel stack top value
    setsp   r0

    // ④ Increment tick counter and wake any sleeping tasks, then schedule.
    call    tick_count_update       // g_tick_count++; wake sleepers
    call    pick_next_task          // updates g_current_tcb; uses kernel stack

    // ⑤ Load the new task's saved SP from its TCB.
    setr    r0, g_current_tcb
    ldidx32 r0, r0, 0              // r0 = new TCB*
    ldidx32 r1, r0, 0              // r1 = new TCB->sp
    setsp   r1                     // switch to new task's stack

    // ⑥ Restore new task's GP registers.
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     r7
    pop     r6
    pop     r5
    pop     r4
    pop     r3
    pop     r2
    pop     r1
    pop     r0

    // ⑦ Return from interrupt: pops {INT_MASK,flags,PC} from the task's stack.
    iret
    .size timer_handler, . - timer_handler


// ── rtos_first_task ────────────────────────────────────────────────────────
// Called from C (rtos_start) after pick_next_task() has set g_current_tcb.
// Never returns.
//
// We can clobber R13 and R14 after the pops because:
//   • The first task is starting fresh; it has no expectation about
//     caller-saved register values before its own prologue runs.
//   • We need two scratch registers to enable the timer MMIO write.

    .globl  rtos_first_task
    .type   rtos_first_task, @function
rtos_first_task:
    // Load first task's saved SP from its TCB.
    setr    r14, g_current_tcb
    ldidx32 r14, r14, 0            // r14 = TCB*
    ldidx32 r14, r14, 0            // r14 = TCB->sp
    setsp   r14                    // switch to first task's stack

    // Restore the 16 fake zero registers that task_create() pushed.
    pop     r15
    pop     r14                    // r14 = 0 after this
    pop     r13                    // r13 = 0 after this
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     r7
    pop     r6
    pop     r5
    pop     r4
    pop     r3
    pop     r2
    pop     r1
    pop     r0

    // Enable the timer interrupt now that we are fully on the task's stack.
    // REG_INT_MASK (0xF00F_0000) = 1  →  unmask source 0 (timer).
    // Clobbers R13 and R14 — safe because the task starts fresh.
    setr    r13, 0xF00F0000        // address of INT_MASK register
    setr    r14, 1                 // enable timer source 0
    memset32 r14, r13              // *INT_MASK = 1

    // IRET into the first task: pops {INT_MASK,flags,PC} from task's stack.
    // The fake context slot (built by task_create) has PC = task entry,
    // flags = 0, and INT_MASK[0] = 1 so the timer stays enabled.
    iret
    .size rtos_first_task, . - rtos_first_task


// ── rtos_yield ─────────────────────────────────────────────────────────────
// Voluntarily gives up the CPU to the next READY task.  Called as a normal
// C function (via CALL); returns normally to the caller when rescheduled.
//
// Stack layout on entry (CALL has already decremented SP and stored the
// return address as an 8-byte slot: bits[31:0]=PC, bits[63:32]=0):
//
//   [SP+0..7]  : return address from CALL  ← SP on entry
//
// After pushing R0–R15 (128 bytes) SP decrements to SP_entry-128, giving:
//
//   [SP+0]     : saved R15
//   [SP+8]     : saved R14
//   ...
//   [SP+120]   : saved R0
//   [SP+128..131] : return PC  (lower 32 bits of CALL slot)
//   [SP+132..135] : 0          ← we patch this to 0x80 (INT_MASK bit 39)
//
// After patching, [SP+128..135] is a valid IRET context, identical in format
// to the one timer_handler saves.  From that point the code is shared.

    .globl  rtos_yield
    .type   rtos_yield, @function
rtos_yield:
    // ① Save R0 and R1 first so we can use them as scratch to disable timer.
    push    r0
    push    r1
    setr    r0, 0xF00F0000           // REG_INT_MASK address
    setr    r1, 0
    memset32 r1, r0                  // disable timer (prevent re-entry)

    // ② Save remaining registers.
    push    r2
    push    r3
    push    r4
    push    r5
    push    r6
    push    r7
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15
    // SP now points to saved R15, same layout as timer_handler post-push.

    // ③ Patch the CALL return-address slot into an IRET context.
    //    CALL stored [PC, 0] (8 bytes LE).  Set upper-32 to 0x80 so that
    //    IRET restores INT_MASK[0]=1 (re-enables timer) when it returns.
    getsp   r0                       // r0 = current SP (→ saved R15)
    setr    r1, 0x80                 // bit 7 of upper-32 = bit 39 of 64-bit = INT_MASK[0]
    stidx32 r1, r0, 132              // [SP+132] = 0x80

    // ④ Save current SP → g_current_tcb->sp.
    setr    r1, g_current_tcb
    ldidx32 r1, r1, 0               // r1 = *g_current_tcb
    cmprv   r1, 0
    jmpe    .Lyield_skip_save
    stidx32 r0, r1, 0               // TCB->sp = r0
.Lyield_skip_save:

    // ⑤ Switch to kernel stack and schedule (no tick update — not a timer tick).
    setr    r0, g_kernel_stack_top
    ldidx32 r0, r0, 0
    setsp   r0
    call    pick_next_task

    // ⑥ Load new task's SP and restore its registers.
    setr    r0, g_current_tcb
    ldidx32 r0, r0, 0
    ldidx32 r1, r0, 0
    setsp   r1
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     r7
    pop     r6
    pop     r5
    pop     r4
    pop     r3
    pop     r2
    pop     r1
    pop     r0

    // ⑦ IRET: restores PC (return addr), flags, INT_MASK from patched slot.
    //    For the yielding task this returns to the instruction after the
    //    rtos_yield() call, with the timer interrupt re-enabled.
    iret
    .size rtos_yield, . - rtos_yield

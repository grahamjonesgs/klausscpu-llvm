/*
 * port.S — KlaussCPU FreeRTOS context-switch assembly.
 *
 * Three entry points:
 *
 *   vPortTickISR      — timer interrupt handler (INT_VEC0).
 *   vPortStartFirstTask — launches the first task after xPortStartScheduler.
 *   vPortYield        — voluntary context switch (called as normal C function).
 *
 * Per-task stack frame layout after portSAVE_CONTEXT (low → high address):
 *
 *   [SP+0]   = R15   ← SP stored in pxCurrentTCB->pxTopOfStack
 *   [SP+8]   = R14
 *   ...
 *   [SP+112] = R1
 *   [SP+120] = R0
 *   [SP+128] = hw_frame { INT_MASK[42:39], flags[38:32], PC[31:0] }
 *
 * This matches timer_handler's push r0…r15 order: r0 is pushed first
 * (highest address), r15 last (lowest address = saved SP).
 *
 * pxCurrentTCB is a TCB_t* (64-bit pointer in FreeRTOS's tasks.c).
 * TCB_t::pxTopOfStack is a StackType_t* (also 64-bit) at offset 0.
 * We use ldidx64/stidx64 for these 8-byte fields throughout.
 *
 * REG_INT_MASK = MMIO at 0xF00F_0000.
 * Write 0 = disable all interrupts; write 1 = enable timer source 0.
 */

#define INT_MASK_ADDR   0xF00F0000

    .text

/* ── portSAVE_CONTEXT ─────────────────────────────────────────────────────
 *
 * Push R0–R15 onto the current (interrupted) task's stack, then save the
 * resulting SP into pxCurrentTCB->pxTopOfStack.
 *
 * On entry: hardware has already pushed the 64-bit IRET frame.
 * On exit:  R0 = saved SP (for kernel-stack switch in the caller).
 *           R1 clobbered.
 */
.macro portSAVE_CONTEXT
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
    /* SP now points to the R15 slot. Save it to pxCurrentTCB->pxTopOfStack. */
    getsp   r0
    setr    r1, pxCurrentTCB        /* r1 = &pxCurrentTCB (32-bit MMIO addr) */
    ldidx64 r1, r1, 0              /* r1 = pxCurrentTCB (64-bit TCB pointer) */
    stidx64 r0, r1, 0              /* pxCurrentTCB->pxTopOfStack = SP (8 bytes) */
.endm

/* ── portRESTORE_CONTEXT ──────────────────────────────────────────────────
 *
 * Load SP from pxCurrentTCB->pxTopOfStack (updated by vTaskSwitchContext),
 * then pop R15–R0 from the new task's stack.
 * The caller issues IRET immediately after this macro.
 *
 * On entry: may be called from any stack (kernel stack is typical).
 * On exit:  SP points to the new task's hw_frame; R0–R15 fully restored.
 */
.macro portRESTORE_CONTEXT
    setr    r0, pxCurrentTCB
    ldidx64 r0, r0, 0              /* r0 = pxCurrentTCB (64-bit) */
    ldidx64 r1, r0, 0              /* r1 = pxTopOfStack (64-bit, addr in lo32) */
    setsp   r1                     /* SP = new task's saved stack pointer */
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
.endm


/* ── vPortTickISR ─────────────────────────────────────────────────────────
 *
 * Installed as INT_VEC0 (timer source 0).
 * Hardware has pushed the IRET frame and cleared INT_MASK[0] before entry.
 *
 * Sequence:
 *   1. Save R0–R15; save SP → current TCB.
 *   2. Switch to dedicated kernel stack.
 *   3. Call xTaskIncrementTick() → pdTRUE if context switch is needed.
 *   4. If switch needed, call vTaskSwitchContext() to update pxCurrentTCB.
 *   5. Restore R15–R0 from the (possibly new) task's stack.
 *   6. IRET: restores PC, flags, INT_MASK from the task's hw_frame.
 */
    .globl  vPortTickISR
    .type   vPortTickISR, @function
vPortTickISR:
    portSAVE_CONTEXT
    /* r0 = saved SP (from the macro); switch to kernel stack. */
    setr    r0, g_freertos_kernel_sp
    ldidx32 r0, r0, 0              /* kernel stack top (32-bit value) */
    setsp   r0

    /* xTaskIncrementTick: increment counter, unblock delayed tasks.
       Returns pdTRUE if a higher-priority task has been unblocked. */
    call    xTaskIncrementTick
    cmprv   r12, 0
    jmpe    .Ltick_no_switch
    call    vTaskSwitchContext      /* updates pxCurrentTCB */
.Ltick_no_switch:
    portRESTORE_CONTEXT
    iret
    .size vPortTickISR, . - vPortTickISR


/* ── vPortStartFirstTask ──────────────────────────────────────────────────
 *
 * Called once from xPortStartScheduler() after FreeRTOS's scheduler
 * internals have set pxCurrentTCB to the first task to run.
 * Never returns.
 *
 * Restores the task's fake register frame (all zeros except R0=pvParameters)
 * then enables the timer and IRETs into the task's entry function.
 *
 * Clobbers R13 and R14 after the pops (safe: first task has no caller
 * context to preserve; the timer-enable write happens before IRET).
 */
    .globl  vPortStartFirstTask
    .type   vPortStartFirstTask, @function
vPortStartFirstTask:
    portRESTORE_CONTEXT
    /* Enable timer interrupt now that we are on the task's stack. */
    setr    r13, INT_MASK_ADDR
    setr    r14, 1
    memset32 r14, r13
    iret
    .size vPortStartFirstTask, . - vPortStartFirstTask


/* ── vPortYield ───────────────────────────────────────────────────────────
 *
 * Voluntary context switch called from a task as a normal C function.
 * The CALL instruction has already decremented SP by 8 and written the
 * 64-bit return-address slot:
 *   [original_SP - 8..original_SP - 5]  = return PC  (bits[31:0])
 *   [original_SP - 4..original_SP - 1]  = 0          (bits[63:32])
 *
 * After pushing R0–R15 (128 bytes total), SP is at (CALL_SP - 128) and the
 * return-address slot sits at [SP+128].  We patch [SP+132] = 0x80, which
 * sets bit 39 of the 64-bit IRET frame (= INT_MASK[0] = 1), so IRET
 * re-enables the timer when this task resumes.
 *
 * Stack layout after all 16 pushes (low → high):
 *   [SP+0]   = R15
 *   ...
 *   [SP+120] = R0
 *   [SP+128] = return PC  (lower 32 bits of CALL slot — do not touch)
 *   [SP+132] = 0x80       (patched upper 32 bits: INT_MASK[0]=1, flags=0)
 */
    .globl  vPortYield
    .type   vPortYield, @function
vPortYield:
    /* Save R0 and R1 first so we can use them as scratch to disable the
       timer before completing the full register save.  Without this, a
       timer interrupt firing mid-save could corrupt the partially-built
       frame. */
    push    r0
    push    r1
    setr    r0, INT_MASK_ADDR
    setr    r1, 0
    memset32 r1, r0                 /* disable timer (REG_INT_MASK = 0) */

    /* Save remaining registers. */
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
    /* SP now points to the R15 slot.  CALL slot is at [SP+128]. */

    /* Patch the CALL return-address slot into a valid IRET frame.
       CALL slot: [SP+128..131]=PC, [SP+132..135]=0.
       Set [SP+132] = 0x80  →  bit7 of upper-32  =  bit39 of 64-bit
                              =  INT_MASK[0] = 1.
       When this task's IRET fires, it restores the timer enable. */
    getsp   r0
    setr    r1, 0x80
    stidx32 r1, r0, 132

    /* Save current SP → pxCurrentTCB->pxTopOfStack. */
    setr    r1, pxCurrentTCB
    ldidx64 r1, r1, 0              /* r1 = pxCurrentTCB */
    stidx64 r0, r1, 0              /* pxCurrentTCB->pxTopOfStack = SP */

    /* Switch to kernel stack and select next task. */
    setr    r0, g_freertos_kernel_sp
    ldidx32 r0, r0, 0
    setsp   r0
    call    vTaskSwitchContext      /* updates pxCurrentTCB */

    portRESTORE_CONTEXT
    iret
    .size vPortYield, . - vPortYield

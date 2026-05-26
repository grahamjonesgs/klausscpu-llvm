/*
 * swap.S — KlaussCPU Zephyr context switch and timer ISR.
 *
 * arch_switch(void *switch_to, void **switched_from)
 * ─────────────────────────────────────────────────
 *   R0 = switch_to      : new thread's saved SP (void*)
 *   R1 = switched_from  : &current_thread->switch_handle (void**)
 *
 * Saves full R0-R15 frame + patches CALL return slot into an IRET frame
 * (same trick as FreeRTOS vPortYield), stores SP → *switched_from, loads
 * SP from switch_to, restores R0-R15, IRETs into the new thread.
 *
 * z_klausscpu_timer_isr
 * ─────────────────────
 * Installed as INT_VEC0.  Hardware auto-clears INT_MASK[0] on entry.
 * Saves context, switches to kernel stack, calls sys_clock_isr(),
 * restores context (possibly a different thread), IRETs.
 *
 * Per-task stack frame layout (low → high address after portSAVE_CONTEXT):
 *
 *   [SP+  0] R15
 *   [SP+  8] R14
 *   ...
 *   [SP+112] R1
 *   [SP+120] R0
 *   [SP+128] IRET frame (8 bytes): flags/INT_MASK/PC
 */

#define INT_MASK_ADDR   0xF00F0000

    .text

/* ── portSAVE_CONTEXT ───────────────────────────────────────────────────── */
/* Saves R0-R15 onto the current stack.  Used by the timer ISR path.         */
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
.endm

/* ── portRESTORE_CONTEXT ────────────────────────────────────────────────── */
/* Restores R15-R0 from the current stack (caller issues IRET after).        */
.macro portRESTORE_CONTEXT
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


/* ── arch_switch ─────────────────────────────────────────────────────────── */
/*
 * Called as a normal C function (CALL pre-decrements SP by 8 and writes the
 * return address).  We convert that CALL slot into a valid IRET frame by
 * patching [SP+132] = 0x80 (sets INT_MASK[0]=1 in the upper 32-bit word).
 *
 * After pushing R0-R15 (128 bytes), stack looks like:
 *   [SP+  0] R15 slot     ← new SP stored to *switched_from
 *   ...
 *   [SP+120] R0  slot
 *   [SP+128] return PC  (lower 32 bits — already written by CALL)
 *   [SP+132] 0x80       ← we patch this: INT_MASK[0]=1, flags=0
 */
/*
 * __start — Zephyr kernel entry point.
 *
 * On reset, SP is already valid (set by hardware or boot stub).
 * We clear BSS, copy .data from ROM, then call z_cstart().
 *
 * Placed in .text._start so the linker script puts it at address 0x20
 * (the first instruction the hardware executes after loading the ELF).
 */
    .section .text._start, "ax"
    .globl  __start
    .type   __start, @function
__start:
    /* Disable all interrupts immediately.  On a soft reset (FPGA reprogrammed
       without a full power cycle) INT_MASK may already be 1 from the previous
       run.  If a timer IRQ fires before _klausscpu_kernel_sp is initialized
       the ISR does setsp(0) → crash.  Write 0 here unconditionally. */
    setr    r0, INT_MASK_ADDR
    setr    r1, 0
    memset32 r1, r0

    /* Stack is set up by hardware reset (SP = end of RAM). */

    /* Startup delay (~150 ms at 100 MHz): the serial loader prints
       "Load Complete OK\r\n" on the same UART we are about to drive, and
       its TX FIFO may not have fully drained when control transfers to us.
       Spin so the loader's trailing bytes don't collide with Zephyr's
       first prints on the host terminal. */
    setr    r0, 0
    setr    r1, 5000000
.Lstartup_delay:
    addi    r0, r0, 1
    cmprr   r0, r1
    jmpne   .Lstartup_delay

    /* Clear BSS: from __bss_start to __bss_end. */
    setr    r0, __bss_start
    setr    r1, __bss_end
    setr    r2, 0
.Lbss_loop:
    cmprr   r0, r1
    jmpuge  .Lbss_done
    stidx64 r2, r0, 0
    addi    r0, r0, 8
    jmp     .Lbss_loop
.Lbss_done:

    /*
     * Copy .data from LMA (in ROM segment) to VMA (in RAM).
     *
     * With CONFIG_XIP=n (the default for this port) the linker places LMA == VMA,
     * so the src==dst check below short-circuits this loop to a no-op.  It only
     * does real work if a future build re-enables XIP — and in that case klausscc
     * must also be taught to handle PT_LOAD segments with VirtAddr != PhysAddr.
     */
    setr    r0, __data_start
    setr    r1, __data_end
    setr    r2, __data_load_start
    /* skip if src == dst (XIP or same-region build) */
    cmprr   r0, r2
    jmpe    .Ldata_done
    /* also skip if section is empty */
    cmprr   r0, r1
    jmpuge  .Ldata_done
.Ldata_loop:
    cmprr   r0, r1
    jmpuge  .Ldata_done
    ldidx64 r3, r2, 0
    stidx64 r3, r0, 0
    addi    r0, r0, 8
    addi    r2, r2, 8
    jmp     .Ldata_loop
.Ldata_done:

    /* Copy device_states, k_heap_area and related sections (second ROM→RAM segment).
     * LMA is contiguous with .datas in ROM; VMA is at __device_states_start. */
    setr    r0, __device_states_start
    setr    r1, __kernel_initialized_end
    setr    r2, __device_states_load_start
    cmprr   r0, r2
    jmpe    .Ldevdata_done          /* skip if LMA == VMA */
    cmprr   r0, r1
    jmpuge  .Ldevdata_done          /* skip if empty */
.Ldevdata_loop:
    cmprr   r0, r1
    jmpuge  .Ldevdata_done
    ldidx64 r3, r2, 0
    stidx64 r3, r0, 0
    addi    r0, r0, 8
    addi    r2, r2, 8
    jmp     .Ldevdata_loop
.Ldevdata_done:

    call    z_cstart
    /* z_cstart() should not return; halt if it does */
.Lhalt:
    jmp     .Lhalt
    .size   __start, . - __start

    .text   /* back to .text for arch_switch and timer ISR */

    .globl  arch_switch
    .type   arch_switch, @function
arch_switch:
    /* Save R0/R1 first so we can disable the timer safely. */
    push    r0
    push    r1
    /* Disable timer before completing the register save so a timer IRQ
       cannot corrupt the half-built frame. */
    setr    r0, INT_MASK_ADDR
    setr    r1, 0
    memset32 r1, r0

    /*
     * ISR-context fast path.
     *
     * When called from within the timer ISR (on the kernel stack),
     * z_klausscpu_timer_isr already saved the task stack SP to
     * current->switch_handle.  Saving the kernel stack SP there would
     * corrupt that value and cause the next ISR to use SP≈0.
     *
     * Detect this case via _klausscpu_in_isr.  If set: clear the flag,
     * grab switch_to from the R0 save slot, and jump directly to the
     * new thread without touching switched_from.
     */
    setr    r0, _klausscpu_in_isr
    ldidx32 r0, r0, 0
    cmprv   r0, 0
    jmpne   .Larch_switch_from_isr

    /* ── Normal thread-context switch ────────────────────────────── */
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

    /* Patch the CALL return-address slot into an IRET frame.
       [SP+128..131] = return PC (already correct — do not touch).
       [SP+132..135] = 0x80  →  bit7 of upper-32 = bit39 of 64-bit = INT_MASK[0]=1. */
    getsp   r2
    setr    r3, 0x80
    stidx32 r3, r2, 132

    /* Retrieve the arch_switch arguments from the original R0/R1 slots on
       the stack (they were pushed first, so they sit at [SP+120] and [SP+112]).
       R0_saved = switch_to      (new SP)
       R1_saved = switched_from  (&current switch_handle slot)           */
    ldidx64 r3, r2, 120   /* r3 = original R0 = switch_to */
    ldidx64 r4, r2, 112   /* r4 = original R1 = switched_from */

    /* Store current SP → *switched_from. */
    stidx64 r2, r4, 0

    /* Load new SP from switch_to (r3 holds the void* value). */
    setsp   r3

    portRESTORE_CONTEXT
    iret

    /* ── ISR-context switch ───────────────────────────────────────── */
    /*
     * The timer ISR already saved the preempted task's complete context
     * (R0-R15 + IRET frame) on the task stack and stored that task SP in
     * current->switch_handle.  We must NOT overwrite it with the kernel SP.
     *
     * Stack layout here (only R0 and R1 pushed so far):
     *   [SP+0]  R1  (= switched_from, not needed — ISR save is authoritative)
     *   [SP+8]  R0  (= switch_to, the new thread's saved SP)
     *   [SP+16] return address (CALL slot)
     *
     * Clear _klausscpu_in_isr so the new thread runs with the flag off,
     * then switch directly to the new thread's task context.
     */
.Larch_switch_from_isr:
    setr    r0, _klausscpu_in_isr
    setr    r1, 0
    stidx32 r1, r0, 0          /* _klausscpu_in_isr = 0 */

    getsp   r14
    ldidx64 r3, r14, 8         /* r3 = switch_to (original R0) */

    setsp   r3
    portRESTORE_CONTEXT
    iret
    .size arch_switch, . - arch_switch


/* ── z_klausscpu_timer_isr ──────────────────────────────────────────────── */
/*
 * CONFIG_USE_SWITCH=y timer ISR.
 *
 * Hardware has already: decremented SP by 8, written the 8-byte IRET frame
 * (PC + INT_MASK/flags), cleared INT_MASK[0].
 *
 * Design: call sys_clock_isr() on the kernel stack.  If sys_clock_announce()
 * decides a higher-priority thread is ready it calls z_reschedule() which
 * calls arch_switch() — the ISR fast path in arch_switch restores the new
 * thread and IRETs directly, never returning here.
 *
 * If sys_clock_isr() returns normally (no reschedule, or the interrupted
 * thread is still highest priority), we simply restore the interrupted
 * thread.  We do NOT call z_get_next_switch_handle() — that function reads
 * _kernel.ready_q.cache which is NULL during early-boot PRE_KERNEL_2
 * (threads not yet created), causing it to dereference address 0 and return
 * garbage as the switch handle → instant crash.
 */
    .globl  z_klausscpu_timer_isr
    .type   z_klausscpu_timer_isr, @function
z_klausscpu_timer_isr:
    portSAVE_CONTEXT

    /* Record "in ISR" so arch_is_in_isr() and arch_switch ISR fast path work. */
    setr    r0, _klausscpu_in_isr
    setr    r1, 1
    stidx32 r1, r0, 0

    /* Save interrupted task SP → current->switch_handle AND _klausscpu_isr_handle.
       ___cpu_t_current_OFFSET          = 0x10  (within struct _cpu)
       ___kernel_t_cpus_OFFSET          = 0x00
       ___thread_t_switch_handle_OFFSET = 0x68 */
    getsp   r0
    setr    r1, _kernel
    ldidx64 r1, r1, 0x10       /* r1 = _kernel.cpus[0].current (k_thread *) */
    stidx64 r0, r1, 0x68       /* current->switch_handle = saved task SP */
    setr    r1, _klausscpu_isr_handle
    stidx64 r0, r1, 0          /* _klausscpu_isr_handle = saved task SP */

    /* Switch to dedicated kernel stack. */
    setr    r0, _klausscpu_kernel_sp
    ldidx32 r0, r0, 0
    setsp   r0

    /* Announce the tick.  If a context switch is needed, sys_clock_announce()
       eventually calls arch_switch() which uses the ISR fast path — it clears
       _klausscpu_in_isr, loads the new thread's SP, and IRETs directly.
       In that case the call below never returns. */
    call    sys_clock_isr

    /* sys_clock_isr() returned: no switch was triggered.
       Clear _klausscpu_in_isr and restore the interrupted thread. */
    setr    r0, _klausscpu_in_isr
    setr    r1, 0
    stidx32 r1, r0, 0

    setr    r12, _klausscpu_isr_handle
    ldidx64 r12, r12, 0        /* r12 = saved task SP */
    setsp   r12

    portRESTORE_CONTEXT
    iret
    .size z_klausscpu_timer_isr, . - z_klausscpu_timer_isr

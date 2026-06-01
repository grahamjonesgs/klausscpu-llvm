/*
 * arch/klausscpu/core/thread.c — thread stack initialisation
 *
 * Stack frame layout built by arch_new_thread() (low → high address):
 *
 *   [SP+0]   R15  (frame pointer = 0)
 *   [SP+8]   R14  = 0
 *   ...
 *   [SP+ 96] R3   = p3
 *   [SP+104] R2   = p2
 *   [SP+112] R1   = p1
 *   [SP+120] R0   = entry
 *   [SP+128] IRET frame: INT_MASK[0]=1 | PC=z_thread_entry
 *
 * On first dispatch arch_switch() restores R0-R15 then IRETs into
 * z_thread_entry(entry, p1, p2, p3).  R0-R3 are set just before the
 * IRET frame so IRET lands in z_thread_entry with R0=entry, R1=p1, etc.
 *
 * KlaussCPU IRET frame (64-bit):
 *   bits[63:43] = 0
 *   bits[42:39] = INT_MASK (bit 39 = timer source 0)
 *   bits[38:32] = processor flags
 *   bits[31: 0] = PC
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>

/*
 * Zephyr thread entry trampoline — calls entry(p1,p2,p3) then exits.
 * Defined in kernel/init.c; we just need the declaration.
 */
extern void z_thread_entry(k_thread_entry_t entry,
                           void *p1, void *p2, void *p3);

void arch_new_thread(struct k_thread *thread, k_thread_stack_t *stack,
                     char *stack_ptr, k_thread_entry_t entry,
                     void *p1, void *p2, void *p3)
{
    /* stack_ptr is the top of the usable stack (highest valid byte + 1). */
    uint64_t *fp = (uint64_t *)stack_ptr;

    /*
     * IRET frame: timer enabled (INT_MASK[0]=1) so the new thread runs
     * with interrupts on from the very first instruction.
     * PC = z_thread_entry — the trampoline that calls entry(p1,p2,p3).
     */
    *--fp = ((uint64_t)1u << 39)
          | (uint64_t)(uint32_t)(uintptr_t)z_thread_entry;

    /*
     * R0–R3: portRESTORE_CONTEXT pops r15 first (lowest addr) → r0 last
     * (highest addr = [switch_handle+120]).  To get r0=entry, r1=p1,
     * r2=p2, r3=p3 after restore, push in reverse order (entry pushed
     * first after IRET so it lands highest = r0, p3 pushed last before
     * the callee-saved zeros so it lands lowest of the four = r3).
     */
    *--fp = (uint64_t)(uintptr_t)entry; /* → [+120] → R0 */
    *--fp = (uint64_t)(uintptr_t)p1;    /* → [+112] → R1 */
    *--fp = (uint64_t)(uintptr_t)p2;    /* → [+104] → R2 */
    *--fp = (uint64_t)(uintptr_t)p3;    /* → [+ 96] → R3 */

    /* R4–R14: zero (no meaningful callee state for a new thread). */
    for (int i = 0; i < 11; i++) {
        *--fp = 0ULL;
    }

    /* R15 (frame pointer): 0 — this thread has no caller frame. */
    *--fp = 0ULL;

    /*
     * arch_switch() key: thread->switch_handle is the saved SP value
     * that arch_switch(switch_to, ...) will load into SP on first dispatch.
     */
    thread->switch_handle = fp;
}

/*
 * arch_cpu_idle — called by the idle thread when there is no work.
 * Enable interrupts, then WAIT: the core suspends until an unmasked, vectored
 * interrupt (timer tick, etc.) becomes pending, the ISR is taken via the normal
 * dispatch path, and IRET resumes at the instruction after WAIT.  The WAIT wake
 * is level-sensitive, so the enable-then-WAIT sequence has no lost-wakeup race.
 */
void arch_cpu_idle(void)
{
    __asm__ volatile(
        "setr   r0, 0xF00F0000\n\t"  /* INT_MASK */
        "setr   r1, 1\n\t"
        "memset32 r1, r0\n\t"        /* unmask the timer source */
        "wait\n\t"                   /* suspend until an interrupt wakes us */
        : : : "r0", "r1", "memory"
    );
}

void arch_cpu_atomic_idle(unsigned int key)
{
    ARG_UNUSED(key);
    arch_cpu_idle();
}

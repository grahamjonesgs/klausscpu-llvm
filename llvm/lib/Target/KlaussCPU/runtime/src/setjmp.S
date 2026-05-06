// setjmp.S — KlaussCPU implementation of setjmp / longjmp.
//
// jmp_buf layout (7 slots × 8 bytes = 56 bytes):
//   [+0]  R4   callee-saved
//   [+8]  R5   callee-saved
//   [+16] R6   callee-saved
//   [+24] R7   callee-saved
//   [+32] R15  caller's frame pointer (saved BEFORE we push our own frame)
//   [+40] SP   caller's stack pointer at point of setjmp call
//   [+48] PC   return address (saved by CALL to setjmp, lives at [SP+0])
//
// Calling convention:
//   setjmp(jmp_buf *env)   → R0=env; returns 0 (or val from longjmp)
//   longjmp(jmp_buf *env, int val) → R0=env, R1=val; never returns

    .text

// ── setjmp ────────────────────────────────────────────────────────────────────

    .globl  setjmp
    .type   setjmp, @function
setjmp:
    // At function entry, BEFORE our prologue, R15 holds the CALLER's frame
    // pointer.  Save it first so we capture the right value.
    stidx64  r15, r0, 32

    // Now set up our own frame.
    push    r15
    getsp   r15

    // Save callee-saved registers.
    stidx64  r4,  r0, 0
    stidx64  r5,  r0, 8
    stidx64  r6,  r0, 16
    stidx64  r7,  r0, 24

    // Caller's SP = our incoming_SP = R15 + 8  (R15 = incoming_SP - 8).
    addi     r12, r15, 8
    stidx64  r12, r0, 40

    // Return address lives at [incoming_SP + 0] = [R15 + 8].
    ldidx64  r12, r15, 8
    stidx64  r12, r0, 48

    // Return 0.
    setr     r12, 0
    setsp    r15
    pop      r15
    ret
    .size setjmp, . - setjmp

// ── longjmp ───────────────────────────────────────────────────────────────────

    .globl  longjmp
    .type   longjmp, @function
longjmp:
    // Restore callee-saved registers.
    ldidx64  r4,  r0, 0
    ldidx64  r5,  r0, 8
    ldidx64  r6,  r0, 16
    ldidx64  r7,  r0, 24

    // Restore caller's frame pointer.
    ldidx64  r15, r0, 32

    // Load saved SP and saved return address into temporaries.
    ldidx64  r14, r0, 40    // caller's SP
    ldidx64  r13, r0, 48    // return address

    // Return value: val, but never 0 (if caller passed 0, return 1 instead).
    copy     r12, r1
    cmprv    r12, 0
    jmpne    .Lnonzero
    setr     r12, 1
.Lnonzero:

    // Restore SP and jump directly to the saved return address.
    // Using JMPR avoids writing [SP+0] — the caller's code resumes correctly
    // because R15 and SP are already the caller's values.
    setsp    r14
    jmpr     r13
    .size longjmp, . - longjmp

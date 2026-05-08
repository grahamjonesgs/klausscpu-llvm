// sys_arch.c — lwIP timing for NO_SYS=1.
//
// sys_now() is the only OS function lwIP requires in NO_SYS=1 mode.
// It returns milliseconds since some epoch; REG_CLOCK_MS is a 64-bit
// free-running ms counter driven by the 100 MHz clock.

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "arch/sys_arch.h"

u32_t sys_now(void) {
    return (u32_t)REG_CLOCK_MS;   // low 32 bits; wraps after ~49 days
}

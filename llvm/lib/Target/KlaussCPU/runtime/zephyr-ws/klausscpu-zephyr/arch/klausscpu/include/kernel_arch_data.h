#ifndef ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KERNEL_ARCH_DATA_H_
#define ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KERNEL_ARCH_DATA_H_

#include <zephyr/types.h>
#include <zephyr/toolchain.h>

#ifndef _ASMLANGUAGE

/*
 * Callee-saved registers saved by arch_switch().
 * arch_switch() saves the full R0-R15 frame so we keep this minimal;
 * the "handle" is just the saved SP (a uint32_t stored as void*).
 */
struct _callee_saved {
    /* nothing: the whole context lives on the task stack */
};
typedef struct _callee_saved _callee_saved_t;

struct _thread_arch {
    uint32_t swap_return_value;
};
typedef struct _thread_arch _thread_arch_t;

#endif /* _ASMLANGUAGE */
#endif /* ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_KERNEL_ARCH_DATA_H_ */

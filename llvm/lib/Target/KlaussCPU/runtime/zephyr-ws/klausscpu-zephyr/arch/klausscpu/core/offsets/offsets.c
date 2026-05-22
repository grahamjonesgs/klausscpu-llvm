/*
 * offsets.c — KlaussCPU kernel struct offset definitions.
 */

#include <zephyr/kernel.h>
#include <kernel_arch_data.h>
#include <gen_offset.h>
#include <kernel_offsets.h>

/* _thread_arch_t offsets */
GEN_OFFSET_SYM(_thread_arch_t, swap_return_value);

/* arch_esf offsets */
GEN_OFFSET_STRUCT(arch_esf, r);
GEN_OFFSET_STRUCT(arch_esf, iret);
GEN_ABSOLUTE_SYM(__arch_esf_SIZEOF, sizeof(struct arch_esf));

GEN_ABS_SYM_END

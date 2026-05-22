#ifndef ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_OFFSETS_SHORT_ARCH_H_
#define ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_OFFSETS_SHORT_ARCH_H_

#include <zephyr/offsets.h>

/* swap_return_value is stored in _thread_arch_t. */
#define _thread_offset_to_swap_return_value \
    (___thread_t_arch_OFFSET + ___thread_arch_t_swap_return_value_OFFSET)

#endif /* ZEPHYR_ARCH_KLAUSSCPU_INCLUDE_OFFSETS_SHORT_ARCH_H_ */

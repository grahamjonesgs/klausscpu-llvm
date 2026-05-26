/* wolfssl_hw.h — wolfSSL hardware crypto callback for KlaussCPU (Zephyr).
 *
 * Same API as the FreeRTOS version but uses Zephyr k_mutex instead of
 * FreeRTOS semaphores for serializing access to the hardware AES block.
 */
#ifndef WOLFSSL_HW_H
#define WOLFSSL_HW_H

#ifdef __cplusplus
extern "C" {
#endif

int wolfssl_hw_init(void);
void wolfssl_hw_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* WOLFSSL_HW_H */

/* wolfssl_hw.h — wolfSSL hardware crypto callback for KlaussCPU.
 *
 * Wires AES-CTR, AES-GCM, and TRNG seeding to the MMIO crypto blocks via
 * wolfSSL's wc_CryptoCb_RegisterDevice() API.
 *
 * Usage:
 *   1. Call wolfssl_hw_init() once at startup (after wolfCrypt_Init()).
 *   2. Create wolfSSL objects with devId = KLAUSSCPU_CRYPTO_DEV_ID.
 *
 * SHA-256 and HMAC-SHA-256: routed via wolfCrypt software (the streaming
 * API doesn't allow accumulating raw bytes across multiple Update calls
 * without a full software buffer — see CRYPTO_PLAN.md §9.2 for upgrade
 * path once we add a bigger SRAM buffer or DMA).
 */
#ifndef WOLFSSL_HW_H
#define WOLFSSL_HW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the hardware crypto device with wolfCrypt.
 * Must be called after wolfCrypt_Init() and FreeRTOS scheduler started.
 * Returns 0 on success, negative on error. */
int wolfssl_hw_init(void);

/* De-register and clean up (call before wolfCrypt_Cleanup()). */
void wolfssl_hw_cleanup(void);

/* TRNG seed — declared in user_settings.h (where CUSTOM_RAND_GENERATE_SEED is defined)
 * so that wolfSSL's random.c sees it. Definition is in wolfssl_hw.c. */

#ifdef __cplusplus
}
#endif

#endif /* WOLFSSL_HW_H */

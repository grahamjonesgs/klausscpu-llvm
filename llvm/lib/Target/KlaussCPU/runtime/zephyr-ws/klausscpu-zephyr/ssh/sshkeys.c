/* sshkeys.c — SSH host key management (ECDSA P-256) for Zephyr.
 *
 * Adapted from freertos/ssh/sshkeys.c.  Uses Zephyr's fs API (FatFS)
 * instead of raw FatFS calls.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/asn_public.h>

#include "sshkeys.h"

LOG_MODULE_REGISTER(sshkeys, LOG_LEVEL_INF);

#define PRIV_DER_MAX 256
#define KEY_PATH     "/SD:/" SSH_KEY_FILENAME

static uint8_t s_priv_der[PRIV_DER_MAX];
static int s_priv_der_len;
static int s_initialized;
static int s_needs_persist;

static int generate_keypair(void)
{
	ecc_key key;
	WC_RNG rng;
	int rc;

	rc = wc_InitRng(&rng);
	if (rc != 0) {
		return rc;
	}

	rc = wc_ecc_init(&key);
	if (rc != 0) {
		wc_FreeRng(&rng);
		return rc;
	}

	rc = wc_ecc_make_key(&rng, 32, &key);
	if (rc != 0) {
		wc_ecc_free(&key);
		wc_FreeRng(&rng);
		return rc;
	}

	rc = wc_EccKeyToDer(&key, s_priv_der, (word32)sizeof(s_priv_der));
	wc_ecc_free(&key);
	wc_FreeRng(&rng);
	if (rc < 0) {
		return rc;
	}
	s_priv_der_len = rc;
	return 0;
}

static int load_from_sd(void)
{
	struct fs_file_t f;
	ssize_t br;

	fs_file_t_init(&f);

	if (fs_open(&f, KEY_PATH, FS_O_READ) != 0) {
		return -1;
	}

	br = fs_read(&f, s_priv_der, PRIV_DER_MAX);
	fs_close(&f);

	if (br < 16 || s_priv_der[0] != 0x30) {
		return -1;
	}

	s_priv_der_len = (int)br;
	return 0;
}

static int save_to_sd(void)
{
	struct fs_file_t f;
	ssize_t bw;

	fs_file_t_init(&f);

	if (fs_open(&f, KEY_PATH, FS_O_WRITE | FS_O_CREATE) != 0) {
		LOG_ERR("Cannot open %s for writing", KEY_PATH);
		return -1;
	}

	bw = fs_write(&f, s_priv_der, s_priv_der_len);
	fs_close(&f);

	if (bw != s_priv_der_len) {
		LOG_ERR("Write failed: %d/%d bytes", (int)bw, s_priv_der_len);
		return -1;
	}
	return 0;
}

int sshkeys_init(void)
{
	if (s_initialized) {
		return 0;
	}

	if (load_from_sd() == 0) {
		LOG_INF("Loaded host key from SD (%d bytes)", s_priv_der_len);
		s_initialized = 1;
		return 0;
	}

	int rc = generate_keypair();

	if (rc != 0) {
		LOG_ERR("Key generation failed: %d", rc);
		return rc;
	}
	LOG_INF("Generated new ECDSA P-256 host key (%d bytes)", s_priv_der_len);
	s_needs_persist = 1;
	s_initialized = 1;
	return 0;
}

int sshkeys_persist(void)
{
	if (!s_initialized || !s_needs_persist) {
		return 0;
	}

	if (save_to_sd() == 0) {
		LOG_INF("Host key saved to SD");
		s_needs_persist = 0;
		return 0;
	}

	LOG_WRN("Could not save host key to SD");
	return -1;
}

const uint8_t *sshkeys_get_private(void)
{
	return s_priv_der;
}

size_t sshkeys_get_private_len(void)
{
	return (size_t)s_priv_der_len;
}

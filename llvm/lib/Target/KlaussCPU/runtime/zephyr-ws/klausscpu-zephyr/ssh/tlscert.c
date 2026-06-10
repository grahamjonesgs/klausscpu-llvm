/* tlscert.c — TLS server certificate + key for KlaussCPU Zephyr.
 *
 * Generates a self-signed ECDSA P-256 certificate (SHA-256 signature) the
 * first time the board boots, persists both the private key and the cert to
 * the SD card as DER blobs, and reloads them on subsequent boots.  This is the
 * TLS analogue of sshkeys.c — SSH presents a raw host key, whereas TLS needs
 * an X.509 certificate, so the cert is the one extra artifact HTTPS requires.
 *
 * The HW crypto callback (wolfssl_hw.c) and the TRNG seed
 * (klausscpu_trng_seed) are registered globally, so the key generation and the
 * later TLS handshakes use hardware AES/SHA/ECC automatically.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <time.h>

#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/asn_public.h>

#include "tlscert.h"
#include "wallclock.h"

LOG_MODULE_REGISTER(tlscert, LOG_LEVEL_INF);

/* wolfSSL's Zephyr port (z_time, wc_port.c) calls clock_gettime(CLOCK_REALTIME)
 * to stamp/validate ASN.1 certificate dates.  The board's minimal libc has no
 * clock_gettime, and wolfSSH never referenced it — so once the TLS layer is
 * linked in (HTTPS), it is an undefined symbol at link time.  Back it with the
 * SNTP-set software wall-clock so the generated cert gets real validity dates.
 * (If the clock was never set, wallclock_now() returns 0 -> epoch 1970, which
 * is self-consistent: the server doesn't date-check its own cert, and a
 * `curl -k` client skips verification.) */
int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	ARG_UNUSED(clk_id);

	if (tp == NULL) {
		return -1;
	}
	tp->tv_sec = (time_t)wallclock_now();
	tp->tv_nsec = 0;
	return 0;
}

#define KEY_DER_MAX   256
#define CERT_DER_MAX  1024
#define KEY_PATH      "/SD:/" TLS_KEY_FILENAME
#define CERT_PATH     "/SD:/" TLS_CERT_FILENAME

static uint8_t s_key_der[KEY_DER_MAX];
static int     s_key_der_len;
static uint8_t s_cert_der[CERT_DER_MAX];
static int     s_cert_der_len;
static int     s_initialized;

/* ── DER blob persistence (same shape as sshkeys.c) ──────────────────────── */

static int load_blob(const char *path, uint8_t *buf, int cap, int min_len,
		     int *out_len)
{
	struct fs_file_t f;
	ssize_t br;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		return -1;
	}
	br = fs_read(&f, buf, cap);
	fs_close(&f);

	/* Must be a DER SEQUENCE (0x30) of at least min_len bytes.  A real
	 * P-256 key DER is ~121 B and a cert ~378 B, so a stale truncated blob
	 * (e.g. a 21-byte key from an earlier export bug) is rejected and
	 * regenerated rather than served. */
	if (br < min_len || buf[0] != 0x30) {
		return -1;
	}
	*out_len = (int)br;
	return 0;
}

static int save_blob(const char *path, const uint8_t *buf, int len)
{
	struct fs_file_t f;
	ssize_t bw;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC) != 0) {
		LOG_ERR("Cannot open %s for writing", path);
		return -1;
	}
	bw = fs_write(&f, buf, len);
	fs_close(&f);

	if (bw != len) {
		LOG_ERR("Write %s failed: %d/%d bytes", path, (int)bw, len);
		return -1;
	}
	return 0;
}

/* ── Self-signed ECDSA P-256 cert generation ─────────────────────────────── */

static int generate(void)
{
	ecc_key key;
	WC_RNG  rng;
	Cert    cert;
	int     rc;
	int     bodySz;

	rc = wc_InitRng(&rng);
	if (rc != 0) {
		return rc;
	}

	rc = wc_ecc_init(&key);
	if (rc != 0) {
		wc_FreeRng(&rng);
		return rc;
	}

	rc = wc_ecc_make_key(&rng, 32, &key);   /* 32 bytes => P-256 */
	if (rc != 0) {
		goto out;
	}

	/* Export the private key (same SEC1 DER form sshkeys.c uses).  Done
	 * before building the cert just to keep the key handling identical to
	 * sshkeys.c; the order is not significant (signing leaves the key
	 * intact). */
	rc = wc_EccKeyToDer(&key, s_key_der, (word32)sizeof(s_key_der));
	if (rc <= 0) {
		rc = (rc == 0) ? BAD_FUNC_ARG : rc;
		goto out;
	}
	s_key_der_len = rc;

	/* Build + self-sign the certificate (issuer defaults to subject when no
	 * CA is set, so passing the same key to MakeCert and SignCert yields a
	 * self-signed cert — see wolfCrypt's certecc_test). */
	rc = wc_InitCert(&cert);
	if (rc != 0) {
		goto out;
	}
	/* 397 days: Safari/Chrome reject server certs valid for more than 398
	 * days with a fatal TLS alert (and often won't let the user click
	 * through).  curl has no such limit, which is why it connects fine. */
	cert.daysValid = 397;
	cert.sigType   = CTC_SHA256wECDSA;
	cert.isCA      = 0;
	strncpy(cert.subject.country, "GB", CTC_NAME_SIZE - 1);
	strncpy(cert.subject.org, "KlaussCPU", CTC_NAME_SIZE - 1);
	strncpy(cert.subject.commonName, "klausscpu.local", CTC_NAME_SIZE - 1);

	bodySz = wc_MakeCert(&cert, s_cert_der, (word32)sizeof(s_cert_der),
			     NULL, &key, &rng);
	if (bodySz < 0) {
		rc = bodySz;
		goto out;
	}

	rc = wc_SignCert(cert.bodySz, cert.sigType, s_cert_der,
			 (word32)sizeof(s_cert_der), NULL, &key, &rng);
	if (rc <= 0) {
		rc = (rc == 0) ? BAD_FUNC_ARG : rc;
		goto out;
	}
	s_cert_der_len = rc;
	rc = 0;

out:
	wc_ecc_free(&key);
	wc_FreeRng(&rng);
	return rc;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int tlscert_init(void)
{
	if (s_initialized) {
		return 0;
	}

	/* Floors below a valid P-256 key (~121 B) / cert (~378 B) so a stale
	 * truncated blob is rejected and regenerated. */
	if (load_blob(KEY_PATH, s_key_der, KEY_DER_MAX, 64, &s_key_der_len) == 0 &&
	    load_blob(CERT_PATH, s_cert_der, CERT_DER_MAX, 100, &s_cert_der_len) == 0) {
		/* One value per LOG line: Zephyr's packaged logging mis-renders
		 * some multi-arg width packing on this target (see CLAUDE.md). */
		LOG_INF("Loaded TLS cert from SD: %d bytes", s_cert_der_len);
		LOG_INF("Loaded TLS key from SD: %d bytes", s_key_der_len);
		s_initialized = 1;
		return 0;
	}

	int rc = generate();

	if (rc != 0) {
		LOG_ERR("TLS cert generation failed: %d", rc);
		return rc;
	}
	LOG_INF("Generated self-signed ECDSA P-256 cert: %d bytes", s_cert_der_len);
	LOG_INF("Generated TLS private key: %d bytes", s_key_der_len);

	/* Best-effort persist; a failed save just means we regenerate next boot. */
	if (save_blob(KEY_PATH, s_key_der, s_key_der_len) == 0 &&
	    save_blob(CERT_PATH, s_cert_der, s_cert_der_len) == 0) {
		LOG_INF("TLS cert + key saved to SD");
	} else {
		LOG_WRN("Could not save TLS cert/key to SD (will regenerate)");
	}

	s_initialized = 1;
	return 0;
}

const uint8_t *tlscert_get_cert(void)
{
	return s_cert_der;
}

size_t tlscert_get_cert_len(void)
{
	return (size_t)s_cert_der_len;
}

const uint8_t *tlscert_get_key(void)
{
	return s_key_der;
}

size_t tlscert_get_key_len(void)
{
	return (size_t)s_key_der_len;
}

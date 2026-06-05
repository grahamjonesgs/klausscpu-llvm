/* ssh_shell_transport.c — Zephyr shell transport backend over wolfSSH.
 *
 * Replaces the hand-rolled interpreter that used to live in ssh_server.c
 * (run_shell + dispatch_command + a private copy of every cmd_*).  SSH is now
 * just another shell transport, so the single set of SHELL_CMD_REGISTER
 * commands (shell_cmds.c) is shared between the serial console and SSH, and
 * line editing / history / tab-completion / `help` all come from the core.
 *
 * One static shell instance is defined per connection slot (Zephyr shells are
 * static — cf. the upstream telnet backend, which supports only one session).
 *
 * ── Concurrency ──────────────────────────────────────────────────────────────
 * Two threads touch one WOLFSSH session, split by direction:
 *   reader  = the per-connection thread (ssh_server.c) — the ONLY caller of
 *             wolfSSH_stream_read.  It feeds bytes into rx_rb (ssh_shell_feed).
 *   writer  = the shell core thread — the ONLY caller of wolfSSH_stream_send
 *             (transport write(), guarded by io_lock).
 * Because the directions never share a reader or a writer, the only genuinely
 * shared mutable state is a mid-session KEX/rekey.  wolfSSH is configured
 * without periodic rekey for this server; if that ever changes, the reader and
 * io_lock would need to cooperate around rekey.  Input for a running `run`
 * extension is taken from the SAME rx_rb (ssh_shell_getc), so the socket is
 * never read from two threads simultaneously.
 */

#define WOLFSSL_USER_SETTINGS
#include "user_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <stdio.h>

#include <wolfssh/ssh.h>

#include "ssh_server.h"          /* SSH_MAX_CONNS */
#include "ssh_shell_transport.h"
#include "llext_loader.h"
#if defined(CONFIG_KLAUSSCPU_SHELL_CMDS)
#include "shell_paths.h"
#endif

#define RX_RING_SIZE 256

/* ── Per-slot transport context ──────────────────────────────────────────── */

struct ssh_shell_ctx {
	WOLFSSH                   *ssh;      /* bound session, NULL when idle  */
	shell_transport_handler_t  handler;  /* shell core's event callback    */
	void                      *context;
	struct ring_buf            rx_rb;
	uint8_t                    rx_buf[RX_RING_SIZE];
	struct k_sem               rx_sem;   /* wakeup for ssh_shell_getc()    */
	struct k_mutex             io_lock;  /* serialises wolfSSH_stream_send */
	atomic_t                   open;
	atomic_t                   running;  /* a `run` extension is executing */
	struct k_sem               done;     /* given when that extension ends */
};

/* ── Transport API ───────────────────────────────────────────────────────── */

static int t_init(const struct shell_transport *transport, const void *config,
		  shell_transport_handler_t evt_handler, void *context)
{
	struct ssh_shell_ctx *c = transport->ctx;

	ARG_UNUSED(config);
	c->handler = evt_handler;
	c->context = context;
	ring_buf_init(&c->rx_rb, sizeof(c->rx_buf), c->rx_buf);
	k_sem_init(&c->rx_sem, 0, 1);
	k_sem_init(&c->done, 0, 1);
	k_mutex_init(&c->io_lock);
	atomic_set(&c->open, 0);
	atomic_set(&c->running, 0);
	return 0;
}

static int t_uninit(const struct shell_transport *transport)
{
	ARG_UNUSED(transport);
	return 0;
}

static int t_enable(const struct shell_transport *transport, bool blocking)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(blocking);
	return 0;
}

static int t_write(const struct shell_transport *transport, const void *data,
		   size_t length, size_t *cnt)
{
	struct ssh_shell_ctx *c = transport->ctx;

	/* No bound session (idle, or already released): swallow output. */
	if (c->ssh == NULL || !atomic_get(&c->open)) {
		*cnt = length;
		return 0;
	}

	k_mutex_lock(&c->io_lock, K_FOREVER);
	int n = wolfSSH_stream_send(c->ssh, (uint8_t *)data, (word32)length);

	k_mutex_unlock(&c->io_lock);

	if (n < 0) {
		*cnt = 0;
		return -EIO;
	}
	*cnt = (size_t)n;
	return 0;
}

static int t_read(const struct shell_transport *transport, void *data,
		  size_t length, size_t *cnt)
{
	struct ssh_shell_ctx *c = transport->ctx;

	*cnt = ring_buf_get(&c->rx_rb, data, length);
	return 0;
}

static const struct shell_transport_api ssh_shell_api = {
	.init   = t_init,
	.uninit = t_uninit,
	.enable = t_enable,
	.write  = t_write,
	.read   = t_read,
};

/* ── One static shell instance per connection slot ───────────────────────── */

#define DEFINE_SSH_SHELL(i, _)                                                 \
	static struct ssh_shell_ctx ssh_ctx_##i;                               \
	static struct shell_transport ssh_tr_##i = {                           \
		.api = &ssh_shell_api,                                         \
		.ctx = &ssh_ctx_##i,                                           \
	};                                                                     \
	SHELL_DEFINE(ssh_shell_##i, "$ ", &ssh_tr_##i, 4, 0,                   \
		     SHELL_FLAG_OLF_CRLF);

LISTIFY(SSH_MAX_CONNS, DEFINE_SSH_SHELL, ())

#define SH_PTR(i, _)  &ssh_shell_##i
#define CTX_PTR(i, _) &ssh_ctx_##i

static const struct shell *const ssh_shells[SSH_MAX_CONNS] = {
	LISTIFY(SSH_MAX_CONNS, SH_PTR, (,))
};
static struct ssh_shell_ctx *const ssh_ctxs[SSH_MAX_CONNS] = {
	LISTIFY(SSH_MAX_CONNS, CTX_PTR, (,))
};

static struct ssh_shell_ctx *ctx_for(const struct shell *sh)
{
	for (int i = 0; i < SSH_MAX_CONNS; i++) {
		if (ssh_shells[i] == sh) {
			return ssh_ctxs[i];
		}
	}
	return NULL;   /* not an SSH shell (e.g. the serial console) */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ssh_shell_transport_init(void)
{
	struct shell_backend_config_flags cfg = SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

	for (int i = 0; i < SSH_MAX_CONNS; i++) {
		/* shell_init() starts the per-instance thread; park it with
		 * shell_stop() until a session claims the slot.  Output is
		 * swallowed by t_write() while ssh == NULL. */
		shell_init(ssh_shells[i], NULL, cfg, false, 0);
		shell_stop(ssh_shells[i]);
	}
}

const struct shell *ssh_shell_claim(int slot, WOLFSSH *ssh)
{
	struct ssh_shell_ctx *c = ssh_ctxs[slot];

	ring_buf_reset(&c->rx_rb);
	k_sem_reset(&c->rx_sem);
	k_sem_reset(&c->done);
	atomic_set(&c->running, 0);
	c->ssh = ssh;
	atomic_set(&c->open, 1);
#if defined(CONFIG_KLAUSSCPU_SHELL_CMDS)
	shell_cwd_reset(ssh_shells[slot]);   /* new session starts at /SD:/ */
	shell_prompt_sync(ssh_shells[slot]); /* prompt shows cwd from 1st line */
#endif
	shell_start(ssh_shells[slot]);

	/* shell_start() queues the prompt into the shell's transport buffer but
	 * does not flush it — that only happens at the end of shell_process(),
	 * which the shell thread runs when it wakes on an RX_RDY signal (it
	 * otherwise blocks in k_poll forever).  Without a nudge the first prompt
	 * stays buffered until the user's first keypress.  Raise RX_RDY now (the
	 * ring is empty, so the read is a harmless no-op) to flush it. */
	if (c->handler != NULL) {
		c->handler(SHELL_TRANSPORT_EVT_RX_RDY, c->context);
	}
	return ssh_shells[slot];
}

bool ssh_shell_release(int slot)
{
	struct ssh_shell_ctx *c = ssh_ctxs[slot];
	bool drained = true;

	atomic_set(&c->open, 0);
	k_sem_give(&c->rx_sem);          /* unblock any getc() in `run` -> EOF */

	/* If a `run` extension is still executing on the shell thread, wait for
	 * it to unwind (it should exit promptly once getchar() returns EOF)
	 * before the caller frees the session.  A program that ignores EOF and
	 * loops forever cannot be safely torn down — report that. */
	if (atomic_get(&c->running)) {
		drained = (k_sem_take(&c->done, K_SECONDS(2)) == 0);
	}

	shell_stop(ssh_shells[slot]);
	c->ssh = NULL;
	return drained;
}

void ssh_shell_feed(int slot, const uint8_t *data, size_t len)
{
	struct ssh_shell_ctx *c = ssh_ctxs[slot];

	ring_buf_put(&c->rx_rb, data, len);
	k_sem_give(&c->rx_sem);
	if (c->handler != NULL) {
		c->handler(SHELL_TRANSPORT_EVT_RX_RDY, c->context);
	}
}

WOLFSSH *ssh_shell_ssh(const struct shell *sh)
{
	struct ssh_shell_ctx *c = ctx_for(sh);

	return c ? c->ssh : NULL;
}

int ssh_shell_getc(const struct shell *sh)
{
	struct ssh_shell_ctx *c = ctx_for(sh);
	uint8_t ch;

	if (c == NULL) {
		return -1;   /* serial console: no SSH input ring */
	}

	while (1) {
		if (ring_buf_get(&c->rx_rb, &ch, 1) == 1) {
			return (int)ch;
		}
		if (!atomic_get(&c->open)) {
			return -1;   /* session closed */
		}
		k_sem_take(&c->rx_sem, K_FOREVER);
	}
}

/* ── `run` command — load & execute an llext extension ───────────────────────
 * Registered here (not shell_cmds.c) because it needs the per-session WOLFSSH*.
 * Available on the serial console too: ssh_shell_ssh() returns NULL there and
 * the loader routes the extension's stdio to the UART. */

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	const char *arg = (argc > 1) ? argv[1] : NULL;
	char pathbuf[128];
	const char *filename;

	if (arg != NULL && arg[0] == '/') {
		filename = arg;
	} else if (arg != NULL) {
		snprintf(pathbuf, sizeof(pathbuf), "/SD:/%s", arg);
		filename = pathbuf;
	} else {
		filename = "/SD:/PROG.LLEXT";
	}

	shell_print(sh, "Loading %s ...", filename);

	/* Mark the slot busy so a concurrent disconnect (ssh_shell_release on
	 * the connection thread) waits for this program to unwind before the
	 * session is freed. */
	struct ssh_shell_ctx *c = ctx_for(sh);

	if (c != NULL) {
		atomic_set(&c->running, 1);
	}

	int rc = llext_run_from_sd(filename, sh);

	if (c != NULL) {
		atomic_set(&c->running, 0);
		k_sem_give(&c->done);
	}

	if (rc < 0) {
		shell_print(sh, "Load/run failed (code %d)", rc);
	} else {
		shell_print(sh, "Program exited: %d", rc);
	}
	return 0;
}

SHELL_CMD_ARG_REGISTER(run, NULL,
		       "Load & run extension [file] (default /SD:/PROG.LLEXT)",
		       cmd_run, 1, 1);

/* ssh_shell_transport.h — Zephyr shell backend over a wolfSSH session.
 *
 * Each SSH connection slot owns one statically-defined Zephyr shell instance.
 * Binding a live WOLFSSH* to a slot turns that shell on; all commands
 * registered with SHELL_CMD_REGISTER (see shell_cmds.c) then work over SSH
 * exactly as they do on the serial console — one command set, no duplication.
 *
 * Threading model (see ssh_shell_transport.c for the rationale):
 *   - The per-connection thread (in ssh_server.c) is the SOLE reader of the
 *     socket.  It pushes decrypted bytes into the slot's RX ring via
 *     ssh_shell_feed() and signals the shell core.
 *   - The shell core runs on its own thread (created by SHELL_DEFINE) and is
 *     the SOLE writer: transport write() encrypts+sends via wolfSSH.
 *   - A running `run` extension consumes input through ssh_shell_getc(), which
 *     drains the same RX ring, so input is never read from two threads at once.
 */

#ifndef SSH_SHELL_TRANSPORT_H
#define SSH_SHELL_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>
#include <wolfssh/ssh.h>

/* Initialise (shell_init + shell_stop) every per-slot shell instance once.
 * Call from ssh_server_start() before the listener thread is created. */
void ssh_shell_transport_init(void);

/* Bind a freshly-accepted session to slot's shell and start it.  Returns the
 * shell instance for that slot (for diagnostics; not normally needed). */
const struct shell *ssh_shell_claim(int slot, WOLFSSH *ssh);

/* Stop slot's shell and unbind the session (call on disconnect).
 *
 * Signals EOF to a running `run` extension and waits briefly for it to unwind.
 * Returns true if the session is now idle and the caller may safely free the
 * WOLFSSH* / close the socket; false if an extension ignored EOF and is still
 * executing — in that case the caller MUST NOT free the session or reuse the
 * slot (doing so would be a use-after-free on the shell thread). */
bool ssh_shell_release(int slot);

/* Push received bytes into slot's RX ring and wake the shell / any getc().
 * Called only from the per-connection (reader) thread. */
void ssh_shell_feed(int slot, const uint8_t *data, size_t len);

/* Map a shell instance back to its WOLFSSH* (NULL if `sh` is not an SSH shell,
 * e.g. the serial console).  Used by the `run` command. */
WOLFSSH *ssh_shell_ssh(const struct shell *sh);

/* Blocking single-byte read from `sh`'s RX ring; -1 on EOF/close, or if `sh`
 * is not an SSH shell.  Used by the llext input router during `run`. */
int ssh_shell_getc(const struct shell *sh);

#endif /* SSH_SHELL_TRANSPORT_H */

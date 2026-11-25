#ifndef SC_SSH_H
#define SC_SSH_H

#include "common.h"

#include <stdbool.h>

#include "util/intr.h"
#include "util/process.h"

/*
 * SSH transport (scrcpy-ssh fork): replaces adb with the system `ssh` client.
 *
 * The device runs an SSH server whose exec channels run commands as root
 * (e.g. RootSSH, Magisk SSH modules, or any server + `su`). Authentication
 * must be non-interactive (key or agent), since several ssh processes are
 * spawned per session.
 */

// Prefix used for the "serial" of SSH devices, e.g. "ssh:root@10.0.0.2:2222"
#define SC_SSH_SERIAL_PREFIX "ssh:"

// Relay helper (dex in a jar) used to reach the server's abstract socket
#define SC_SSH_RELAY_FILENAME "scrcpy-ssh-relay.jar"
#define SC_SSH_DEVICE_RELAY_PATH "/data/local/tmp/scrcpy-ssh-relay.jar"

struct sc_ssh_target {
    char *dest; // [user@]host
    char *port; // may be NULL (ssh default / ~/.ssh/config)
};

bool
sc_ssh_is_serial(const char *serial);

/**
 * Parse "[user@]host[:port]" (or "ssh:[user@]host[:port]")
 */
bool
sc_ssh_target_parse(struct sc_ssh_target *target, const char *spec);

void
sc_ssh_target_destroy(struct sc_ssh_target *target);

/**
 * Build the serial string "ssh:<spec>" (to be freed by the caller)
 */
char *
sc_ssh_target_to_serial(const struct sc_ssh_target *target);

/**
 * Quote a string for the remote POSIX shell (to be freed by the caller)
 */
char *
sc_ssh_shell_quote(const char *s);

/**
 * Execute a remote command; `remote_argv` items are joined with spaces by ssh
 * (exactly like "adb shell"), so they must already be shell-safe.
 */
enum sc_process_result
sc_ssh_execute_p(const struct sc_ssh_target *target,
                 const char *const remote_argv[], sc_pid *pid, unsigned flags,
                 sc_pipe *pin, sc_pipe *pout, sc_pipe *perr);

sc_pid
sc_ssh_execute(const struct sc_ssh_target *target,
               const char *const remote_argv[], unsigned flags);

/**
 * Run a remote command and wait for it (output inherited unless flags say
 * otherwise). Returns true on exit code 0.
 */
bool
sc_ssh_run(struct sc_intr *intr, const struct sc_ssh_target *target,
           const char *const remote_argv[], unsigned flags);

/**
 * Copy a local file to the device (streams it into `cat > remote`).
 */
bool
sc_ssh_push(struct sc_intr *intr, const struct sc_ssh_target *target,
            const char *local, const char *remote);

/**
 * Push an APK to /data/local/tmp and install it with `pm install`.
 */
bool
sc_ssh_install(struct sc_intr *intr, const struct sc_ssh_target *target,
               const char *local);

/**
 * Check that the connection works non-interactively (prints a helpful error).
 */
bool
sc_ssh_check(struct sc_intr *intr, const struct sc_ssh_target *target);

#endif

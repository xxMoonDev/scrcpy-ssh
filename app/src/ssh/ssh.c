#include "ssh.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/env.h"
#include "util/log.h"
#include "util/str.h"

#define SC_SSH_MAX_ARGS 160

bool
sc_ssh_is_serial(const char *serial) {
    return serial && !strncmp(serial, SC_SSH_SERIAL_PREFIX,
                              sizeof(SC_SSH_SERIAL_PREFIX) - 1);
}

bool
sc_ssh_target_parse(struct sc_ssh_target *target, const char *spec) {
    if (sc_ssh_is_serial(spec)) {
        spec += sizeof(SC_SSH_SERIAL_PREFIX) - 1;
    }
    if (!*spec) {
        LOGE("Empty --ssh destination");
        return false;
    }

    target->dest = NULL;
    target->port = NULL;

    const char *colon = strrchr(spec, ':');
    // "host:port" (IPv6 literals are not supported in this short form;
    // use a Host alias in ~/.ssh/config instead)
    if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
        size_t len = colon - spec;
        target->dest = malloc(len + 1);
        if (target->dest) {
            memcpy(target->dest, spec, len);
            target->dest[len] = '\0';
        }
        target->port = strdup(colon + 1);
        if (!target->dest || !target->port) {
            free(target->dest);
            free(target->port);
            LOG_OOM();
            return false;
        }
    } else {
        target->dest = strdup(spec);
        if (!target->dest) {
            LOG_OOM();
            return false;
        }
    }

    if (strpbrk(target->dest, " \t'\"`$;&|<>")) {
        LOGE("Invalid --ssh destination: %s", spec);
        sc_ssh_target_destroy(target);
        return false;
    }
    return true;
}

void
sc_ssh_target_destroy(struct sc_ssh_target *target) {
    free(target->dest);
    free(target->port);
    target->dest = NULL;
    target->port = NULL;
}

char *
sc_ssh_target_to_serial(const struct sc_ssh_target *target) {
    char *s;
    int r = target->port
          ? asprintf(&s, SC_SSH_SERIAL_PREFIX "%s:%s", target->dest, target->port)
          : asprintf(&s, SC_SSH_SERIAL_PREFIX "%s", target->dest);
    if (r == -1) {
        LOG_OOM();
        return NULL;
    }
    return s;
}

char *
sc_ssh_shell_quote(const char *s) {
    // 'abc' with every ' replaced by '\''
    size_t len = 2;
    for (const char *p = s; *p; ++p) {
        len += *p == '\'' ? 4 : 1;
    }
    char *q = malloc(len + 1);
    if (!q) {
        LOG_OOM();
        return NULL;
    }
    char *w = q;
    *w++ = '\'';
    for (const char *p = s; *p; ++p) {
        if (*p == '\'') {
            memcpy(w, "'\\''", 4);
            w += 4;
        } else {
            *w++ = *p;
        }
    }
    *w++ = '\'';
    *w = '\0';
    return q;
}

static const char *
get_ssh_executable(void) {
    static char *exe = NULL;
    if (!exe) {
        exe = sc_get_env("SSH");
        if (!exe) {
            exe = strdup("ssh");
        }
    }
    return exe;
}

#define SC_SSH_MAX_EXTRA_ARGS 32

// Extra ssh options from $SCRCPY_SSH_ARGS, one argument per line (so that
// paths may contain spaces), e.g. "-i\nC:\keys\id\n-o\nUserKnownHostsFile=..."
static unsigned
get_extra_args(const char **out) {
    static char *buf = NULL;
    static const char *args[SC_SSH_MAX_EXTRA_ARGS];
    static unsigned count = 0;
    static bool init = false;
    if (!init) {
        init = true;
        buf = sc_get_env("SCRCPY_SSH_ARGS");
        if (buf) {
            char *p = buf;
            while (*p && count < SC_SSH_MAX_EXTRA_ARGS) {
                char *nl = strpbrk(p, "\r\n");
                if (nl) {
                    *nl = '\0';
                }
                if (*p) {
                    args[count++] = p;
                }
                if (!nl) {
                    break;
                }
                p = nl + 1;
            }
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        out[i] = args[i];
    }
    return count;
}

static unsigned
build_argv(const struct sc_ssh_target *target, const char *argv[],
           const char *const remote_argv[]) {
    unsigned n = 0;
    argv[n++] = get_ssh_executable();
    // Never prompt (several ssh processes run concurrently); fail fast
    argv[n++] = "-o";
    argv[n++] = "BatchMode=yes";
    argv[n++] = "-o";
    argv[n++] = "ConnectTimeout=10";
    argv[n++] = "-o";
    argv[n++] = "ServerAliveInterval=15";
    argv[n++] = "-o";
    argv[n++] = "LogLevel=ERROR";
    argv[n++] = "-T"; // no pty: binary-safe pipes
    n += get_extra_args(&argv[n]);
    if (target->port) {
        argv[n++] = "-p";
        argv[n++] = target->port;
    }
    argv[n++] = target->dest;
    for (unsigned i = 0; remote_argv[i]; ++i) {
        assert(n < SC_SSH_MAX_ARGS - 1);
        argv[n++] = remote_argv[i];
    }
    argv[n] = NULL;
    return n;
}

enum sc_process_result
sc_ssh_execute_p(const struct sc_ssh_target *target,
                 const char *const remote_argv[], sc_pid *pid, unsigned flags,
                 sc_pipe *pin, sc_pipe *pout, sc_pipe *perr) {
    const char *argv[SC_SSH_MAX_ARGS];
    build_argv(target, argv, remote_argv);
    enum sc_process_result r =
        sc_process_execute_p(argv, pid, flags, pin, pout, perr);
    if (r == SC_PROCESS_ERROR_MISSING_BINARY) {
        LOGE("Command not found: %s", argv[0]);
        LOGE("Install the OpenSSH client, or set the SSH environment variable "
             "to the ssh executable");
    } else if (r != SC_PROCESS_SUCCESS) {
        LOGE("Failed to execute: %s", argv[0]);
    }
    return r;
}

sc_pid
sc_ssh_execute(const struct sc_ssh_target *target,
               const char *const remote_argv[], unsigned flags) {
    sc_pid pid;
    enum sc_process_result r =
        sc_ssh_execute_p(target, remote_argv, &pid, flags, NULL, NULL, NULL);
    return r == SC_PROCESS_SUCCESS ? pid : SC_PROCESS_NONE;
}

static bool
wait_success_intr(struct sc_intr *intr, sc_pid pid, const char *what) {
    if (intr && !sc_intr_set_process(intr, pid)) {
        sc_process_terminate(pid);
        sc_process_wait(pid, true);
        return false;
    }
    sc_exit_code code = sc_process_wait(pid, false);
    if (intr) {
        sc_intr_set_process(intr, SC_PROCESS_NONE);
    }
    sc_process_close(pid);
    if (code) {
        if (code == 255) {
            LOGE("ssh %s failed (connection or authentication error)", what);
        } else if (code != SC_EXIT_CODE_NONE) {
            LOGE("ssh %s returned with value %" SC_PRIexitcode, what, code);
        } else {
            LOGE("ssh %s exited unexpectedly", what);
        }
        return false;
    }
    return true;
}

bool
sc_ssh_run(struct sc_intr *intr, const struct sc_ssh_target *target,
           const char *const remote_argv[], unsigned flags) {
    sc_pid pid = sc_ssh_execute(target, remote_argv, flags);
    if (pid == SC_PROCESS_NONE) {
        return false;
    }
    return wait_success_intr(intr, pid, remote_argv[0]);
}

bool
sc_ssh_push(struct sc_intr *intr, const struct sc_ssh_target *target,
            const char *local, const char *remote) {
    FILE *f = fopen(local, "rb");
    if (!f) {
        LOGE("Could not open %s", local);
        return false;
    }

    char *qremote = sc_ssh_shell_quote(remote);
    if (!qremote) {
        fclose(f);
        return false;
    }
    char *cmd;
    if (asprintf(&cmd, "cat > %s", qremote) == -1) {
        free(qremote);
        fclose(f);
        LOG_OOM();
        return false;
    }
    free(qremote);

    const char *const remote_argv[] = {cmd, NULL};
    sc_pid pid;
    sc_pipe pin;
    enum sc_process_result r =
        sc_ssh_execute_p(target, remote_argv, &pid, 0, &pin, NULL, NULL);
    free(cmd);
    if (r != SC_PROCESS_SUCCESS) {
        fclose(f);
        return false;
    }

    bool write_ok = true;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (sc_intr_is_interrupted(intr)) {
            write_ok = false;
            break;
        }
        if (sc_pipe_write_all(pin, buf, n) != (ssize_t) n) {
            write_ok = false;
            break;
        }
    }
    fclose(f);
    sc_pipe_close(pin); // EOF for the remote "cat"

    bool ok = wait_success_intr(intr, pid, "push");
    if (!write_ok) {
        LOGE("Could not send %s over ssh", local);
    }
    return ok && write_ok;
}

bool
sc_ssh_install(struct sc_intr *intr, const struct sc_ssh_target *target,
               const char *local) {
    static const char *tmp = "/data/local/tmp/scrcpy-ssh-install.apk";
    if (!sc_ssh_push(intr, target, local, tmp)) {
        return false;
    }
    const char *const argv[] = {"pm", "install", "-r", tmp, ";", "r=$?;",
                                "rm", "-f", tmp, ";", "exit", "$r", NULL};
    return sc_ssh_run(intr, target, argv, 0);
}

bool
sc_ssh_check(struct sc_intr *intr, const struct sc_ssh_target *target) {
    const char *const argv[] = {"true", NULL};
    sc_pid pid = sc_ssh_execute(target, argv, 0);
    if (pid == SC_PROCESS_NONE) {
        return false;
    }
    if (intr && !sc_intr_set_process(intr, pid)) {
        sc_process_terminate(pid);
        sc_process_wait(pid, true);
        return false;
    }
    sc_exit_code code = sc_process_wait(pid, false);
    if (intr) {
        sc_intr_set_process(intr, SC_PROCESS_NONE);
    }
    sc_process_close(pid);
    if (code) {
        LOGE("Could not connect with ssh to %s%s%s", target->dest,
             target->port ? " port " : "", target->port ? target->port : "");
        LOGE("scrcpy needs non-interactive login: add your public key "
             "(e.g. ~/.ssh/id_ed25519.pub) to the device's authorized keys, "
             "or load it in ssh-agent. Test with:");
        LOGE("    ssh -o BatchMode=yes %s%s %s true",
             target->port ? "-p " : "", target->port ? target->port : "",
             target->dest);
        return false;
    }
    return true;
}

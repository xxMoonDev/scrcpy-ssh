#include "ssh_tunnel.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_thread.h> // SDL_DetachThread

#include "util/log.h"

struct sc_ssh_relay {
    sc_socket socket;
    sc_pid pid;
    sc_pipe pin;  // to ssh stdin (-> device socket)
    sc_pipe pout; // from ssh stdout (<- device socket)
};

// socket -> ssh stdin
static int
run_upstream(void *data) {
    struct sc_ssh_relay *relay = data;
    char buf[16384];
    for (;;) {
        ssize_t r = net_recv(relay->socket, buf, sizeof(buf));
        if (r <= 0) {
            break;
        }
        if (sc_pipe_write_all(relay->pin, buf, r) != r) {
            break;
        }
    }
    sc_pipe_close(relay->pin); // remote relay sees EOF and exits
    return 0;
}

// ssh stdout -> socket (owns the relay)
static int
run_downstream(void *data) {
    struct sc_ssh_relay *relay = data;

    sc_thread upstream;
    bool up = sc_thread_create(&upstream, run_upstream, "scrcpy-ssh-up", relay);
    if (!up) {
        sc_pipe_close(relay->pin);
    }

    char buf[65536];
    for (;;) {
        ssize_t r = sc_pipe_read(relay->pout, buf, sizeof(buf));
        if (r <= 0) {
            break;
        }
        if (net_send_all(relay->socket, buf, r) != r) {
            break;
        }
    }

    // Wake up the upstream thread blocked in recv()
    net_interrupt(relay->socket);
    if (up) {
        sc_thread_join(&upstream, NULL);
    }
    sc_process_terminate(relay->pid);
    sc_process_wait(relay->pid, true);
    sc_pipe_close(relay->pout);
    net_close(relay->socket);
    free(relay);
    return 0;
}

static sc_pid
start_relay_process(struct sc_ssh_tunnel *tunnel, sc_pipe *pin, sc_pipe *pout) {
    const char *const remote_argv[] = {
        "CLASSPATH=" SC_SSH_DEVICE_RELAY_PATH,
        "app_process",
        "/",
        "ScrcpySshRelay",
        tunnel->device_socket_name,
        NULL,
    };
    sc_pid pid;
    // stderr discarded: failed attempts are expected until the server listens
    enum sc_process_result r =
        sc_ssh_execute_p(tunnel->target, remote_argv, &pid,
                         SC_PROCESS_NO_STDERR, pin, pout, NULL);
    return r == SC_PROCESS_SUCCESS ? pid : SC_PROCESS_NONE;
}

static bool
handle_connection(struct sc_ssh_tunnel *tunnel, sc_socket socket) {
    sc_pipe pin, pout;
    sc_pid pid = start_relay_process(tunnel, &pin, &pout);
    if (pid == SC_PROCESS_NONE) {
        net_close(socket);
        return false;
    }

    // The relay writes one 'K' byte once connected to the device socket
    char ack;
    ssize_t r = sc_pipe_read_all(pout, &ack, 1);
    if (r != 1 || ack != 'K') {
        // Device socket not listening yet: close, the client will retry
        LOGD("ssh relay: device socket not ready");
        sc_pipe_close(pin);
        sc_process_terminate(pid);
        sc_process_wait(pid, true);
        sc_pipe_close(pout);
        net_close(socket);
        return false;
    }

    struct sc_ssh_relay *relay = malloc(sizeof(*relay));
    if (!relay) {
        LOG_OOM();
        sc_pipe_close(pin);
        sc_process_terminate(pid);
        sc_process_wait(pid, true);
        sc_pipe_close(pout);
        net_close(socket);
        return false;
    }
    relay->socket = socket;
    relay->pid = pid;
    relay->pin = pin;
    relay->pout = pout;

    sc_thread thread;
    if (!sc_thread_create(&thread, run_downstream, "scrcpy-ssh-down", relay)) {
        LOGE("Could not start ssh relay thread");
        sc_pipe_close(pin);
        sc_process_terminate(pid);
        sc_process_wait(pid, true);
        sc_pipe_close(pout);
        net_close(socket);
        free(relay);
        return false;
    }
    // The relay outlives the tunnel (the tunnel is closed once connected)
    SDL_DetachThread(thread.thread);
    LOGD("ssh relay established");
    return true;
}

static int
run_accept(void *data) {
    struct sc_ssh_tunnel *tunnel = data;
    unsigned done = 0;
    while (done < tunnel->expected) {
        sc_socket socket = net_accept(tunnel->server_socket);
        if (socket == SC_SOCKET_NONE) {
            // closed by sc_ssh_tunnel_close()
            break;
        }
        if (handle_connection(tunnel, socket)) {
            ++done;
        } else if (done) {
            // The server was reachable but is not anymore: give up (the
            // client will fail on its side)
            LOGE("ssh relay failed after the server was reached");
            break;
        }
        // else: server not listening yet, the client retries
    }
    return 0;
}

void
sc_ssh_tunnel_init(struct sc_ssh_tunnel *tunnel) {
    tunnel->enabled = false;
    tunnel->target = NULL;
    tunnel->device_socket_name = NULL;
    tunnel->server_socket = SC_SOCKET_NONE;
    tunnel->local_port = 0;
    tunnel->expected = 0;
}

bool
sc_ssh_tunnel_open(struct sc_ssh_tunnel *tunnel,
                   const struct sc_ssh_target *target,
                   const char *device_socket_name,
                   struct sc_port_range port_range, unsigned expected) {
    assert(!tunnel->enabled);
    assert(expected);
    tunnel->expected = expected;

    tunnel->device_socket_name = strdup(device_socket_name);
    if (!tunnel->device_socket_name) {
        LOG_OOM();
        return false;
    }
    tunnel->target = target;

    uint16_t port = port_range.first;
    for (;;) {
        sc_socket s = net_socket();
        if (s != SC_SOCKET_NONE) {
            if (net_listen(s, IPV4_LOCALHOST, port, 4)) {
                tunnel->server_socket = s;
                tunnel->local_port = port;
                break;
            }
            net_close(s);
        }
        if (port >= port_range.last) {
            LOGE("Could not listen on any port in range %" PRIu16 ":%" PRIu16,
                 port_range.first, port_range.last);
            free(tunnel->device_socket_name);
            tunnel->device_socket_name = NULL;
            return false;
        }
        ++port;
    }

    if (!sc_thread_create(&tunnel->thread, run_accept, "scrcpy-ssh-tun",
                          tunnel)) {
        LOGE("Could not start ssh tunnel thread");
        net_close(tunnel->server_socket);
        free(tunnel->device_socket_name);
        tunnel->device_socket_name = NULL;
        return false;
    }

    LOGD("ssh tunnel listening on localhost:%" PRIu16, tunnel->local_port);
    tunnel->enabled = true;
    return true;
}

void
sc_ssh_tunnel_close(struct sc_ssh_tunnel *tunnel, bool wait) {
    if (!tunnel->enabled) {
        return;
    }
    if (wait) {
        // the accept thread terminates once all connections are relayed
        sc_thread_join(&tunnel->thread, NULL);
        net_close(tunnel->server_socket);
    } else {
        net_interrupt(tunnel->server_socket);
        net_close(tunnel->server_socket);
        sc_thread_join(&tunnel->thread, NULL);
    }
    free(tunnel->device_socket_name);
    tunnel->device_socket_name = NULL;
    tunnel->enabled = false;
}

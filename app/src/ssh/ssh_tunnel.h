#ifndef SC_SSH_TUNNEL_H
#define SC_SSH_TUNNEL_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "options.h"
#include "ssh/ssh.h"
#include "util/intr.h"
#include "util/net.h"
#include "util/thread.h"

/*
 * Equivalent of "adb forward tcp:PORT localabstract:NAME" over ssh.
 *
 * The client listens on localhost:PORT. Each accepted connection spawns
 * `ssh <dest> app_process ... ScrcpySshRelay NAME`, whose stdin/stdout are
 * bridged with the device's abstract socket. Connections are relayed one at a
 * time (the next one is accepted only once the previous relay reached the
 * device socket), so the server accepts video/audio/control in the same order
 * as the client connects them.
 *
 * Closing the tunnel stops accepting new connections; established relays keep
 * running until either side closes.
 */
struct sc_ssh_tunnel {
    bool enabled;
    const struct sc_ssh_target *target;
    char *device_socket_name;
    sc_socket server_socket;
    uint16_t local_port;
    sc_thread thread;
    unsigned expected; // number of connections to relay (video/audio/control)
};

void
sc_ssh_tunnel_init(struct sc_ssh_tunnel *tunnel);

bool
sc_ssh_tunnel_open(struct sc_ssh_tunnel *tunnel,
                   const struct sc_ssh_target *target,
                   const char *device_socket_name,
                   struct sc_port_range port_range, unsigned expected);

/**
 * Close the tunnel.
 *
 * If `wait` is set (all client sockets are connected), wait until every
 * expected connection has been relayed before closing the listening socket;
 * otherwise abort immediately.
 */
void
sc_ssh_tunnel_close(struct sc_ssh_tunnel *tunnel, bool wait);

#endif

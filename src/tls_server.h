#ifndef DYMO_TLS_SERVER_H
#define DYMO_TLS_SERVER_H

#include "server.h"

// Run an HTTPS server on cfg->bind_addr:cfg->https_port using LibreSSL's
// native libtls API. Spawns a thread per accepted connection; each thread
// does: tls_accept_socket → read HTTP request → dispatch → tls_write response.
// Returns 0 on clean shutdown, -1 on fatal error.
//
// The `running` pointer is polled between accept() calls; setting *running=0
// causes the loop to exit on the next iteration.
int tls_server_run(const server_cfg_t *cfg, volatile int *running);

#endif

#ifndef DVS_IPC_SERVER_H
#define DVS_IPC_SERVER_H

#include <stddef.h>

/* Tuneable simulated prepare duration (ms). Default 50; tests may set 0. */
void dvs_ipc_set_prepare_delay_ms(int ms);

/* Run the IPC server (blocking). JSON line protocol over a Unix domain
 * socket: long-lived connections, messageId echo, async prepare with
 * media.prepare.result events, and broadcast events on commit. */
int dvs_ipc_server_run(const char* sock_path);

#endif

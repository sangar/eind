#ifndef EIND_SERVER_H
#define EIND_SERVER_H

#include <stdbool.h>

#include "../core/arena.h"
#include "../core/json.h"
#include "../core/util.h"
#include "../index/index.h"

/*
 * The socket protocol is JSON lines: one request object per line, one
 * response object per request. A new request on a connection cancels the one
 * before it, which then answers {"id":..,"cancelled":true}. See docs/protocol.md.
 */
#define DEFAULT_LIMIT 100

typedef struct Server Server;

/* server_start binds the socket and answers queries from ix's snapshots on background threads. */
Server *server_start(Index *ix, const char *socket_path, const char *index_path, Err *err);
void server_stop(Server *srv);

/* default_socket_path is $EIND_SOCKET, else $XDG_RUNTIME_DIR/eind.sock, else a per-user temp file. */
char *default_socket_path(void);
bool server_running(const char *socket_path);

#endif

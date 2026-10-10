#ifndef EIND_SERVER_H
#define EIND_SERVER_H

#include "mc/concurrency/threadpool.h"
#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"
#include "../index/index.h"

/*
 * The socket protocol is JSON lines: one request object per line, one
 * response object per request. A new request on a connection cancels the one
 * before it, which then answers {"id":..,"cancelled":true}. See docs/protocol.md.
 */
enum { DEFAULT_LIMIT = 100 };

typedef struct Server Server;

/*
 * server_start binds the socket and answers queries from ix's snapshots on
 * background threads, searching on cpu, until server_stop. A socket another
 * daemon still answers on is ERR_IO.
 */
[[nodiscard]] Error server_start(ThreadPool *cpu, Index *ix, String socket_path, String index_path, Server **server,
                                 Err *err);
/* server_stop closes the listener and every open connection, so shutdown never waits on an idle client. */
void server_stop(Server *srv);

/* default_socket_path is $EIND_SOCKET, else $XDG_RUNTIME_DIR/eind.sock, else a per-user temp file. */
String default_socket_path(Arena *arena);
bool server_running(String socket_path);

#endif

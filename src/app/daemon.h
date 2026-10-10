#ifndef EIND_DAEMON_H
#define EIND_DAEMON_H

#include "mc/core/error.h"
#include "mc/text/str.h"

typedef struct {
    String config_path;
    String index_path;
    String socket_path;
    bool serve;               /* also answer queries over the socket */
    int64_t save_interval_ms; /* how often accumulated changes are compacted and saved */
} DaemonOptions;

/*
 * daemon_run keeps the index fresh from filesystem events until SIGINT,
 * SIGTERM or SIGHUP, compacting and saving it while changes accumulate, and
 * optionally answers queries over the socket from the same snapshots.
 */
[[nodiscard]] Error daemon_run(const DaemonOptions *o, Err *err);

#endif

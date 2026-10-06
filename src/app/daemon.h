#ifndef EIND_DAEMON_H
#define EIND_DAEMON_H

#include <stdbool.h>

#include "../core/util.h"

typedef struct {
    const char *config_path;
    const char *index_path;
    const char *socket_path;
    bool serve;            /* also answer queries over the socket */
    int save_interval_ms;  /* how often accumulated changes are compacted and saved */
} DaemonOptions;

/*
 * daemon_run keeps the index fresh from filesystem events until SIGINT or
 * SIGTERM, compacting and saving it while changes accumulate, and optionally
 * answers queries over the socket from the same snapshots.
 */
bool daemon_run(const DaemonOptions *o, Err *err);

#endif

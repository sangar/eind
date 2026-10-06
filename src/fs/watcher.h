#ifndef EIND_WATCHER_H
#define EIND_WATCHER_H

#include <stdbool.h>

#include "../core/util.h"

/*
 * Watcher turns filesystem notifications (FSEvents on macOS, inotify on
 * Linux) into batches of changed paths. Notifications are unreliable in
 * order and detail, so it reports only which paths to look at again.
 */
typedef struct Watcher Watcher;

Watcher *watcher_open(const StrList *roots, Err *err);
void watcher_close(Watcher *w);

/* inotify watches single directories, so every indexed directory must be added. */
bool watcher_needs_dirs(void);
void watcher_add_dir(Watcher *w, const char *path);

/*
 * watcher_collect waits up to timeout_ms for a change, then keeps collecting
 * until changes pause, so a path touched many times in a row is reported
 * once. It returns the number of distinct paths added, or -1 on error.
 */
int watcher_collect(Watcher *w, int timeout_ms, StrList *paths, Err *err);

/* The platform backends implement these. */
typedef struct WatchBackend WatchBackend;
WatchBackend *backend_open(const StrList *roots, Err *err);
void backend_close(WatchBackend *b);
bool backend_needs_dirs(void);
void backend_add_dir(WatchBackend *b, const char *path);
/* backend_next returns 1 with a malloc'd path, 0 on timeout, -1 on error. */
int backend_next(WatchBackend *b, int timeout_ms, char **path, Err *err);

#endif

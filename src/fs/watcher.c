#include "watcher.h"

#include <stdlib.h>
#include <string.h>

#include "../core/hash.h"

#define SETTLE_MS 250
#define BURST_LIMIT_MS 2000

struct Watcher {
    WatchBackend *backend;
};

Watcher *watcher_open(const StrList *roots, Err *err) {
    WatchBackend *b = backend_open(roots, err);
    if (!b) return NULL;
    Watcher *w = xcalloc(1, sizeof *w);
    w->backend = b;
    return w;
}

void watcher_close(Watcher *w) {
    if (!w) return;
    backend_close(w->backend);
    free(w);
}

bool watcher_needs_dirs(void) { return backend_needs_dirs(); }

void watcher_add_dir(Watcher *w, const char *path) { backend_add_dir(w->backend, path); }

/* PathSet remembers which paths a batch already holds. */
typedef struct {
    const StrList *paths;
    IdTable table;
} PathSet;

static uint64_t hash_path(const char *p) { return hash_bytes(p, strlen(p), 0); }

static uint64_t rehash_entry(void *ctx, uint32_t i) {
    const PathSet *set = ctx;
    return hash_path(set->paths->items[i]);
}

typedef struct {
    const PathSet *set;
    const char *path;
} PathKey;

static bool path_equal(void *ctx, uint32_t i) {
    const PathKey *k = ctx;
    return strcmp(k->set->paths->items[i], k->path) == 0;
}

static bool add_unique(PathSet *set, StrList *paths, char *path) {
    uint64_t h = hash_path(path);
    PathKey key = {set, path};
    if (idtable_find(&set->table, h, path_equal, &key) != UINT32_MAX) {
        free(path);
        return false;
    }
    strlist_push_owned(paths, path);
    idtable_insert(&set->table, h, (uint32_t)(paths->len - 1), rehash_entry, set);
    return true;
}

int watcher_collect(Watcher *w, int timeout_ms, StrList *paths, Err *err) {
    char *path;
    int rc = backend_next(w->backend, timeout_ms, &path, err);
    if (rc <= 0) return rc;

    PathSet set = {.paths = paths};
    idtable_init(&set.table, 64);
    for (size_t i = 0; i < paths->len; i++) idtable_insert(&set.table, hash_path(paths->items[i]), (uint32_t)i, rehash_entry, &set);
    int added = add_unique(&set, paths, path);
    int64_t deadline = monotonic_ms() + BURST_LIMIT_MS;
    while (monotonic_ms() < deadline) {
        rc = backend_next(w->backend, SETTLE_MS, &path, err);
        if (rc < 0) {
            added = -1;
            break;
        }
        if (rc == 0) break;
        added += add_unique(&set, paths, path);
    }
    idtable_free(&set.table);
    return added;
}

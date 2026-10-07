#include "scanner.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/arena.h"
#include "../core/queue.h"
#include "../core/threadpool.h"

#define PROGRESS_EVERY 10000

typedef struct {
    const char *name;
    size_t len;
    struct stat st;
} Child;

/* DirScan is one directory's work item; the worker fills in its children. */
typedef struct {
    char *path;
    uint32_t id;
    const Excludes *ex;
    Queue *results;
    Arena arena; /* the children's names, dropped once the collector has copied them */
    Child *children;
    size_t count, cap;
    bool failed;
} DirScan;

uint32_t scan_add_record(SegmentBuilder *b, const char *name, size_t len, uint32_t parent, const struct stat *st) {
    bool dir = S_ISDIR(st->st_mode);
    return builder_add(b, name, len, parent, dir ? 0 : (int64_t)st->st_size, (int64_t)st->st_mtime,
                       stat_birthtime(st), dir ? RECORD_DIR : 0);
}

static bool skip_excluded(void *ctx, const char *name) {
    DirScan *d = ctx;
    if (!d->ex || excludes_empty(d->ex)) return false;
    StrBuf path = {0};
    sb_puts(&path, d->path);
    if (path.len == 0 || path.data[path.len - 1] != '/') sb_putc(&path, '/');
    sb_puts(&path, name);
    bool excluded = excludes_match(d->ex, path.data, name);
    sb_free(&path);
    return excluded;
}

static void add_child(void *ctx, const char *name, const struct stat *st) {
    DirScan *d = ctx;
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->children = xrealloc(d->children, d->cap * sizeof *d->children);
    }
    size_t len = strlen(name);
    d->children[d->count++] = (Child){.name = arena_strndup(&d->arena, name, len), .len = len, .st = *st};
}

static void read_directory(void *arg) {
    DirScan *d = arg;
    int fd = open(d->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    d->failed = fd < 0 || !fs_read_dir(fd, skip_excluded, add_child, d);
    if (fd >= 0) close(fd);
    queue_push(d->results, d);
}

static DirScan *new_scan(char *path, uint32_t id, const Excludes *ex, Queue *results) {
    DirScan *d = xcalloc(1, sizeof *d);
    d->path = path;
    d->id = id;
    d->ex = ex;
    d->results = results;
    arena_init(&d->arena, 16 * 1024);
    return d;
}

static void free_scan(DirScan *d) {
    free(d->path);
    free(d->children);
    arena_free(&d->arena);
    free(d);
}

bool scan_tree(ThreadPool *pool, SegmentBuilder *b, const char *root, uint32_t parent, const Excludes *ex,
               ScanProgress progress, void *progress_ctx, ScanResult *result, Err *err) {
    *result = (ScanResult){.first_id = b->base_id + b->count};
    struct stat st;
    if (lstat(root, &st) != 0) {
        err_set(err, "%s: %s", root, strerror(errno));
        return false;
    }
    const char *name = parent == NO_PARENT ? root : path_base(root);
    uint32_t root_id = scan_add_record(b, name, strlen(name), parent, &st);
    if (!S_ISDIR(st.st_mode)) {
        result->end_id = root_id + 1;
        return true;
    }

    Queue results;
    queue_init(&results);
    threadpool_submit(pool, read_directory, new_scan(xstrdup(root), root_id, ex, &results));
    size_t inflight = 1;
    uint32_t start_count = b->count, last_reported = 0;
    while (inflight > 0) {
        DirScan *d = queue_pop(&results);
        inflight--;
        if (d->failed) result->errors++;
        for (size_t i = 0; i < d->count; i++) {
            const Child *c = &d->children[i];
            uint32_t id = scan_add_record(b, c->name, c->len, d->id, &c->st);
            if (S_ISDIR(c->st.st_mode)) {
                threadpool_submit(pool, read_directory, new_scan(path_join(d->path, c->name), id, ex, &results));
                inflight++;
            }
        }
        free_scan(d);
        uint32_t added = b->count - start_count;
        if (progress && added - last_reported >= PROGRESS_EVERY) {
            last_reported = added;
            progress(progress_ctx, added);
        }
    }
    queue_destroy(&results);
    result->end_id = b->base_id + b->count;
    return true;
}

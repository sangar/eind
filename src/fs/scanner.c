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

/*
 * A ReadBuf holds one directory's children from the moment a worker lists it
 * until the collector has copied them out. A handful circulate through a
 * pool, since only the directories being listed and the collector's backlog
 * need one at a time, however many are queued.
 */
typedef struct {
    Arena arena; /* the children's names, reset when the buffer returns to the pool */
    Child *children;
    size_t count, cap;
    StrBuf scratch; /* a child's path, for the exclude patterns */
} ReadBuf;

/*
 * DirScan is one queued directory. The collector keeps finished items and
 * reuses them, so a scan allocates one per directory only while the queue
 * grows to the tree's widest level.
 */
typedef struct DirScan {
    Task task;
    StrBuf path;
    uint32_t id;
    const Excludes *ex;
    Queue *results, *bufs;
    ReadBuf *buf; /* while listing, until collected */
    bool failed;
    struct DirScan *spare; /* the next unused item */
} DirScan;

uint32_t scan_add_record(SegmentBuilder *b, const char *name, size_t len, uint32_t parent, const struct stat *st) {
    bool dir = S_ISDIR(st->st_mode);
    return builder_add(b, name, len, parent, dir ? 0 : (int64_t)st->st_size, (int64_t)st->st_mtime,
                       stat_birthtime(st), dir ? RECORD_DIR : 0);
}

static void join_into(StrBuf *out, const char *dir, const char *name) {
    sb_clear(out);
    sb_puts(out, dir);
    if (out->len == 0 || out->data[out->len - 1] != '/') sb_putc(out, '/');
    sb_puts(out, name);
}

/* ---- read buffers ---- */

static ReadBuf *take_buf(Queue *bufs) {
    void *item;
    if (queue_pop_timeout(bufs, 0, &item)) return item;
    ReadBuf *b = xcalloc(1, sizeof *b);
    arena_init(&b->arena, 16 * 1024);
    return b;
}

static void return_buf(Queue *bufs, ReadBuf *b) {
    arena_reset(&b->arena);
    b->count = 0;
    queue_push(bufs, b);
}

static void free_buf(ReadBuf *b) {
    arena_free(&b->arena);
    free(b->children);
    sb_free(&b->scratch);
    free(b);
}

/* ---- listing, on a worker ---- */

static bool skip_excluded(void *ctx, const char *name) {
    DirScan *d = ctx;
    if (!d->ex || excludes_empty(d->ex)) return false;
    join_into(&d->buf->scratch, d->path.data, name);
    return excludes_match(d->ex, d->buf->scratch.data, name);
}

static void add_child(void *ctx, const char *name, const struct stat *st) {
    ReadBuf *b = ((DirScan *)ctx)->buf;
    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->children = xrealloc(b->children, b->cap * sizeof *b->children);
    }
    size_t len = strlen(name);
    b->children[b->count++] = (Child){.name = arena_strndup(&b->arena, name, len), .len = len, .st = *st};
}

static void read_directory(void *arg) {
    DirScan *d = arg;
    d->buf = take_buf(d->bufs);
    int fd = open(d->path.data, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    d->failed = fd < 0 || !fs_read_dir(fd, skip_excluded, add_child, d);
    if (fd >= 0) close(fd);
    queue_push(d->results, d);
}

/* ---- work items, on the collector ---- */

static DirScan *take_scan(DirScan **spares, const char *dir, const char *name, uint32_t id, const Excludes *ex,
                          Queue *results, Queue *bufs) {
    DirScan *d = *spares;
    if (d) *spares = d->spare;
    else d = xcalloc(1, sizeof *d);
    if (name) {
        join_into(&d->path, dir, name);
    } else {
        sb_clear(&d->path);
        sb_puts(&d->path, dir);
    }
    d->task = (Task){read_directory, d};
    d->id = id;
    d->ex = ex;
    d->results = results;
    d->bufs = bufs;
    d->buf = NULL;
    d->failed = false;
    return d;
}

static void return_scan(DirScan **spares, DirScan *d) {
    d->spare = *spares;
    *spares = d;
}

static void free_spares(DirScan *spares) {
    while (spares) {
        DirScan *next = spares->spare;
        sb_free(&spares->path);
        free(spares);
        spares = next;
    }
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

    Queue results, bufs;
    queue_init(&results);
    queue_init(&bufs);
    DirScan *spares = NULL;
    threadpool_submit(pool, &take_scan(&spares, root, NULL, root_id, ex, &results, &bufs)->task);
    size_t inflight = 1;
    uint32_t start_count = b->count, last_reported = 0;
    while (inflight > 0) {
        DirScan *d = queue_pop(&results);
        inflight--;
        if (d->failed) result->errors++;
        for (size_t i = 0; i < d->buf->count; i++) {
            const Child *c = &d->buf->children[i];
            uint32_t id = scan_add_record(b, c->name, c->len, d->id, &c->st);
            if (S_ISDIR(c->st.st_mode)) {
                threadpool_submit(pool, &take_scan(&spares, d->path.data, c->name, id, ex, &results, &bufs)->task);
                inflight++;
            }
        }
        return_buf(&bufs, d->buf);
        return_scan(&spares, d);
        uint32_t added = b->count - start_count;
        if (progress && added - last_reported >= PROGRESS_EVERY) {
            last_reported = added;
            progress(progress_ctx, added);
        }
    }
    void *buf;
    while (queue_pop_timeout(&bufs, 0, &buf)) free_buf(buf);
    queue_destroy(&bufs);
    free_spares(spares);
    queue_destroy(&results);
    result->end_id = b->base_id + b->count;
    return true;
}

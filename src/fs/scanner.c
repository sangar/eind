#include "scanner.h"

#include <string.h>

#include "mc/concurrency/queue.h"
#include "mc/text/path.h"

/*
 * A read buffer starts small, since most directories are, and one that grew
 * past KEEP_BUF_BYTES for a large directory is freed rather than pooled:
 * releasing an arena keeps its blocks, and results can queue up by the
 * thousand while the collector catches up.
 */
enum { PROGRESS_EVERY = 10000, BUF_BLOCK_BYTES = 4096, KEEP_BUF_BYTES = 16 * 1024 };

/*
 * A ReadBuf holds one directory's entries from the moment a worker lists it
 * until the collector has copied them out. A handful circulate through a
 * queue, since only the directories being listed and the collector's backlog
 * need one at a time, however many are queued. Each lives in an arena of its
 * own, released back to the mark after the buffer itself when it returns.
 */
typedef struct {
    Arena *arena;
    ArenaMark empty;
    DirEntryList entries;
} ReadBuf;

/*
 * DirScan is one queued directory. The collector keeps finished items and
 * reuses them, so a scan allocates one per directory only while the queue
 * grows to the tree's widest level. Items and their paths live in the
 * scan's arena, which only the collecting thread touches.
 */
typedef struct DirScan {
    Task task;
    char *path;
    size_t path_cap;
    uint32_t id;
    const Excludes *ex;
    Queue *results, *bufs;
    ReadBuf *buf; /* while listing, until collected */
    bool failed;
    struct DirScan *spare; /* the next unused item */
} DirScan;

int64_t seconds_of(int64_t nanoseconds) {
    int64_t seconds = nanoseconds / NS_PER_SECOND;
    return nanoseconds % NS_PER_SECOND < 0 ? seconds - 1 : seconds;
}

uint32_t scan_add_record(SegmentBuilder *b, String name, uint32_t parent, const FileInfo *info) {
    return builder_add(b, name, parent, info->is_dir ? 0 : info->size, seconds_of(info->mtime_ns),
                       seconds_of(info->birth_ns), info->is_dir ? RECORD_DIR : 0);
}

/* ---- read buffers ---- */

static ReadBuf *take_buf(Queue *bufs) {
    void *item;
    if (queue_pop_timeout(bufs, 0, &item)) return item;
    Arena *arena = arena_create(BUF_BLOCK_BYTES);
    ReadBuf *b = arena_push(arena, sizeof *b);
    b->arena = arena;
    b->empty = arena_mark(arena);
    return b;
}

static void return_buf(Queue *bufs, ReadBuf *b) {
    if (arena_bytes_used(b->arena) > KEEP_BUF_BYTES) {
        arena_destroy(b->arena);
        return;
    }
    arena_release(b->empty);
    b->entries = (DirEntryList){0};
    /* The buffer queue stays open for the whole scan. */
    (void)queue_push(bufs, b);
}

/* ---- listing, on a worker ---- */

/* drop_excluded removes the entries the exclude patterns reject, keeping the order of the rest. */
static void drop_excluded(DirScan *d) {
    if (!d->ex || excludes_empty(d->ex)) return;
    DirEntryList *entries = &d->buf->entries;
    ArenaMark mark = arena_mark(d->buf->arena);
    size_t kept = 0;
    for (size_t i = 0; i < entries->count; i++) {
        String path = path_join(d->buf->arena, S(d->path), entries->items[i].name);
        if (!excludes_match(d->ex, path, entries->items[i].name)) entries->items[kept++] = entries->items[i];
    }
    entries->count = kept;
    arena_release(mark);
}

static void read_directory(void *argument) {
    DirScan *d = argument;
    d->buf = take_buf(d->bufs);
    d->failed = dir_read(d->buf->arena, S(d->path), &d->buf->entries, nullptr) != ERR_OK;
    drop_excluded(d);
    /* The results queue stays open until every directory has come back. */
    (void)queue_push(d->results, d);
}

/* ---- work items, on the collector ---- */

static void set_path(Arena *arena, DirScan *d, String path) {
    if (path.len + 1 > d->path_cap) {
        d->path_cap = max_size(path.len + 1, 2 * d->path_cap);
        d->path = arena_push(arena, d->path_cap);
    }
    memcpy(d->path, path.data, path.len);
    d->path[path.len] = '\0';
}

typedef struct {
    Arena *arena;
    DirScan *spares;
    const Excludes *ex;
    Queue *results, *bufs;
} Collector;

static DirScan *take_scan(Collector *c, String path, uint32_t id) {
    DirScan *d = c->spares;
    if (d) {
        c->spares = d->spare;
    } else {
        d = arena_push(c->arena, sizeof *d);
    }
    set_path(c->arena, d, path);
    d->task = (Task){read_directory, d};
    d->id = id;
    d->ex = c->ex;
    d->results = c->results;
    d->bufs = c->bufs;
    d->buf = nullptr;
    d->failed = false;
    return d;
}

static void return_scan(Collector *c, DirScan *d) {
    d->spare = c->spares;
    c->spares = d;
}

Error scan_tree(ThreadPool *pool, SegmentBuilder *b, String root, uint32_t parent, const Excludes *ex,
                ScanProgress progress, void *progress_context, ScanResult *result, Err *err) {
    *result = (ScanResult){.first_id = b->base_id + b->count};
    FileInfo info;
    Error e = file_info(root, &info, err);
    if (e != ERR_OK) return e;
    if (!info.exists) return err_set(err, ERR_NOT_FOUND, "%.*s: no such file or directory", (int)root.len, root.data);
    String name = parent == NO_PARENT ? root : path_base(root);
    uint32_t root_id = scan_add_record(b, name, parent, &info);
    if (!info.is_dir) {
        result->end_id = root_id + 1;
        return ERR_OK;
    }

    Collector c = {.arena = arena_create(0), .ex = ex, .results = queue_create(), .bufs = queue_create()};
    StringBuilder child = str_builder_create(c.arena, 256);
    threadpool_submit(pool, &take_scan(&c, root, root_id)->task);
    size_t inflight = 1;
    uint32_t start_count = b->count, last_reported = 0;
    while (inflight > 0) {
        DirScan *d = queue_pop(c.results);
        inflight--;
        if (d->failed) result->errors++;
        for (size_t i = 0; i < d->buf->entries.count; i++) {
            const DirEntry *entry = &d->buf->entries.items[i];
            uint32_t id = scan_add_record(b, entry->name, d->id, &entry->info);
            if (entry->info.is_dir) {
                child.len = 0;
                str_builder_append(&child, S(d->path));
                if (child.len == 0 || child.data[child.len - 1] != '/') str_builder_append_char(&child, '/');
                str_builder_append(&child, entry->name);
                threadpool_submit(pool, &take_scan(&c, (String){child.data, child.len}, id)->task);
                inflight++;
            }
        }
        return_buf(c.bufs, d->buf);
        return_scan(&c, d);
        uint32_t added = b->count - start_count;
        if (progress && added - last_reported >= PROGRESS_EVERY) {
            last_reported = added;
            progress(progress_context, added);
        }
    }
    void *buf;
    while (queue_pop_timeout(c.bufs, 0, &buf)) arena_destroy(((ReadBuf *)buf)->arena);
    queue_destroy(c.bufs);
    queue_destroy(c.results);
    arena_destroy(c.arena);
    result->end_id = b->base_id + b->count;
    return ERR_OK;
}

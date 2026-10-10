#include "build.h"

#include <stdio.h>
#include <time.h>

#include "mc/concurrency/threadpool.h"
#include "mc/platform/platform.h"
#include "mc/platform/terminal.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"
#include "../fs/scanner.h"

/* At most this many threads read directories, since reads contend on filesystem locks in the kernel. */
enum { MAX_IO_THREADS = 6 };

/*
 * Indexing 150k entries on APFS took 1.0s with 64 threads but 0.37s with 5
 * or 6, while kernel time fell from 10s to 1.3s. A few threads beat many.
 */
size_t io_thread_count(void) { return min_size(thread_cpu_count(), MAX_IO_THREADS); }

String format_elapsed(Arena *arena, int64_t nanoseconds) {
    int64_t milliseconds = (nanoseconds + NS_PER_MILLISECOND / 2) / NS_PER_MILLISECOND;
    if (milliseconds < 1000) return str_format(arena, "%lldms", (long long)milliseconds);
    int64_t unit = 10 * NS_PER_MILLISECOND;
    return fmt_duration(arena, (nanoseconds + unit / 2) / unit * unit);
}

static void show_progress(void *context, uint32_t added) {
    unused(added);
    const SegmentBuilder *b = context;
    Arena *scratch = arena_create(256);
    String count = fmt_thousands(scratch, b->count);
    fprintf(stderr, "\r  %.*s entries...", (int)count.len, count.data);
    arena_destroy(scratch);
}

Error build_index(const Config *cfg, String index_path, Snapshot **snapshot, Err *err) {
    *snapshot = nullptr;
    Arena *arena = arena_create(0);
    Excludes ex;
    Error e = excludes_init(arena, &ex, cfg->excludes, env_home(arena), err);
    ThreadPool *io = nullptr;
    if (e == ERR_OK) e = threadpool_create(io_thread_count(), &io, err);
    if (e != ERR_OK) {
        arena_destroy(arena);
        return e;
    }
    int64_t start = clock_monotonic_ns();
    SegmentBuilder b;
    builder_init(&b, 0);
    StringList roots = {0};
    uint32_t unreadable = 0;
    bool tty = terminal_is_terminal(2);
    for (size_t i = 0; i < cfg->roots.count; i++) {
        String root = path_absolute(arena, path_expand_home(arena, cfg->roots.items[i], env_home(arena)));
        ScanResult res;
        Err why;
        if (scan_tree(io, &b, root, NO_PARENT, &ex, tty ? show_progress : nullptr, &b, &res, &why) != ERR_OK) {
            fprintf(stderr, "skipping %.*s: %s\n", (int)root.len, root.data, why.msg);
            continue;
        }
        strlist_push(arena, &roots, root);
        unreadable += res.errors;
    }
    threadpool_destroy(io);
    if (roots.count == 0) {
        builder_free(&b);
        arena_destroy(arena);
        return err_set(err, ERR_NOT_FOUND, "none of the configured roots could be read");
    }
    e = segment_write(index_path, &b, roots, (int64_t)time(nullptr), err);
    uint32_t dirs = 0;
    for (uint32_t i = 0; i < b.count; i++) dirs += (b.recs[i].flags & RECORD_DIR) != 0;
    uint32_t files = b.count - dirs;
    builder_free(&b);
    if (e == ERR_OK) {
        String nfiles = fmt_thousands(arena, files), ndirs = fmt_thousands(arena, dirs);
        String took = format_elapsed(arena, clock_monotonic_ns() - start);
        if (tty) fputs("\r\x1b[K", stderr);
        fprintf(stderr, "Indexed %.*s files and %.*s folders in %.*s", (int)nfiles.len, nfiles.data, (int)ndirs.len,
                ndirs.data, (int)took.len, took.data);
        if (unreadable) fprintf(stderr, " (%u folders unreadable)", unreadable);
        FileInfo info;
        if (file_info(index_path, &info, nullptr) == ERR_OK && info.exists) {
            String size = fmt_bytes(arena, info.size);
            fprintf(stderr, "; index is %.*s at %.*s", (int)size.len, size.data, (int)index_path.len, index_path.data);
        }
        fputc('\n', stderr);
        e = index_load(index_path, snapshot, err);
    }
    arena_destroy(arena);
    return e;
}

Error load_or_build(String config_path, String index_path, Snapshot **snapshot, Err *err) {
    Error e = index_load(index_path, snapshot, err);
    if (e != ERR_NOT_FOUND) return e;
    fprintf(stderr, "No index at %.*s yet; building one (run `eind index` to rebuild later).\n", (int)index_path.len,
            index_path.data);
    Arena *arena = arena_create(0);
    Config cfg;
    e = config_load(arena, config_path, &cfg, err);
    if (e == ERR_OK) e = build_index(&cfg, index_path, snapshot, err);
    arena_destroy(arena);
    return e;
}

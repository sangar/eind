#include "daemon.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "mc/concurrency/cancel.h"
#include "mc/concurrency/threadpool.h"
#include "mc/platform/platform.h"
#include "mc/platform/watch.h"
#include "mc/text/path.h"
#include "../fs/excludes.h"
#include "../fs/updater.h"
#include "../index/index.h"
#include "../index/journal.h"
#include "build.h"
#include "config.h"
#include "server.h"

/* Events are collected until they pause for SETTLE_MS; the loop wakes at least every POLL_MS. */
enum { POLL_MS = 500, SETTLE_MS = 250 };

[[gnu::format(printf, 1, 2)]] static void log_line(const char *fmt, ...) {
    char stamp[16];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);
    fprintf(stderr, "%s ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* wait_for_stop turns the first stop or reload signal into a cancelled run. */
static void *wait_for_stop(void *argument) {
    signals_wait();
    cancel_request(argument);
    return nullptr;
}

/* Daemon is what the loop works on; it lives on daemon_run's stack. */
typedef struct {
    Index ix;
    Updater *updater;
    Journal *journal;
    String index_path;
} Daemon;

/* start_over makes a freshly loaded snapshot, and its journal, the current state; it takes the reference to s. */
static void start_over(Daemon *d, Snapshot *s, Journal *journal) {
    journal_free(d->journal);
    d->journal = journal;
    snapshot_retain(s);
    index_publish(&d->ix, s);
    updater_reset(d->updater, s);
}

/* compact_and_save merges the deltas into a fresh index file, starts the updater over from it and starts its journal. */
[[nodiscard]] static Error compact_and_save(Daemon *d, Err *err) {
    size_t changes = journal_entries(d->journal);
    Snapshot *merged;
    Error e = index_compact(updater_snapshot(d->updater), d->index_path, &merged, err);
    if (e != ERR_OK) return e;
    Journal *fresh;
    e = journal_open(d->index_path, merged, &fresh, err);
    if (e != ERR_OK) {
        snapshot_release(merged);
        return e;
    }
    start_over(d, merged, fresh);
    log_line("folded %zu changes into the index: %u entries", changes, snap_live_count(merged));
    return ERR_OK;
}

/* reload starts over from an index file another process wrote. */
[[nodiscard]] static Error reload(Daemon *d, Err *err) {
    log_line("the index file was replaced by another process; loading it again");
    Snapshot *fresh;
    Error e = index_load(d->index_path, &fresh, err);
    if (e != ERR_OK) return e;
    Journal *fresh_journal;
    e = journal_open(d->index_path, fresh, &fresh_journal, err);
    if (e != ERR_OK) {
        snapshot_release(fresh);
        return e;
    }
    start_over(d, fresh, fresh_journal);
    return ERR_OK;
}

/*
 * compact_after is how many journaled changes make a new index file worth
 * writing. Each command that loads the index replays the journal, which costs
 * about a millisecond per ten thousand changes.
 */
static size_t compact_after(const Snapshot *s) { return max_size(10000, snap_live_count(s) / 100); }

/* apply_events reconciles one batch, journals it at once so commands loading the index see it, and flushes. */
[[nodiscard]] static Error apply_events(Daemon *d, const WatchEventList *events, Err *err) {
    Snapshot *before = updater_snapshot(d->updater);
    snapshot_retain(before);
    if (updater_apply(d->updater, events->items, events->count))
        journal_record(d->journal, before, updater_snapshot(d->updater));
    snapshot_release(before);
    bool replaced;
    Error e = journal_flush(d->journal, &replaced, err);
    return e == ERR_OK && replaced ? reload(d, err) : e;
}

typedef struct {
    const Excludes *ex;
} WatchFilter;

static bool skip_excluded(void *context, String directory) {
    const WatchFilter *filter = context;
    return excludes_match(filter->ex, directory, path_base(directory));
}

[[nodiscard]] static Error watch_loop(Daemon *d, Watch *watch, Cancel *stop, int64_t save_interval_ms, Err *err) {
    Arena *batch = arena_create(0);
    int64_t next_save = clock_monotonic_ns() + save_interval_ms * NS_PER_MILLISECOND;
    Error e = ERR_OK;
    while (e == ERR_OK && !cancel_requested(stop)) {
        arena_reset(batch);
        int64_t until_save = (next_save - clock_monotonic_ns()) / NS_PER_MILLISECOND;
        int wait = until_save < 0 ? 0 : until_save < POLL_MS ? (int)until_save : POLL_MS;
        WatchEventList events = {0};
        e = watch_read(watch, batch, wait, SETTLE_MS, &events, err);
        if (e == ERR_OK && events.count > 0) e = apply_events(d, &events, err);
        if (e == ERR_OK && clock_monotonic_ns() >= next_save) {
            next_save = clock_monotonic_ns() + save_interval_ms * NS_PER_MILLISECOND;
            if (journal_entries(d->journal) >= compact_after(updater_snapshot(d->updater))) e = compact_and_save(d, err);
        }
    }
    if (e == ERR_OK && journal_entries(d->journal) > 0) e = compact_and_save(d, err);
    arena_destroy(batch);
    return e;
}

/* watch_and_update runs the loop with everything it needs, then takes it all down again. */
[[nodiscard]] static Error watch_and_update(Daemon *d, const Excludes *ex, Cancel *stop, int64_t save_interval_ms,
                                            Err *err) {
    Snapshot *initial = index_acquire(&d->ix);
    WatchFilter filter = {ex};
    Watch *watch = nullptr;
    ThreadPool *io = nullptr;
    Error e = watch_create(initial->roots, skip_excluded, &filter, &watch, err);
    if (e == ERR_OK) e = journal_open(d->index_path, initial, &d->journal, err);
    if (e == ERR_OK) {
        for (size_t i = 0; i < initial->roots.count; i++)
            fprintf(stderr, "watching %.*s\n", (int)initial->roots.items[i].len, initial->roots.items[i].data);
    }
    snapshot_release(initial);
    if (e == ERR_OK) e = threadpool_create(io_thread_count(), &io, err);
    if (e == ERR_OK) {
        d->updater = updater_create(io, &d->ix, ex);
        e = watch_loop(d, watch, stop, save_interval_ms, err);
        updater_destroy(d->updater);
    }
    if (io) threadpool_destroy(io);
    journal_free(d->journal);
    watch_destroy(watch);
    return e;
}

Error daemon_run(const DaemonOptions *o, Err *err) {
    Arena *arena = arena_create(0);
    Config cfg;
    Excludes ex;
    Error e = config_load(arena, o->config_path, &cfg, err);
    if (e == ERR_OK) e = excludes_init(arena, &ex, cfg.excludes, env_home(arena), err);
    Snapshot *initial = nullptr;
    if (e == ERR_OK) e = load_or_build(o->config_path, o->index_path, &initial, err);
    if (e != ERR_OK) {
        arena_destroy(arena);
        return e;
    }
    Daemon d = {.index_path = o->index_path};
    index_init(&d.ix, initial);

    /* From here on threads start; a signal now only ends the loop, which then saves the index. */
    signals_block();
    Arena *stop_arena = arena_create(sizeof(Cancel) + 64);
    Cancel *stop = arena_push(stop_arena, sizeof *stop);
    cancel_init(stop);
    Thread signal_thread;
    e = thread_start(&signal_thread, wait_for_stop, stop, err);
    bool waiting_for_signal = e == ERR_OK;

    ThreadPool *cpu = nullptr;
    Server *srv = nullptr;
    if (e == ERR_OK && o->serve) {
        e = threadpool_create(0, &cpu, err);
        if (e == ERR_OK) e = server_start(cpu, &d.ix, o->socket_path, o->index_path, &srv, err);
        if (e == ERR_OK) fprintf(stderr, "serving queries at %.*s\n", (int)o->socket_path.len, o->socket_path.data);
    }
    if (e == ERR_OK) {
        fprintf(stderr, "Tip: `eind service enable` keeps it running in the background from login.\n");
        e = watch_and_update(&d, &ex, stop, o->save_interval_ms, err);
    }
    if (srv) server_stop(srv);
    if (cpu) threadpool_destroy(cpu);
    index_destroy(&d.ix);
    arena_destroy(arena);
    if (waiting_for_signal && !cancel_requested(stop)) {
        /* Nothing can wake the thread still waiting for a signal, so it keeps stop for as long as the process lives. */
        thread_detach(&signal_thread);
        return e;
    }
    if (waiting_for_signal) thread_join(&signal_thread);
    cancel_destroy(stop);
    arena_destroy(stop_arena);
    return e;
}

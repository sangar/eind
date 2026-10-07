#include "daemon.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../core/threadpool.h"
#include "../fs/fs.h"
#include "../fs/updater.h"
#include "../fs/watcher.h"
#include "../index/index.h"
#include "../index/journal.h"
#include "build.h"
#include "config.h"
#include "server.h"

#define POLL_MS 500

static volatile sig_atomic_t stop_requested; // a signal handler can only set a flag; modern-c: allow global-mutable

static void on_stop(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_line(const char *fmt, ...) {
    char stamp[16];
    time_t now = time(NULL);
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

static void watch_dirs(Watcher *w, const Snapshot *s) {
    StrBuf path = {0};
    for (uint32_t id = 0; id < s->total; id++) {
        if (!snap_live(s, id) || !record_is_dir(snap_record(s, id))) continue;
        snap_path(s, id, &path);
        watcher_add_dir(w, path.data);
    }
    sb_free(&path);
}

/*
 * compact_and_save merges the deltas into a fresh index file, starts the
 * updater over from it and starts its journal.
 */
static bool compact_and_save(Index *ix, Updater *u, Journal **j, const char *index_path, Err *err) {
    size_t changes = journal_entries(*j);
    Snapshot *merged = index_compact(updater_snapshot(u), index_path, err);
    if (!merged) return false;
    Journal *fresh = journal_open(index_path, merged, err);
    if (!fresh) {
        snapshot_release(merged);
        return false;
    }
    journal_free(*j);
    *j = fresh;
    snapshot_retain(merged);
    index_publish(ix, merged);
    updater_reset(u, merged);
    log_line("folded %zu changes into the index: %u entries", changes, snap_live_count(merged));
    return true;
}

/* reload starts over from an index file another process wrote. */
static bool reload(Index *ix, Updater *u, Journal **j, const char *index_path, Err *err) {
    log_line("the index file was replaced by another process; loading it again");
    bool missing;
    Snapshot *fresh = index_load(index_path, &missing, err);
    if (!fresh) return false;
    Journal *fresh_journal = journal_open(index_path, fresh, err);
    if (!fresh_journal) {
        snapshot_release(fresh);
        return false;
    }
    journal_free(*j);
    *j = fresh_journal;
    snapshot_retain(fresh);
    index_publish(ix, fresh);
    updater_reset(u, fresh);
    return true;
}

/*
 * compact_after is how many journaled changes make a new index file worth
 * writing. Each command that loads the index replays the journal, which costs
 * about a millisecond per ten thousand changes.
 */
static size_t compact_after(const Snapshot *s) { return max_size(10000, snap_live_count(s) / 100); }

bool daemon_run(const DaemonOptions *o, Err *err) {
    Config cfg;
    if (!config_load(o->config_path, &cfg, err)) return false;
    Excludes ex;
    bool ok = excludes_init(&ex, &cfg.excludes, err);
    config_free(&cfg);
    if (!ok) return false;
    Snapshot *initial = load_or_build(o->config_path, o->index_path, err);
    if (!initial) {
        excludes_free(&ex);
        return false;
    }
    Index ix;
    index_init(&ix, initial);

    struct sigaction sa = {.sa_handler = on_stop};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    ThreadPool *cpu = NULL;
    Server *srv = NULL;
    if (o->serve) {
        cpu = threadpool_create(cpu_count());
        srv = server_start(cpu, &ix, o->socket_path, o->index_path, err);
        if (!srv) {
            threadpool_destroy(cpu);
            index_destroy(&ix);
            excludes_free(&ex);
            return false;
        }
        fprintf(stderr, "serving queries at %s\n", o->socket_path);
    }
    fprintf(stderr, "Tip: `eind service enable` keeps it running in the background from login.\n");

    Watcher *w = watcher_open(&initial->roots, err);
    Journal *j = w ? journal_open(o->index_path, initial, err) : NULL;
    ThreadPool *io = threadpool_create(io_thread_count());
    Updater *u = j ? updater_new(io, &ix, &ex) : NULL;
    if (w) {
        for (size_t i = 0; i < initial->roots.len; i++) fprintf(stderr, "watching %s\n", initial->roots.items[i]);
        if (watcher_needs_dirs()) watch_dirs(w, initial);
    }
    ok = u != NULL;
    int64_t next_save = monotonic_ms() + o->save_interval_ms;
    StrList changed = {0}, new_dirs = {0};
    while (ok && !stop_requested) {
        int wait = (int)min_i64(POLL_MS, max_i64(next_save - monotonic_ms(), 0));
        int n = watcher_collect(w, wait, &changed, err);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n > 0) {
            /* Every batch goes to the journal at once, so commands loading the index see it. */
            Snapshot *before = updater_snapshot(u);
            snapshot_retain(before);
            if (updater_apply(u, &changed, &new_dirs)) journal_record(j, before, updater_snapshot(u));
            snapshot_release(before);
            for (size_t i = 0; i < new_dirs.len; i++) watcher_add_dir(w, new_dirs.items[i]);
            strlist_clear(&changed);
            strlist_clear(&new_dirs);
            JournalStatus st = journal_flush(j, err);
            if (st == JOURNAL_FAILED) ok = false;
            if (st == JOURNAL_REPLACED) ok = reload(&ix, u, &j, o->index_path, err);
        }
        if (ok && monotonic_ms() >= next_save) {
            next_save = monotonic_ms() + o->save_interval_ms;
            if (journal_entries(j) >= compact_after(updater_snapshot(u)))
                ok = compact_and_save(&ix, u, &j, o->index_path, err);
        }
    }
    if (ok && journal_entries(j) > 0) ok = compact_and_save(&ix, u, &j, o->index_path, err);
    strlist_free(&changed);
    strlist_free(&new_dirs);
    if (srv) server_stop(srv);
    if (u) updater_free(u);
    if (cpu) threadpool_destroy(cpu);
    threadpool_destroy(io);
    journal_free(j);
    watcher_close(w);
    index_destroy(&ix);
    excludes_free(&ex);
    return ok;
}

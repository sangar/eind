#ifndef EIND_INDEX_H
#define EIND_INDEX_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../core/util.h"
#include "segment.h"

/*
 * A Snapshot is a consistent, read-only view of the index: the base segment,
 * the delta segments added since, and a tombstone bitmap over all file ids.
 * File ids run contiguously across segments, and a parent always has a
 * smaller id than its children, so paths are rebuilt by walking up parents.
 * Searches hold a snapshot while the watcher publishes newer ones.
 */
typedef struct Snapshot {
    atomic_int refs;
    Segment **segs;
    uint32_t seg_count;
    uint32_t total; /* ids 0..total-1 exist, dead or alive */
    uint64_t *dead; /* tombstone bitmap */
    uint32_t dead_count;
    int64_t built_at;
    StrList roots;
} Snapshot;

/* snapshot_new takes the caller's reference to base. */
Snapshot *snapshot_new(Segment *base, const StrList *roots, int64_t built_at);
/* snapshot_derive adds an optional delta segment (taking its reference) and replaces the tombstones. */
Snapshot *snapshot_derive(const Snapshot *from, Segment *delta, uint64_t *dead, uint32_t dead_count);
void snapshot_retain(Snapshot *s);
void snapshot_release(Snapshot *s);

static inline const Segment *snap_segment(const Snapshot *s, uint32_t id) {
    if (id < s->segs[0]->count) return s->segs[0];
    for (uint32_t k = s->seg_count; k-- > 1;)
        if (id >= s->segs[k]->base_id) return s->segs[k];
    return s->segs[0];
}

static inline const FileRecord *snap_record(const Snapshot *s, uint32_t id) {
    const Segment *seg = snap_segment(s, id);
    return &seg->recs[id - seg->base_id];
}

static inline const char *snap_name(const Snapshot *s, uint32_t id) {
    const Segment *seg = snap_segment(s, id);
    return seg->names + seg->recs[id - seg->base_id].name_off;
}

static inline const char *snap_lower(const Snapshot *s, uint32_t id) {
    const Segment *seg = snap_segment(s, id);
    return seg->lower + seg->recs[id - seg->base_id].name_off;
}

static inline bool bitmap_test(const uint64_t *bits, uint32_t id) { return bits[id >> 6] >> (id & 63) & 1; }
static inline void bitmap_set(uint64_t *bits, uint32_t id) { bits[id >> 6] |= (uint64_t)1 << (id & 63); }
static inline size_t bitmap_words(uint32_t n) { return ((size_t)n + 63) / 64; }

static inline bool snap_live(const Snapshot *s, uint32_t id) { return !bitmap_test(s->dead, id); }
static inline bool record_is_dir(const FileRecord *r) { return r->flags & RECORD_DIR; }

/* snap_path writes the absolute path of id into sb, replacing its contents. */
void snap_path(const Snapshot *s, uint32_t id, StrBuf *sb);
/* snap_ext returns the lowercased extension without the dot, or "". */
const char *snap_ext(const Snapshot *s, uint32_t id, size_t *len);
void snap_stats(const Snapshot *s, int64_t *files, int64_t *dirs);
uint32_t snap_live_count(const Snapshot *s);

/* Index publishes snapshots: readers acquire the current one, writers replace it. */
typedef struct {
    pthread_mutex_t mu;
    Snapshot *current;
} Index;

void index_init(Index *ix, Snapshot *initial);
void index_destroy(Index *ix);
Snapshot *index_acquire(Index *ix);
void index_publish(Index *ix, Snapshot *next);

/* index_load maps the index file and replays its journal; *missing is set when there is none yet. */
Snapshot *index_load(const char *path, bool *missing, Err *err);
/*
 * index_compact merges every segment into one, drops tombstoned records and
 * everything below them, writes the result to path and returns a snapshot
 * of the written file. File ids are not stable across a compaction.
 */
Snapshot *index_compact(const Snapshot *s, const char *path, Err *err);

#endif

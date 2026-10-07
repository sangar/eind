#ifndef EIND_SEGMENT_H
#define EIND_SEGMENT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../core/util.h"

/* A root record has no parent; its name is the absolute root path. */
#define NO_PARENT UINT32_MAX

enum { RECORD_DIR = 1 };

/*
 * FileRecord is one record as a value: the delta segments store an array of
 * them, and the accessors below assemble one from the base segment's
 * columns. Names live in the segment's string table, NUL-terminated.
 */
typedef struct {
    uint32_t parent;
    uint32_t name_off;
    uint32_t name_len;
    uint32_t flags;
    int64_t size;
    int64_t mtime;
    int64_t ctime; /* 0 when the platform does not report birth time */
} FileRecord;

/* The base segment's columns, pointing into the mapped index file (docs/file-format.md). */
typedef struct {
    const uint32_t *name_off; /* per distinct name: offset in names */
    uint32_t distinct;
    size_t names_len;
    const uint32_t *name_id, *parent, *modified, *created;
    const int64_t *size;
    const uint64_t *dirs;
} Columns;

/*
 * A Segment is immutable once built. The base segment reads the mapped
 * index file in place, so a command touches only the columns its query
 * needs; delta segments hold the watcher's additions in memory as records.
 * Searches scan every segment in parallel.
 */
typedef struct Segment {
    atomic_int refs;
    uint32_t base_id;
    uint32_t count;
    uint64_t generation; /* of the index file; its journal names the same one */
    const char *names;
    const FileRecord *recs; /* delta segments only; NULL for the base */
    Columns col;            /* base segment only */
    /*
     * Journal entries that replace a base record's size and times, applied
     * before the segment is published: a bitset over the records plus the
     * new values at the same index. NULL until the first one.
     */
    uint64_t *updated;
    int64_t *updated_size, *updated_mtime, *updated_ctime;
    void *map;
    size_t map_len;
    void *owned_recs, *owned_names;
} Segment;

static inline bool segment_bit(const uint64_t *bits, uint32_t i) { return bits[i >> 6] >> (i & 63) & 1; }

static inline uint32_t seg_parent(const Segment *s, uint32_t i) { return s->recs ? s->recs[i].parent : s->col.parent[i]; }

static inline bool seg_is_dir(const Segment *s, uint32_t i) {
    return s->recs ? (s->recs[i].flags & RECORD_DIR) != 0 : segment_bit(s->col.dirs, i);
}

static inline int64_t seg_size(const Segment *s, uint32_t i) {
    if (s->recs) return s->recs[i].size;
    if (s->updated && segment_bit(s->updated, i)) return s->updated_size[i];
    return s->col.size[i];
}

static inline int64_t seg_mtime(const Segment *s, uint32_t i) {
    if (s->recs) return s->recs[i].mtime;
    if (s->updated && segment_bit(s->updated, i)) return s->updated_mtime[i];
    return s->col.modified[i];
}

static inline int64_t seg_ctime(const Segment *s, uint32_t i) {
    if (s->recs) return s->recs[i].ctime;
    if (s->updated && segment_bit(s->updated, i)) return s->updated_ctime[i];
    return s->col.created[i];
}

static inline const char *seg_name(const Segment *s, uint32_t i) {
    return s->names + (s->recs ? s->recs[i].name_off : s->col.name_off[s->col.name_id[i]]);
}

static inline uint32_t seg_name_len(const Segment *s, uint32_t i) {
    if (s->recs) return s->recs[i].name_len;
    uint32_t id = s->col.name_id[i];
    uint32_t end = id + 1 < s->col.distinct ? s->col.name_off[id + 1] : (uint32_t)s->col.names_len;
    return end - 1 - s->col.name_off[id];
}

/* SegmentBuilder accumulates records in id order before they become a segment. */
typedef struct {
    uint32_t base_id;
    FileRecord *recs;
    uint32_t count, cap;
    char *names;
    size_t names_len, names_cap;
} SegmentBuilder;

void builder_init(SegmentBuilder *b, uint32_t base_id);
void builder_free(SegmentBuilder *b);
/* builder_add appends a record and returns its file id. */
uint32_t builder_add(SegmentBuilder *b, const char *name, size_t len, uint32_t parent, int64_t size,
                     int64_t mtime, int64_t ctime, uint32_t flags);
static inline const char *builder_name(const SegmentBuilder *b, uint32_t id) {
    return b->names + b->recs[id - b->base_id].name_off;
}

/* segment_from_builder takes the builder's buffers; the builder is left empty. */
Segment *segment_from_builder(SegmentBuilder *b);
/* segment_write stores the builder as an index file in the shared format (docs/file-format.md). */
bool segment_write(const char *path, const SegmentBuilder *b, const StrList *roots, int64_t built_at, Err *err);
/* segment_open maps an index file; *missing is set when it does not exist. */
Segment *segment_open(const char *path, StrList *roots, int64_t *built_at, bool *missing, Err *err);
/*
 * segment_set_times replaces a base record's size and times, for the journal
 * to replay before the segment is published; nobody else may call it.
 */
void segment_set_times(Segment *s, uint32_t i, int64_t size, int64_t mtime, int64_t ctime);
void segment_retain(Segment *s);
void segment_release(Segment *s);

#endif

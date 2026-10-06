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
 * FileRecord is fixed size, so a segment's records are one array that can be
 * mapped straight from disk. The record's file id is its position: the
 * segment's base id plus its index. Names live in the segment's string
 * table, NUL-terminated, at name_off; the lowercased table shares offsets.
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

/*
 * A Segment is immutable once built. The base segment is decoded from the
 * mapped index file, with its names left in the mapping; delta segments hold
 * the watcher's additions in memory. Searches scan every segment in parallel.
 */
typedef struct Segment {
    atomic_int refs;
    uint32_t base_id;
    uint32_t count;
    uint64_t generation; /* of the index file; its journal names the same one */
    const FileRecord *recs;
    const char *names;
    const char *lower;
    void *map;
    size_t map_len;
    void *owned_recs, *owned_names, *owned_lower;
} Segment;

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
void segment_retain(Segment *s);
void segment_release(Segment *s);

#endif

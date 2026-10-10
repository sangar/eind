#include <time.h>

#include "index.h"

/*
 * Compaction walks every id in order, so a parent is always decided before
 * its children: a record survives only if it is alive and its parent
 * survived, and survivors are renumbered densely.
 */
Error index_compact(const Snapshot *s, String path, Snapshot **compacted, Err *err) {
    *compacted = nullptr;
    SegmentBuilder b;
    builder_init(&b, 0);
    Arena *scratch = arena_create(0);
    uint32_t *remap = arena_push(scratch, ((size_t)s->total + 1) * sizeof *remap);
    for (uint32_t id = 0; id < s->total; id++) {
        FileRecord r = snap_record(s, id);
        bool dead = !snap_live(s, id) || (r.parent != NO_PARENT && remap[r.parent] == NO_PARENT);
        if (dead) {
            remap[id] = NO_PARENT;
            continue;
        }
        uint32_t parent = r.parent == NO_PARENT ? NO_PARENT : remap[r.parent];
        remap[id] = builder_add(&b, snap_name_view(s, id), parent, r.size, r.mtime, r.ctime, r.flags);
    }
    arena_destroy(scratch);

    int64_t built_at = s->built_at ? s->built_at : (int64_t)time(nullptr);
    Error e = segment_write(path, &b, s->roots, built_at, err);
    builder_free(&b);
    return e == ERR_OK ? index_load(path, compacted, err) : e;
}

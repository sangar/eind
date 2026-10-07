#include <stdlib.h>
#include <time.h>

#include "index.h"

/*
 * Compaction walks every id in order, so a parent is always decided before
 * its children: a record survives only if it is alive and its parent
 * survived, and survivors are renumbered densely.
 */
Snapshot *index_compact(const Snapshot *s, const char *path, Err *err) {
    uint32_t *remap = xmalloc(((size_t)s->total + 1) * sizeof *remap);
    SegmentBuilder b;
    builder_init(&b, 0);
    for (uint32_t id = 0; id < s->total; id++) {
        FileRecord r = snap_record(s, id);
        bool dead = !snap_live(s, id) || (r.parent != NO_PARENT && remap[r.parent] == NO_PARENT);
        if (dead) {
            remap[id] = NO_PARENT;
            continue;
        }
        uint32_t parent = r.parent == NO_PARENT ? NO_PARENT : remap[r.parent];
        remap[id] = builder_add(&b, snap_name(s, id), r.name_len, parent, r.size, r.mtime, r.ctime, r.flags);
    }
    free(remap);

    int64_t built_at = s->built_at ? s->built_at : (int64_t)time(NULL);
    bool ok = segment_write(path, &b, &s->roots, built_at, err);
    builder_free(&b);
    if (!ok) return NULL;
    bool missing;
    return index_load(path, &missing, err);
}

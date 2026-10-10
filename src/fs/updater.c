#include "updater.h"

#include <string.h>

#include "mc/container/hash.h"
#include "mc/container/idtable.h"
#include "mc/text/path.h"
#include "scanner.h"

struct Updater {
    Arena *arena;       /* the updater itself */
    Arena *table_arena; /* the table, rebuilt on reset */
    Arena *batch;       /* what one updater_apply needs, reset by the next */
    Index *ix;
    ThreadPool *io;
    const Excludes *ex;
    Snapshot *snap;
    IdTable *by_parent_name; /* every live record, keyed by (parent id, name) */

    /* state of the batch being applied */
    SegmentBuilder delta;
    uint64_t *dead;
    size_t dead_words;
    uint32_t dead_count;
    bool removed_dir;
};

/* ---- a view over the snapshot plus the delta under construction ---- */

static FileRecord view_record(const Updater *u, uint32_t id) {
    return id < u->snap->total ? snap_record(u->snap, id) : u->delta.recs[id - u->delta.base_id];
}

static bool view_is_dir(const Updater *u, uint32_t id) {
    FileRecord r = view_record(u, id);
    return record_is_dir(&r);
}

static String view_name(const Updater *u, uint32_t id) {
    return id < u->snap->total ? snap_name_view(u->snap, id) : builder_name(&u->delta, id);
}

static uint32_t view_total(const Updater *u) { return u->delta.base_id + u->delta.count; }

static bool is_dead(const Updater *u, uint32_t id) { return bitmap_test(u->dead, id); }

static void ensure_dead_capacity(Updater *u) {
    size_t need = bitmap_words(view_total(u)) + 1;
    if (need <= u->dead_words) return;
    size_t words = max_size(need, u->dead_words * 2);
    uint64_t *dead = arena_push(u->batch, words * sizeof *dead);
    memcpy(dead, u->dead, u->dead_words * sizeof *dead);
    u->dead = dead;
    u->dead_words = words;
}

/* ---- the (parent, name) table ---- */

static uint64_t key_hash(uint32_t parent, String name) { return hash_bytes(name.data, name.len, parent); }

static uint64_t rehash_record(void *context, uint32_t id) {
    const Updater *u = context;
    return key_hash(view_record(u, id).parent, view_name(u, id));
}

typedef struct {
    const Updater *u;
    uint32_t parent;
    String name;
} Key;

static bool key_equal(void *context, uint32_t id) {
    const Key *k = context;
    return view_record(k->u, id).parent == k->parent && str_equal(view_name(k->u, id), k->name);
}

static void table_insert(Updater *u, uint32_t id) { idtable_insert(u->by_parent_name, rehash_record(u, id), id, rehash_record, u); }

static void table_remove(Updater *u, uint32_t id) { idtable_remove(u->by_parent_name, rehash_record(u, id), id); }

static bool find_child(const Updater *u, uint32_t parent, String name, uint32_t *out) {
    Key k = {u, parent, name};
    uint32_t id = idtable_find(u->by_parent_name, key_hash(parent, name), key_equal, &k);
    if (id == IDTABLE_NONE) return false;
    *out = id;
    return true;
}

static void build_table(Updater *u) {
    arena_reset(u->table_arena);
    u->by_parent_name = idtable_create(u->table_arena, snap_live_count(u->snap));
    for (uint32_t id = 0; id < u->snap->total; id++)
        if (snap_live(u->snap, id)) table_insert(u, id);
}

/* ---- lifecycle ---- */

Updater *updater_create(ThreadPool *io, Index *ix, const Excludes *ex) {
    Arena *arena = arena_create(4096);
    Updater *u = arena_push(arena, sizeof *u);
    u->arena = arena;
    u->table_arena = arena_create(0);
    u->batch = arena_create(0);
    u->ix = ix;
    u->io = io;
    u->ex = ex;
    updater_reset(u, index_acquire(ix));
    return u;
}

void updater_reset(Updater *u, Snapshot *s) {
    if (u->snap) snapshot_release(u->snap);
    u->snap = s;
    build_table(u);
}

Snapshot *updater_snapshot(const Updater *u) { return u->snap; }

void updater_destroy(Updater *u) {
    snapshot_release(u->snap);
    builder_free(&u->delta);
    arena_destroy(u->batch);
    arena_destroy(u->table_arena);
    arena_destroy(u->arena);
}

/* ---- reconciling one path ---- */

/* root_containing finds the root that path is or lies below, and what follows it in path. */
static bool root_containing(const Updater *u, String path, String *root, String *rest) {
    for (size_t i = 0; i < u->snap->roots.count; i++) {
        *root = u->snap->roots.items[i];
        if (str_equal(path, *root)) {
            *rest = S("");
            return true;
        }
        bool slash_root = str_ends_with(*root, S("/"));
        if (str_starts_with(path, *root) && (slash_root || path.data[root->len] == '/')) {
            *rest = str_slice(path, root->len + (slash_root ? 0 : 1), path.len);
            return true;
        }
    }
    return false;
}

static bool lookup(const Updater *u, String path, uint32_t *out) {
    String root, rest;
    if (!root_containing(u, path, &root, &rest)) return false;
    uint32_t cur;
    if (!find_child(u, NO_PARENT, root, &cur)) return false;
    String part;
    while (rest.len) {
        str_cut(rest, '/', &part, &rest);
        if (part.len > 0 && !find_child(u, cur, part, &cur)) return false;
    }
    *out = cur;
    return !is_dead(u, cur);
}

static void tombstone(Updater *u, uint32_t id) {
    if (is_dead(u, id)) return;
    bitmap_set(u->dead, id);
    u->dead_count++;
    table_remove(u, id);
    if (view_is_dir(u, id)) u->removed_dir = true;
}

static bool remove_path(Updater *u, String path) {
    uint32_t id;
    if (!lookup(u, path, &id)) return false;
    tombstone(u, id);
    return true;
}

static bool same_metadata(const FileRecord *r, const FileInfo *info) {
    return r->size == info->size && r->mtime == seconds_of(info->mtime_ns) && r->ctime == seconds_of(info->birth_ns);
}

static void absorb(Updater *u, uint32_t first, uint32_t end) {
    ensure_dead_capacity(u);
    for (uint32_t id = first; id < end; id++) table_insert(u, id);
}

static bool reconcile(Updater *u, String raw_path, bool rescan);

static bool upsert(Updater *u, String path, const FileInfo *info, bool rescan) {
    uint32_t id;
    if (lookup(u, path, &id)) {
        FileRecord r = view_record(u, id);
        if (record_is_dir(&r) == info->is_dir) {
            /* A directory's own size and times are not tracked; its children report their changes. */
            if (info->is_dir && !rescan) return false;
            if (!info->is_dir) {
                if (same_metadata(&r, info)) return false;
                String name = view_name(u, id);
                tombstone(u, id);
                uint32_t fresh = scan_add_record(&u->delta, name, r.parent, info);
                absorb(u, fresh, fresh + 1);
                return true;
            }
        }
        tombstone(u, id);
    }
    String root, rest;
    root_containing(u, path, &root, &rest);
    uint32_t parent = NO_PARENT;
    if (rest.len > 0) {
        String parent_path = path_dir(path);
        if (!lookup(u, parent_path, &parent)) {
            /* The parent is not indexed yet (events arrive out of order); reconciling it scans this path too. */
            return reconcile(u, parent_path, false);
        }
    }
    if (info->is_dir) {
        ScanResult res;
        if (scan_tree(u->io, &u->delta, path, parent, u->ex, nullptr, nullptr, &res, nullptr) != ERR_OK) return false;
        absorb(u, res.first_id, res.end_id);
        return res.end_id > res.first_id;
    }
    uint32_t fresh = scan_add_record(&u->delta, parent == NO_PARENT ? path : path_base(path), parent, info);
    absorb(u, fresh, fresh + 1);
    return true;
}

static bool reconcile(Updater *u, String raw_path, bool rescan) {
    String path = path_clean(u->batch, raw_path);
    String root, rest;
    if (!root_containing(u, path, &root, &rest)) return false;
    if (excludes_match(u->ex, path, path_base(path))) return remove_path(u, path);
    FileInfo info;
    if (file_info(path, &info, nullptr) != ERR_OK) return false;
    if (!info.exists) return remove_path(u, path);
    return upsert(u, path, &info, rescan);
}

/* drop_orphans tombstones everything below removed directories; parents precede children, so one pass suffices. */
static void drop_orphans(Updater *u) {
    uint32_t total = view_total(u);
    for (uint32_t id = 0; id < total; id++) {
        uint32_t parent = view_record(u, id).parent;
        if (parent != NO_PARENT && !is_dead(u, id) && is_dead(u, parent)) tombstone(u, id);
    }
}

bool updater_apply(Updater *u, const WatchEvent *events, size_t count) {
    arena_reset(u->batch);
    builder_init(&u->delta, u->snap->total);
    u->dead_words = bitmap_words(u->snap->total) + 1;
    u->dead = arena_push(u->batch, u->dead_words * sizeof *u->dead);
    memcpy(u->dead, u->snap->dead, u->dead_words * sizeof *u->dead);
    u->dead_count = u->snap->dead_count;
    u->removed_dir = false;

    bool changed = false;
    for (size_t i = 0; i < count; i++) changed |= reconcile(u, events[i].path, events[i].rescan);
    if (u->removed_dir) drop_orphans(u);

    if (!changed) {
        builder_free(&u->delta);
        return false;
    }
    ensure_dead_capacity(u);
    Segment *delta = u->delta.count ? segment_from_builder(&u->delta) : nullptr;
    builder_free(&u->delta);
    Snapshot *next = snapshot_derive(u->snap, delta, u->dead, u->dead_count);
    snapshot_retain(next);
    index_publish(u->ix, next);
    snapshot_release(u->snap);
    u->snap = next;
    return true;
}

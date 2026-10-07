#include "updater.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/hash.h"
#include "scanner.h"

struct Updater {
    Index *ix;
    ThreadPool *io;
    const Excludes *ex;
    Snapshot *snap;
    IdTable by_parent_name; /* every live record, keyed by (parent id, name) */

    /* state of the batch being applied */
    SegmentBuilder delta;
    uint64_t *dead;
    size_t dead_words;
    uint32_t dead_count;
    bool removed_dir;
    StrList *new_dirs;
};

/* ---- a view over the snapshot plus the delta under construction ---- */

static const FileRecord *view_record(const Updater *u, uint32_t id) {
    return id < u->snap->total ? snap_record(u->snap, id) : &u->delta.recs[id - u->delta.base_id];
}

static const char *view_name(const Updater *u, uint32_t id) {
    return id < u->snap->total ? snap_name(u->snap, id) : builder_name(&u->delta, id);
}

static uint32_t view_total(const Updater *u) { return u->delta.base_id + u->delta.count; }

static void view_path(const Updater *u, uint32_t id, StrBuf *out) {
    U32Vec chain = {0};
    for (uint32_t cur = id; cur != NO_PARENT; cur = view_record(u, cur)->parent) u32vec_push(&chain, cur);
    sb_clear(out);
    for (size_t k = chain.len; k-- > 0;) {
        if (out->len > 0 && out->data[out->len - 1] != '/') sb_putc(out, '/');
        sb_puts(out, view_name(u, chain.data[k]));
    }
    sb_cstr(out);
    u32vec_free(&chain);
}

static bool is_dead(const Updater *u, uint32_t id) { return bitmap_test(u->dead, id); }

static void ensure_dead_capacity(Updater *u) {
    size_t need = bitmap_words(view_total(u)) + 1;
    if (need <= u->dead_words) return;
    size_t words = max_size(need, u->dead_words * 2);
    u->dead = xrealloc(u->dead, words * sizeof *u->dead);
    memset(u->dead + u->dead_words, 0, (words - u->dead_words) * sizeof *u->dead);
    u->dead_words = words;
}

/* ---- the (parent, name) table ---- */

static uint64_t key_hash(uint32_t parent, const char *name, size_t len) { return hash_bytes(name, len, parent); }

static uint64_t rehash_record(void *ctx, uint32_t id) {
    const Updater *u = ctx;
    const FileRecord *r = view_record(u, id);
    return key_hash(r->parent, view_name(u, id), r->name_len);
}

typedef struct {
    const Updater *u;
    uint32_t parent;
    const char *name;
    size_t len;
} Key;

static bool key_equal(void *ctx, uint32_t id) {
    const Key *k = ctx;
    const FileRecord *r = view_record(k->u, id);
    return r->parent == k->parent && r->name_len == k->len && memcmp(view_name(k->u, id), k->name, k->len) == 0;
}

static void table_insert(Updater *u, uint32_t id) { idtable_insert(&u->by_parent_name, rehash_record(u, id), id, rehash_record, u); }

static void table_remove(Updater *u, uint32_t id) { idtable_remove(&u->by_parent_name, rehash_record(u, id), id); }

static bool find_child(const Updater *u, uint32_t parent, const char *name, size_t len, uint32_t *out) {
    Key k = {u, parent, name, len};
    uint32_t id = idtable_find(&u->by_parent_name, key_hash(parent, name, len), key_equal, &k);
    if (id == UINT32_MAX) return false;
    *out = id;
    return true;
}

static void build_table(Updater *u) {
    idtable_free(&u->by_parent_name);
    idtable_init(&u->by_parent_name, snap_live_count(u->snap));
    for (uint32_t id = 0; id < u->snap->total; id++)
        if (snap_live(u->snap, id)) table_insert(u, id);
}

/* ---- lifecycle ---- */

Updater *updater_new(ThreadPool *io, Index *ix, const Excludes *ex) {
    Updater *u = xcalloc(1, sizeof *u);
    u->ix = ix;
    u->io = io;
    u->ex = ex;
    updater_reset(u, index_acquire(ix));
    return u;
}

void updater_reset(Updater *u, Snapshot *s) {
    if (u->snap) snapshot_release(u->snap);
    u->snap = s;
    builder_init(&u->delta, s->total);
    build_table(u);
}

Snapshot *updater_snapshot(const Updater *u) { return u->snap; }

void updater_free(Updater *u) {
    snapshot_release(u->snap);
    idtable_free(&u->by_parent_name);
    free(u);
}

/* ---- reconciling one path ---- */

static const char *root_containing(const Updater *u, const char *path, const char **rest) {
    for (size_t i = 0; i < u->snap->roots.len; i++) {
        const char *root = u->snap->roots.items[i];
        size_t n = strlen(root);
        if (strcmp(path, root) == 0) {
            *rest = path + n;
            return root;
        }
        bool slash_root = n > 0 && root[n - 1] == '/';
        if (strncmp(path, root, n) == 0 && (slash_root || path[n] == '/')) {
            *rest = path + n + (slash_root ? 0 : 1);
            return root;
        }
    }
    return NULL;
}

static bool lookup(const Updater *u, const char *path, uint32_t *out) {
    const char *rest;
    const char *root = root_containing(u, path, &rest);
    if (!root) return false;
    uint32_t cur;
    if (!find_child(u, NO_PARENT, root, strlen(root), &cur)) return false;
    while (*rest) {
        size_t len = strcspn(rest, "/");
        if (len > 0 && !find_child(u, cur, rest, len, &cur)) return false;
        rest += len;
        if (*rest == '/') rest++;
    }
    *out = cur;
    return !is_dead(u, cur);
}

static void tombstone(Updater *u, uint32_t id) {
    if (is_dead(u, id)) return;
    bitmap_set(u->dead, id);
    u->dead_count++;
    table_remove(u, id);
    if (record_is_dir(view_record(u, id))) u->removed_dir = true;
}

static bool remove_path(Updater *u, const char *path) {
    uint32_t id;
    if (!lookup(u, path, &id)) return false;
    tombstone(u, id);
    return true;
}

static bool same_metadata(const FileRecord *r, const struct stat *st) {
    return r->size == (int64_t)st->st_size && r->mtime == (int64_t)st->st_mtime && r->ctime == stat_birthtime(st);
}

static void absorb(Updater *u, uint32_t first, uint32_t end) {
    ensure_dead_capacity(u);
    StrBuf path = {0};
    for (uint32_t id = first; id < end; id++) {
        table_insert(u, id);
        if (u->new_dirs && record_is_dir(view_record(u, id))) {
            view_path(u, id, &path);
            strlist_push(u->new_dirs, path.data);
        }
    }
    sb_free(&path);
}

static bool reconcile(Updater *u, const char *raw_path);

static bool upsert(Updater *u, const char *path, const struct stat *st) {
    uint32_t id;
    if (lookup(u, path, &id)) {
        const FileRecord *r = view_record(u, id);
        if (record_is_dir(r) == S_ISDIR(st->st_mode)) {
            /* A directory's own size and times are not tracked; its children report their changes. */
            if (S_ISDIR(st->st_mode) || same_metadata(r, st)) return false;
            uint32_t parent = r->parent;
            char *name = xstrdup(view_name(u, id));
            tombstone(u, id);
            uint32_t fresh = scan_add_record(&u->delta, name, strlen(name), parent, st);
            free(name);
            absorb(u, fresh, fresh + 1);
            return true;
        }
        tombstone(u, id);
    }
    char *parent_path = path_dir(path);
    uint32_t parent;
    if (!lookup(u, parent_path, &parent)) {
        /* The parent is not indexed yet (events arrive out of order); reconciling it scans this path too. */
        bool changed = reconcile(u, parent_path);
        free(parent_path);
        return changed;
    }
    free(parent_path);
    if (S_ISDIR(st->st_mode)) {
        ScanResult res;
        Err err;
        if (!scan_tree(u->io, &u->delta, path, parent, u->ex, NULL, NULL, &res, &err)) return false;
        absorb(u, res.first_id, res.end_id);
        return res.end_id > res.first_id;
    }
    const char *name = path_base(path);
    uint32_t fresh = scan_add_record(&u->delta, name, strlen(name), parent, st);
    absorb(u, fresh, fresh + 1);
    return true;
}

static bool reconcile(Updater *u, const char *raw_path) {
    char *path = path_clean(raw_path);
    const char *rest;
    bool changed = false;
    if (!root_containing(u, path, &rest)) {
        free(path);
        return false;
    }
    struct stat st;
    if (excludes_match(u->ex, path, path_base(path))) {
        changed = remove_path(u, path);
    } else if (lstat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) changed = remove_path(u, path);
    } else {
        changed = upsert(u, path, &st);
    }
    free(path);
    return changed;
}

/* drop_orphans tombstones everything below removed directories; parents precede children, so one pass suffices. */
static void drop_orphans(Updater *u) {
    uint32_t total = view_total(u);
    for (uint32_t id = 0; id < total; id++) {
        uint32_t parent = view_record(u, id)->parent;
        if (parent != NO_PARENT && !is_dead(u, id) && is_dead(u, parent)) tombstone(u, id);
    }
}

bool updater_apply(Updater *u, const StrList *paths, StrList *new_dirs) {
    builder_init(&u->delta, u->snap->total);
    u->dead_words = bitmap_words(u->snap->total) + 1;
    u->dead = xmalloc(u->dead_words * sizeof *u->dead);
    memcpy(u->dead, u->snap->dead, (bitmap_words(u->snap->total) + 1) * sizeof *u->dead);
    u->dead_count = u->snap->dead_count;
    u->removed_dir = false;
    u->new_dirs = new_dirs;

    bool changed = false;
    for (size_t i = 0; i < paths->len; i++) changed |= reconcile(u, paths->items[i]);
    if (u->removed_dir) drop_orphans(u);

    if (!changed) {
        builder_free(&u->delta);
        free(u->dead);
        u->dead = NULL;
        return false;
    }
    Segment *delta = u->delta.count ? segment_from_builder(&u->delta) : NULL;
    builder_free(&u->delta);
    Snapshot *next = snapshot_derive(u->snap, delta, u->dead, u->dead_count);
    u->dead = NULL;
    snapshot_retain(next);
    index_publish(u->ix, next);
    snapshot_release(u->snap);
    u->snap = next;
    builder_init(&u->delta, next->total);
    return true;
}

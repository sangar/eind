#include "index.h"

#include <string.h>

#include "journal.h"

static Snapshot *snapshot_alloc(void) {
    Arena *arena = arena_create(0);
    Snapshot *s = arena_push(arena, sizeof *s);
    atomic_init(&s->refs, 1);
    s->arena = arena;
    return s;
}

/* snapshot_new takes the reference to base. */
static Snapshot *snapshot_new(Segment *base) {
    Snapshot *s = snapshot_alloc();
    s->segs = arena_push(s->arena, sizeof *s->segs);
    s->segs[0] = base;
    s->seg_count = 1;
    s->total = base->count;
    s->dead = arena_push(s->arena, (bitmap_words(s->total) + 1) * sizeof *s->dead);
    s->built_at = base->built_at;
    s->roots = base->roots;
    return s;
}

Snapshot *snapshot_derive(const Snapshot *from, Segment *delta, const uint64_t *dead, uint32_t dead_count) {
    Snapshot *s = snapshot_alloc();
    s->seg_count = from->seg_count + (delta ? 1 : 0);
    s->segs = arena_push(s->arena, s->seg_count * sizeof *s->segs);
    for (uint32_t k = 0; k < from->seg_count; k++) {
        s->segs[k] = from->segs[k];
        segment_retain(s->segs[k]);
    }
    s->total = from->total;
    if (delta) {
        s->segs[from->seg_count] = delta;
        s->total += delta->count;
    }
    size_t words = bitmap_words(s->total) + 1;
    s->dead = arena_push(s->arena, words * sizeof *s->dead);
    memcpy(s->dead, dead, words * sizeof *s->dead);
    s->dead_count = dead_count;
    s->built_at = from->built_at;
    s->roots = from->roots;
    return s;
}

void snapshot_retain(Snapshot *s) { atomic_fetch_add(&s->refs, 1); }

void snapshot_release(Snapshot *s) {
    if (!s || atomic_fetch_sub(&s->refs, 1) != 1) return;
    for (uint32_t k = 0; k < s->seg_count; k++) segment_release(s->segs[k]);
    arena_destroy(s->arena);
}

/*
 * snap_path collects the names from id up to its root, then copies them in
 * one pass into room reserved once: path matchers build a path for every
 * record they test.
 */
String snap_path(const Snapshot *s, uint32_t id, StringBuilder *out) {
    String stack_chain[64];
    String *chain = stack_chain;
    size_t n = 0, cap = countof(stack_chain), len = 0;
    for (uint32_t cur = id;;) {
        if (n == cap) {
            String *bigger = arena_push(out->arena, cap * 2 * sizeof *bigger);
            memcpy(bigger, chain, n * sizeof *chain);
            chain = bigger;
            cap *= 2;
        }
        chain[n] = snap_name_view(s, cur);
        len += chain[n++].len + 1;
        uint32_t parent = snap_parent(s, cur);
        if (parent == NO_PARENT) break;
        cur = parent;
    }
    if (out->capacity <= len) *out = str_builder_create(out->arena, max_size(len + 1, 2 * out->capacity));
    char *path = out->data;
    size_t at = 0;
    for (size_t k = n; k-- > 0;) {
        if (at > 0 && path[at - 1] != '/') path[at++] = '/';
        if (chain[k].len) memcpy(path + at, chain[k].data, chain[k].len);
        at += chain[k].len;
    }
    path[at] = '\0';
    out->len = at;
    return (String){path, at};
}

String snap_ext(const Snapshot *s, uint32_t id) {
    String name = snap_name_view(s, id);
    size_t dot;
    if (!str_find_last_char(name, '.', &dot) || dot == 0) return S("");
    return str_slice(name, dot + 1, name.len);
}

void snap_stats(const Snapshot *s, int64_t *files, int64_t *dirs) {
    *files = *dirs = 0;
    for (uint32_t id = 0; id < s->total; id++) {
        if (!snap_live(s, id)) continue;
        if (snap_is_dir(s, id)) {
            (*dirs)++;
        } else {
            (*files)++;
        }
    }
}

uint32_t snap_live_count(const Snapshot *s) { return s->total - s->dead_count; }

void index_init(Index *ix, Snapshot *initial) {
    mutex_init(&ix->mutex);
    ix->current = initial;
}

void index_destroy(Index *ix) {
    snapshot_release(ix->current);
    mutex_destroy(&ix->mutex);
}

Snapshot *index_acquire(Index *ix) {
    mutex_lock(&ix->mutex);
    Snapshot *s = ix->current;
    snapshot_retain(s);
    mutex_unlock(&ix->mutex);
    return s;
}

void index_publish(Index *ix, Snapshot *next) {
    mutex_lock(&ix->mutex);
    Snapshot *old = ix->current;
    ix->current = next;
    mutex_unlock(&ix->mutex);
    snapshot_release(old);
}

Error index_load(String path, Snapshot **snapshot, Err *err) {
    *snapshot = nullptr;
    Segment *seg;
    Error e = segment_open(path, &seg, err);
    if (e != ERR_OK) return e;
    return journal_replay(snapshot_new(seg), path, snapshot, err);
}

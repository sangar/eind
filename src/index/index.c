#include "index.h"

#include <stdlib.h>
#include <string.h>

#include "journal.h"

Snapshot *snapshot_new(Segment *base, const StrList *roots, int64_t built_at) {
    Snapshot *s = xcalloc(1, sizeof *s);
    atomic_init(&s->refs, 1);
    s->segs = xmalloc(sizeof *s->segs);
    s->segs[0] = base;
    s->seg_count = 1;
    s->total = base->count;
    s->dead = xcalloc(bitmap_words(s->total) + 1, sizeof *s->dead);
    s->built_at = built_at;
    strlist_copy(&s->roots, roots);
    return s;
}

Snapshot *snapshot_derive(const Snapshot *from, Segment *delta, uint64_t *dead, uint32_t dead_count) {
    Snapshot *s = xcalloc(1, sizeof *s);
    atomic_init(&s->refs, 1);
    s->seg_count = from->seg_count + (delta ? 1 : 0);
    s->segs = xmalloc(s->seg_count * sizeof *s->segs);
    for (uint32_t k = 0; k < from->seg_count; k++) {
        s->segs[k] = from->segs[k];
        segment_retain(s->segs[k]);
    }
    s->total = from->total;
    if (delta) {
        s->segs[from->seg_count] = delta;
        s->total += delta->count;
    }
    s->dead = dead;
    s->dead_count = dead_count;
    s->built_at = from->built_at;
    strlist_copy(&s->roots, &from->roots);
    return s;
}

void snapshot_retain(Snapshot *s) { atomic_fetch_add(&s->refs, 1); }

void snapshot_release(Snapshot *s) {
    if (!s || atomic_fetch_sub(&s->refs, 1) != 1) return;
    for (uint32_t k = 0; k < s->seg_count; k++) segment_release(s->segs[k]);
    free(s->segs);
    free(s->dead);
    strlist_free(&s->roots);
    free(s);
}

void snap_path(const Snapshot *s, uint32_t id, StrBuf *sb) {
    uint32_t stack_buf[64];
    uint32_t *chain = stack_buf;
    size_t n = 0, cap = ARRAY_LEN(stack_buf);
    for (uint32_t cur = id;;) {
        if (n == cap) {
            uint32_t *bigger = xmalloc(cap * 2 * sizeof *bigger);
            memcpy(bigger, chain, n * sizeof *chain);
            if (chain != stack_buf) free(chain);
            chain = bigger;
            cap *= 2;
        }
        chain[n++] = cur;
        uint32_t parent = snap_record(s, cur)->parent;
        if (parent == NO_PARENT) break;
        cur = parent;
    }
    sb_clear(sb);
    for (size_t k = n; k-- > 0;) {
        const FileRecord *r = snap_record(s, chain[k]);
        if (sb->len > 0 && sb->data[sb->len - 1] != '/') sb_putc(sb, '/');
        sb_append(sb, snap_name(s, chain[k]), r->name_len);
    }
    sb_cstr(sb);
    if (chain != stack_buf) free(chain);
}

const char *snap_ext(const Snapshot *s, uint32_t id, size_t *len) {
    const char *lower = snap_lower(s, id);
    const char *dot = strrchr(lower, '.');
    if (!dot || dot == lower) {
        *len = 0;
        return "";
    }
    *len = strlen(dot + 1);
    return dot + 1;
}

void snap_stats(const Snapshot *s, int64_t *files, int64_t *dirs) {
    *files = *dirs = 0;
    for (uint32_t id = 0; id < s->total; id++) {
        if (!snap_live(s, id)) continue;
        if (record_is_dir(snap_record(s, id))) {
            (*dirs)++;
        } else {
            (*files)++;
        }
    }
}

uint32_t snap_live_count(const Snapshot *s) { return s->total - s->dead_count; }

void index_init(Index *ix, Snapshot *initial) {
    pthread_mutex_init(&ix->mu, NULL);
    ix->current = initial;
}

void index_destroy(Index *ix) {
    snapshot_release(ix->current);
    pthread_mutex_destroy(&ix->mu);
}

Snapshot *index_acquire(Index *ix) {
    pthread_mutex_lock(&ix->mu);
    Snapshot *s = ix->current;
    snapshot_retain(s);
    pthread_mutex_unlock(&ix->mu);
    return s;
}

void index_publish(Index *ix, Snapshot *next) {
    pthread_mutex_lock(&ix->mu);
    Snapshot *old = ix->current;
    ix->current = next;
    pthread_mutex_unlock(&ix->mu);
    snapshot_release(old);
}

Snapshot *index_load(const char *path, bool *missing, Err *err) {
    StrList roots = {0};
    int64_t built_at = 0;
    Segment *seg = segment_open(path, &roots, &built_at, missing, err);
    if (!seg) {
        strlist_free(&roots);
        return NULL;
    }
    Snapshot *s = snapshot_new(seg, &roots, built_at);
    strlist_free(&roots);
    return journal_replay(s, path, err);
}

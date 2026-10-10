#include "journal.h"

#include <string.h>

enum { JOURNAL_HEADER_SIZE = 12, OP_ADD = 'A', OP_REMOVE = 'R', OP_UPDATE = 'U' };

static const char JOURNAL_MAGIC[4] = {'E', 'I', 'N', 'J'};

/* A Journal and everything it holds live in its arena. */
struct Journal {
    Arena *arena;
    String path;
    char header[JOURNAL_HEADER_SIZE];
    StringBuilder pending;
    size_t entries;
};

static String journal_path(Arena *arena, String index_path) { return str_concat(arena, index_path, S(".journal")); }

static void make_header(char out[JOURNAL_HEADER_SIZE], uint64_t generation) {
    memcpy(out, JOURNAL_MAGIC, 4);
    memcpy(out + 4, &generation, 8);
}

static uint32_t get_u32(const char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int64_t get_i64(const char *p) {
    int64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* entry_size is the length of the entry at p, or 0 if it is incomplete or unknown. */
static size_t entry_size(const char *p, size_t avail) {
    size_t need;
    switch (p[0]) {
    case OP_ADD:
        if (avail < 32) return 0;
        need = 32 + (uint16_t)((uint8_t)p[30] | (uint8_t)p[31] << 8);
        break;
    case OP_REMOVE: need = 5; break;
    case OP_UPDATE: need = 29; break;
    default: return 0;
    }
    return avail < need ? 0 : need;
}

/* valid_length is the length of the whole entries in data, or 0 if data is not this generation's journal. */
static size_t valid_length(String data, uint64_t generation, size_t *entries) {
    char header[JOURNAL_HEADER_SIZE];
    make_header(header, generation);
    *entries = 0;
    if (data.len < JOURNAL_HEADER_SIZE || memcmp(data.data, header, JOURNAL_HEADER_SIZE) != 0) return 0;
    size_t p = JOURNAL_HEADER_SIZE;
    while (p < data.len) {
        size_t n = entry_size(data.data + p, data.len - p);
        if (n == 0) break;
        p += n;
        (*entries)++;
    }
    return p;
}

/* read_journal reads the journal at path; a missing journal reads as empty. */
[[nodiscard]] static Error read_journal(Arena *arena, String path, String *data, Err *err) {
    Error e = file_read_all(arena, path, data, err);
    if (e == ERR_NOT_FOUND) {
        *data = S("");
        return ERR_OK;
    }
    return e;
}

/* Dead is a tombstone bitmap that grows with the ids the journal adds. */
typedef struct {
    Arena *arena;
    uint64_t *bits;
    size_t words;
    uint32_t count;
} Dead;

static void dead_reserve(Dead *d, size_t words) {
    if (words <= d->words) return;
    size_t grown = max_size(d->words * 2, words);
    uint64_t *bits = arena_push(d->arena, grown * sizeof *bits);
    if (d->words) memcpy(bits, d->bits, d->words * sizeof *bits);
    d->bits = bits;
    d->words = grown;
}

static void dead_set(Dead *d, uint32_t id) {
    dead_reserve(d, id / 64 + 1);
    if (!bitmap_test(d->bits, id)) {
        bitmap_set(d->bits, id);
        d->count++;
    }
}

Error journal_replay(Snapshot *s, String path, Snapshot **replayed, Err *err) {
    *replayed = nullptr;
    Arena *scratch = arena_create(0);
    String data;
    Error e = read_journal(scratch, journal_path(scratch, path), &data, err);
    size_t entries = 0;
    size_t valid = e == ERR_OK ? valid_length(data, s->segs[0]->generation, &entries) : 0;
    if (e != ERR_OK || valid == 0 || entries == 0) {
        arena_destroy(scratch);
        if (e != ERR_OK) {
            snapshot_release(s);
            return e;
        }
        *replayed = s;
        return ERR_OK;
    }
    Segment *base = s->segs[0]; /* unpublished, so the journal may still change its records */
    SegmentBuilder added;
    builder_init(&added, s->total);
    Dead dead = {.arena = scratch};
    dead_reserve(&dead, bitmap_words(s->total) + 1);
    bool corrupt = false;
    for (size_t p = JOURNAL_HEADER_SIZE; p < valid && !corrupt;) {
        const char *entry = data.data + p;
        p += entry_size(entry, valid - p);
        uint32_t total = s->total + added.count;
        if (entry[0] == OP_ADD) {
            uint32_t parent = get_u32(entry + 1);
            uint16_t len = (uint16_t)((uint8_t)entry[30] | (uint8_t)entry[31] << 8);
            if (parent != NO_PARENT && parent >= total) {
                corrupt = true;
                break;
            }
            builder_add(&added, (String){entry + 32, len}, parent, get_i64(entry + 5), get_i64(entry + 13),
                        get_i64(entry + 21), entry[29] ? RECORD_DIR : 0);
            continue;
        }
        uint32_t id = get_u32(entry + 1);
        if (id >= total) {
            corrupt = true;
            break;
        }
        if (entry[0] == OP_REMOVE) {
            dead_set(&dead, id);
            continue;
        }
        int64_t size = get_i64(entry + 5), mtime = get_i64(entry + 13), ctime = get_i64(entry + 21);
        if (id < base->count) {
            segment_set_times(base, id, size, mtime, ctime);
        } else {
            FileRecord *r = &added.recs[id - added.base_id];
            r->size = size;
            r->mtime = mtime;
            r->ctime = ctime;
        }
    }
    if (corrupt) {
        builder_free(&added);
        arena_destroy(scratch);
        snapshot_release(s);
        return err_set(err, ERR_PARSE, "%.*s: corrupt index journal", (int)path.len, path.data);
    }
    dead_reserve(&dead, bitmap_words(s->total + added.count) + 1);
    Segment *delta = added.count ? segment_from_builder(&added) : nullptr;
    builder_free(&added);
    *replayed = snapshot_derive(s, delta, dead.bits, dead.count);
    snapshot_release(s);
    arena_destroy(scratch);
    return ERR_OK;
}

Error journal_open(String path, const Snapshot *s, Journal **journal, Err *err) {
    *journal = nullptr;
    Arena *arena = arena_create(0);
    Journal *j = arena_push(arena, sizeof *j);
    j->arena = arena;
    j->path = journal_path(arena, path);
    j->pending = str_builder_create(arena, 4096);
    make_header(j->header, s->segs[0]->generation);
    Arena *scratch = arena_create(0);
    String data;
    Error e = read_journal(scratch, j->path, &data, err);
    if (e == ERR_OK) {
        size_t valid = valid_length(data, s->segs[0]->generation, &j->entries);
        if (valid == 0) {
            e = file_write_atomic(j->path, (String){j->header, JOURNAL_HEADER_SIZE}, 0644, err);
        } else if (valid < data.len) {
            e = file_write_atomic(j->path, str_slice(data, 0, valid), 0644, err);
        }
    }
    arena_destroy(scratch);
    if (e != ERR_OK) {
        arena_destroy(arena);
        return e;
    }
    *journal = j;
    return ERR_OK;
}

void journal_free(Journal *j) {
    if (j) arena_destroy(j->arena);
}

static void put_bytes(StringBuilder *out, const void *data, size_t len) { str_builder_append(out, (String){data, len}); }
static void put_u32(StringBuilder *out, uint32_t v) { put_bytes(out, &v, 4); }
static void put_i64(StringBuilder *out, int64_t v) { put_bytes(out, &v, 8); }

/*
 * A changed file is a new record in the next snapshot and its old record a
 * tombstone, so the differences between snapshots are additions, written
 * first in id order, then removals.
 */
void journal_record(Journal *j, const Snapshot *before, const Snapshot *after) {
    for (uint32_t id = before->total; id < after->total; id++) {
        FileRecord r = snap_record(after, id);
        str_builder_append_char(&j->pending, OP_ADD);
        put_u32(&j->pending, r.parent);
        put_i64(&j->pending, r.size);
        put_i64(&j->pending, r.mtime);
        put_i64(&j->pending, r.ctime);
        str_builder_append_char(&j->pending, record_is_dir(&r) ? 1 : 0);
        uint16_t len = (uint16_t)r.name_len;
        put_bytes(&j->pending, &len, 2);
        put_bytes(&j->pending, snap_name(after, id), len);
        j->entries++;
    }
    for (uint32_t id = 0; id < after->total; id++) {
        bool was_dead = id < before->total && !snap_live(before, id);
        if (snap_live(after, id) || was_dead) continue;
        str_builder_append_char(&j->pending, OP_REMOVE);
        put_u32(&j->pending, id);
        j->entries++;
    }
}

Error journal_flush(Journal *j, bool *replaced, Err *err) {
    *replaced = false;
    if (j->pending.len == 0) return ERR_OK;
    int fd;
    Error e = file_open_append(j->path, &fd, err);
    if (e != ERR_OK) return e;
    char header[JOURNAL_HEADER_SIZE];
    size_t got = 0;
    e = file_pread(fd, header, sizeof header, 0, &got, err);
    if (e == ERR_OK && (got != sizeof header || memcmp(header, j->header, sizeof header) != 0)) {
        *replaced = true;
    } else if (e == ERR_OK) {
        e = file_write(fd, (String){j->pending.data, j->pending.len}, err);
        if (e == ERR_OK) j->pending.len = 0;
    }
    file_close(fd);
    return e;
}

size_t journal_entries(const Journal *j) { return j->entries; }

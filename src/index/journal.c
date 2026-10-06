#include "journal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JOURNAL_MAGIC "EINJ"
#define JOURNAL_HEADER_SIZE 12
#define OP_ADD 'A'
#define OP_REMOVE 'R'
#define OP_UPDATE 'U'

struct Journal {
    char *path;
    char header[JOURNAL_HEADER_SIZE];
    StrBuf pending;
    size_t entries;
};

static char *journal_path(const char *index_path) {
    StrBuf sb = {0};
    sb_printf(&sb, "%s.journal", index_path);
    return sb.data;
}

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
static size_t valid_length(const StrBuf *data, uint64_t generation, size_t *entries) {
    char header[JOURNAL_HEADER_SIZE];
    make_header(header, generation);
    *entries = 0;
    if (data->len < JOURNAL_HEADER_SIZE || memcmp(data->data, header, JOURNAL_HEADER_SIZE) != 0) return 0;
    size_t p = JOURNAL_HEADER_SIZE;
    for (;;) {
        if (p >= data->len) break;
        size_t n = entry_size(data->data + p, data->len - p);
        if (n == 0) break;
        p += n;
        (*entries)++;
    }
    return p;
}

/* read_journal reads the journal at path; a missing journal reads as empty. */
static bool read_journal(const char *path, StrBuf *out, Err *err) {
    sb_clear(out);
    if (access(path, F_OK) != 0 && errno == ENOENT) return true;
    return read_file(path, out, err);
}

static void set_dead(uint64_t **dead, size_t *words, uint32_t id, uint32_t *dead_count) {
    if (id / 64 >= *words) {
        size_t grown = MAX(*words * 2, id / 64 + 1);
        *dead = xrealloc(*dead, grown * sizeof **dead);
        memset(*dead + *words, 0, (grown - *words) * sizeof **dead);
        *words = grown;
    }
    if (!bitmap_test(*dead, id)) {
        bitmap_set(*dead, id);
        (*dead_count)++;
    }
}

Snapshot *journal_replay(Snapshot *s, const char *path, Err *err) {
    char *jpath = journal_path(path);
    StrBuf data = {0};
    bool ok = read_journal(jpath, &data, err);
    free(jpath);
    if (!ok) {
        sb_free(&data);
        snapshot_release(s);
        return NULL;
    }
    size_t entries;
    size_t valid = valid_length(&data, s->segs[0]->generation, &entries);
    if (valid == 0 || entries == 0) {
        sb_free(&data);
        return s;
    }
    Segment *base = s->segs[0];
    FileRecord *base_recs = base->owned_recs; /* decoded on load, so a fresh snapshot may still change them */
    SegmentBuilder added;
    builder_init(&added, s->total);
    size_t words = bitmap_words(s->total) + 1;
    uint64_t *dead = xcalloc(words, sizeof *dead);
    uint32_t dead_count = 0;
    bool corrupt = false;
    for (size_t p = JOURNAL_HEADER_SIZE; p < valid && !corrupt;) {
        const char *e = data.data + p;
        p += entry_size(e, valid - p);
        uint32_t total = s->total + added.count;
        if (e[0] == OP_ADD) {
            uint32_t parent = get_u32(e + 1);
            uint16_t len = (uint16_t)((uint8_t)e[30] | (uint8_t)e[31] << 8);
            if (parent != NO_PARENT && parent >= total) {
                corrupt = true;
                break;
            }
            builder_add(&added, e + 32, len, parent, get_i64(e + 5), get_i64(e + 13), get_i64(e + 21),
                        e[29] ? RECORD_DIR : 0);
            continue;
        }
        uint32_t id = get_u32(e + 1);
        if (id >= total) {
            corrupt = true;
            break;
        }
        if (e[0] == OP_REMOVE) {
            set_dead(&dead, &words, id, &dead_count);
            continue;
        }
        FileRecord *r = id < base->count ? &base_recs[id] : &added.recs[id - added.base_id];
        r->size = get_i64(e + 5);
        r->mtime = get_i64(e + 13);
        r->ctime = get_i64(e + 21);
    }
    sb_free(&data);
    if (corrupt) {
        builder_free(&added);
        free(dead);
        snapshot_release(s);
        err_set(err, "%s: corrupt index journal", path);
        return NULL;
    }
    uint32_t total = s->total + added.count;
    if (words < bitmap_words(total) + 1) {
        size_t grown = bitmap_words(total) + 1;
        dead = xrealloc(dead, grown * sizeof *dead);
        memset(dead + words, 0, (grown - words) * sizeof *dead);
    }
    Segment *delta = added.count ? segment_from_builder(&added) : NULL;
    builder_free(&added);
    Snapshot *replayed = snapshot_derive(s, delta, dead, dead_count);
    snapshot_release(s);
    return replayed;
}

Journal *journal_open(const char *path, const Snapshot *s, Err *err) {
    Journal *j = xcalloc(1, sizeof *j);
    j->path = journal_path(path);
    make_header(j->header, s->segs[0]->generation);
    StrBuf data = {0};
    if (!read_journal(j->path, &data, err)) {
        sb_free(&data);
        journal_free(j);
        return NULL;
    }
    size_t valid = valid_length(&data, s->segs[0]->generation, &j->entries);
    bool ok = true;
    if (valid == 0) {
        ok = write_file_atomic(j->path, j->header, JOURNAL_HEADER_SIZE, 0644, err);
    } else if (valid < data.len) {
        ok = write_file_atomic(j->path, data.data, valid, 0644, err);
    }
    sb_free(&data);
    if (!ok) {
        journal_free(j);
        return NULL;
    }
    return j;
}

void journal_free(Journal *j) {
    if (!j) return;
    free(j->path);
    sb_free(&j->pending);
    free(j);
}

static void put_u32(StrBuf *sb, uint32_t v) { sb_append(sb, (const char *)&v, 4); }
static void put_i64(StrBuf *sb, int64_t v) { sb_append(sb, (const char *)&v, 8); }

/*
 * A changed file is a new record in the next snapshot and its old record a
 * tombstone, so the differences between snapshots are additions, written
 * first in id order, then removals.
 */
void journal_record(Journal *j, const Snapshot *before, const Snapshot *after) {
    for (uint32_t id = before->total; id < after->total; id++) {
        const FileRecord *r = snap_record(after, id);
        sb_putc(&j->pending, OP_ADD);
        put_u32(&j->pending, r->parent);
        put_i64(&j->pending, r->size);
        put_i64(&j->pending, r->mtime);
        put_i64(&j->pending, r->ctime);
        sb_putc(&j->pending, record_is_dir(r) ? 1 : 0);
        uint16_t len = (uint16_t)r->name_len;
        sb_append(&j->pending, (const char *)&len, 2);
        sb_append(&j->pending, snap_name(after, id), len);
        j->entries++;
    }
    for (uint32_t id = 0; id < after->total; id++) {
        bool was_dead = id < before->total && !snap_live(before, id);
        if (snap_live(after, id) || was_dead) continue;
        sb_putc(&j->pending, OP_REMOVE);
        put_u32(&j->pending, id);
        j->entries++;
    }
}

JournalStatus journal_flush(Journal *j, Err *err) {
    if (j->pending.len == 0) return JOURNAL_OK;
    int fd = open(j->path, O_RDWR | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        err_set(err, "%s: %s", j->path, strerror(errno));
        return JOURNAL_FAILED;
    }
    char header[JOURNAL_HEADER_SIZE];
    ssize_t n = pread(fd, header, sizeof header, 0);
    if (n != (ssize_t)sizeof header || memcmp(header, j->header, sizeof header) != 0) {
        close(fd);
        return JOURNAL_REPLACED;
    }
    const char *p = j->pending.data;
    size_t left = j->pending.len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) {
            err_set(err, "%s: %s", j->path, strerror(errno));
            close(fd);
            return JOURNAL_FAILED;
        }
        p += w;
        left -= (size_t)w;
    }
    close(fd);
    sb_clear(&j->pending);
    return JOURNAL_OK;
}

size_t journal_entries(const Journal *j) { return j->entries; }

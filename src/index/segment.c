#include "segment.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../core/sort.h"

/*
 * The index file format is shared by every eind implementation and is
 * specified in docs/file-format.md. A header is followed by 8-byte aligned
 * sections, all integers little endian (the host order on every supported
 * platform):
 *
 *   roots      root paths, each followed by NUL
 *   names      the distinct names in name order, each followed by NUL
 *   name offs  u32 per distinct name: its offset in names
 *   name ids   u32 per record
 *   parents    u32 per record
 *   sizes      i64 per record
 *   modified   u32 per record, unix seconds
 *   created    u32 per record, unix seconds, 0 when unknown
 *   dirs       a bitset over records
 *   unicode    a bitset over distinct names whose Unicode lowercasing may
 *              differ from ASCII lowercasing
 *
 * Records are stored in the order of their lowercased paths and names are
 * numbered in name order, which readers rely on for sorting.
 *
 * Next to the file lives its journal (journal.c). segment_write starts an
 * empty one for the new file, so that a running daemon notices the file was
 * replaced.
 */
#define SEGMENT_MAGIC "EIND"
#define SEGMENT_VERSION 2
#define SECTION_COUNT 10
#define HEADER_SIZE (32 + 16 * SECTION_COUNT)
#define JOURNAL_MAGIC "EINJ"

enum { SEC_ROOTS, SEC_NAMES, SEC_NAME_OFF, SEC_NAME_ID, SEC_PARENT, SEC_SIZE, SEC_MODIFIED, SEC_CREATED, SEC_DIRS, SEC_UNICODE };

void builder_init(SegmentBuilder *b, uint32_t base_id) {
    *b = (SegmentBuilder){.base_id = base_id};
}

void builder_free(SegmentBuilder *b) {
    free(b->recs);
    free(b->names);
    *b = (SegmentBuilder){0};
}

uint32_t builder_add(SegmentBuilder *b, const char *name, size_t len, uint32_t parent, int64_t size,
                     int64_t mtime, int64_t ctime, uint32_t flags) {
    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->recs = xrealloc(b->recs, (size_t)b->cap * sizeof *b->recs);
    }
    if (b->names_len + len + 1 > b->names_cap) {
        size_t cap = b->names_cap ? b->names_cap : 16384;
        while (cap < b->names_len + len + 1) cap *= 2;
        b->names = xrealloc(b->names, cap);
        b->names_cap = cap;
    }
    FileRecord *r = &b->recs[b->count];
    *r = (FileRecord){.parent = parent,
                      .name_off = (uint32_t)b->names_len,
                      .name_len = (uint32_t)len,
                      .flags = flags,
                      .size = size,
                      .mtime = mtime,
                      .ctime = ctime};
    memcpy(b->names + b->names_len, name, len);
    b->names[b->names_len + len] = '\0';
    b->names_len += len + 1;
    return b->base_id + b->count++;
}

Segment *segment_from_builder(SegmentBuilder *b) {
    Segment *s = xcalloc(1, sizeof *s);
    atomic_init(&s->refs, 1);
    s->base_id = b->base_id;
    s->count = b->count;
    s->owned_recs = b->recs ? b->recs : xcalloc(1, sizeof(FileRecord));
    s->owned_names = b->names ? b->names : xcalloc(1, 1);
    s->recs = s->owned_recs;
    s->names = s->owned_names;
    b->recs = NULL;
    b->names = NULL;
    builder_free(b);
    return s;
}

/* ---- writing ---- */

typedef struct {
    const SegmentBuilder *b;
} WriteCtx;

static const char *rec_name(const SegmentBuilder *b, uint32_t i) { return b->names + b->recs[i].name_off; }

/*
 * compare_fold orders names as comparing their ASCII-lowercased forms would.
 * With a tail, a name compares as if followed by a separator.
 */
static int compare_fold(const char *a, size_t alen, bool atail, const char *b, size_t blen, bool btail) {
    for (size_t k = 0;; k++) {
        int x = k < alen ? (uint8_t)a[k] : atail && k == alen ? '/' : -1;
        int y = k < blen ? (uint8_t)b[k] : btail && k == blen ? '/' : -1;
        if (x < 0 || y < 0) return (x >= 0) - (y >= 0);
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return x < y ? -1 : 1;
    }
}

static int compare_raw(const char *a, size_t alen, const char *b, size_t blen) {
    int c = memcmp(a, b, min_size(alen, blen));
    return c ? c : (alen > blen) - (alen < blen);
}

static int compare_by_name(const void *ctx, const void *pa, const void *pb) {
    const SegmentBuilder *b = ((const WriteCtx *)ctx)->b;
    uint32_t x = *(const uint32_t *)pa, y = *(const uint32_t *)pb;
    const FileRecord *rx = &b->recs[x], *ry = &b->recs[y];
    int c = compare_fold(rec_name(b, x), rx->name_len, false, rec_name(b, y), ry->name_len, false);
    return c ? c : compare_raw(rec_name(b, x), rx->name_len, rec_name(b, y), ry->name_len);
}

/* An item is a record id shifted left, with the low bit set for the subtree below a directory. */
static int compare_items(const void *ctx, const void *pa, const void *pb) {
    const SegmentBuilder *b = ((const WriteCtx *)ctx)->b;
    uint32_t x = *(const uint32_t *)pa, y = *(const uint32_t *)pb;
    const char *nx = rec_name(b, x >> 1), *ny = rec_name(b, y >> 1);
    size_t lx = b->recs[x >> 1].name_len, ly = b->recs[y >> 1].name_len;
    size_t kx = (x & 1) && lx && nx[lx - 1] == '/' ? lx - 1 : lx;
    size_t ky = (y & 1) && ly && ny[ly - 1] == '/' ? ly - 1 : ly;
    int c = compare_fold(nx, kx, x & 1, ny, ky, y & 1);
    if (!c) c = compare_raw(nx, lx, ny, ly);
    return c ? c : (x > y) - (x < y);
}

/*
 * path_order orders records by lowercased path, keeping parents before
 * children. It walks the tree with each directory's children sorted by name,
 * where a directory appears twice: once as itself, keyed by its name, and once
 * as the subtree below it, keyed by its name and a separator. That places
 * "a.txt" between "a" and "a/b", as comparing whole paths would.
 */
static void path_order(const SegmentBuilder *b, uint32_t *order, uint32_t *new_id) {
    uint32_t n = b->count;
    uint32_t *start = xcalloc((size_t)n + 2, sizeof *start);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = min_u32(b->recs[i].parent, n);
        start[p + 1] += (b->recs[i].flags & RECORD_DIR) ? 2u : 1u;
    }
    for (uint32_t k = 1; k < n + 2; k++) start[k] += start[k - 1];
    uint32_t *items = xmalloc(((size_t)start[n + 1] + 1) * sizeof *items);
    uint32_t *fill = xmalloc(((size_t)n + 2) * sizeof *fill);
    memcpy(fill, start, ((size_t)n + 2) * sizeof *fill);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = min_u32(b->recs[i].parent, n);
        items[fill[p]++] = i << 1;
        if (b->recs[i].flags & RECORD_DIR) items[fill[p]++] = i << 1 | 1;
    }
    free(fill);
    WriteCtx ctx = {b};
    for (uint32_t p = 0; p <= n; p++) sort_stable(items + start[p], start[p + 1] - start[p], sizeof *items, compare_items, &ctx);

    uint32_t *stack = xmalloc(((size_t)start[n + 1] + 1) * sizeof *stack);
    size_t depth = 0, placed = 0;
    stack[depth++] = n << 1 | 1;
    while (depth > 0) {
        uint32_t it = stack[--depth];
        if (!(it & 1)) {
            new_id[it >> 1] = (uint32_t)placed;
            order[placed++] = it >> 1;
            continue;
        }
        uint32_t dir = it >> 1;
        for (uint32_t k = start[dir + 1]; k-- > start[dir];) stack[depth++] = items[k];
    }
    free(stack);
    free(items);
    free(start);
}

static size_t bitmap_words(uint32_t n) { return ((size_t)n + 63) / 64; }

static void bit_set(uint64_t *bits, uint32_t i) { bits[i >> 6] |= (uint64_t)1 << (i & 63); }

static bool has_non_ascii(const char *s, size_t n) {
    for (size_t k = 0; k < n; k++)
        if ((uint8_t)s[k] >= 0x80) return true;
    return false;
}

static void put_u32(StrBuf *sb, uint32_t v) { sb_append(sb, (const char *)&v, 4); }

static void pad8(StrBuf *sb) {
    static const char zeros[8] = {0};
    if (sb->len % 8) sb_append(sb, zeros, 8 - sb->len % 8);
}

/* Sections are laid out one after another; beginning one ends the one before it. */
typedef struct {
    uint64_t offs[SECTION_COUNT], lens[SECTION_COUNT];
    int open; /* the section being written, or -1 */
} Sections;

static void end_sections(const StrBuf *out, Sections *s) {
    if (s->open >= 0) s->lens[s->open] = out->len - s->offs[s->open];
    s->open = -1;
}

static void begin_section(StrBuf *out, Sections *s, int k) {
    end_sections(out, s);
    pad8(out);
    s->offs[k] = out->len;
    s->open = k;
}

static uint32_t clamp_time(int64_t t) { return t < 0 ? 0 : t > UINT32_MAX ? UINT32_MAX : (uint32_t)t; }

static uint64_t new_generation(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

bool segment_write(const char *path, const SegmentBuilder *b, const StrList *roots, int64_t built_at, Err *err) {
    uint32_t n = b->count;
    WriteCtx ctx = {b};

    /* Distinct names, numbered in name order. */
    uint32_t *by_name = xmalloc(((size_t)n + 1) * sizeof *by_name);
    for (uint32_t i = 0; i < n; i++) by_name[i] = i;
    sort_stable(by_name, n, sizeof *by_name, compare_by_name, &ctx);
    uint32_t *name_id = xmalloc(((size_t)n + 1) * sizeof *name_id);
    U32Vec name_off = {0};
    StrBuf names = {0};
    uint64_t *unicode = xcalloc(bitmap_words(n) + 1, sizeof *unicode);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = by_name[k];
        const FileRecord *r = &b->recs[i];
        const FileRecord *prev = k ? &b->recs[by_name[k - 1]] : NULL;
        bool same = prev && prev->name_len == r->name_len && memcmp(rec_name(b, by_name[k - 1]), rec_name(b, i), r->name_len) == 0;
        if (!same) {
            if (has_non_ascii(rec_name(b, i), r->name_len)) bit_set(unicode, (uint32_t)name_off.len);
            u32vec_push(&name_off, (uint32_t)names.len);
            sb_append(&names, rec_name(b, i), r->name_len);
            sb_putc(&names, '\0');
        }
        name_id[i] = (uint32_t)name_off.len - 1;
    }
    free(by_name);
    uint32_t distinct = (uint32_t)name_off.len;

    uint32_t *order = xmalloc(((size_t)n + 1) * sizeof *order);
    uint32_t *new_id = xmalloc(((size_t)n + 1) * sizeof *new_id);
    path_order(b, order, new_id);

    StrBuf out = {0};
    sb_grow(&out, HEADER_SIZE + names.len + (size_t)n * 32 + (size_t)distinct * 4 + 4096);
    out.len = HEADER_SIZE;
    memset(out.data, 0, HEADER_SIZE);
    Sections sec = {.open = -1};
    begin_section(&out, &sec, SEC_ROOTS);
    for (size_t i = 0; i < roots->len; i++) sb_append(&out, roots->items[i], strlen(roots->items[i]) + 1);
    begin_section(&out, &sec, SEC_NAMES);
    sb_append(&out, names.data ? names.data : "", names.len);
    begin_section(&out, &sec, SEC_NAME_OFF);
    sb_append(&out, (const char *)name_off.data, (size_t)distinct * 4);
    begin_section(&out, &sec, SEC_NAME_ID);
    for (uint32_t i = 0; i < n; i++) put_u32(&out, name_id[order[i]]);
    begin_section(&out, &sec, SEC_PARENT);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = b->recs[order[i]].parent;
        put_u32(&out, p == NO_PARENT ? NO_PARENT : new_id[p]);
    }
    begin_section(&out, &sec, SEC_SIZE);
    for (uint32_t i = 0; i < n; i++) sb_append(&out, (const char *)&b->recs[order[i]].size, 8);
    begin_section(&out, &sec, SEC_MODIFIED);
    for (uint32_t i = 0; i < n; i++) put_u32(&out, clamp_time(b->recs[order[i]].mtime));
    begin_section(&out, &sec, SEC_CREATED);
    for (uint32_t i = 0; i < n; i++) put_u32(&out, clamp_time(b->recs[order[i]].ctime));
    begin_section(&out, &sec, SEC_DIRS);
    uint64_t *dirs = xcalloc(bitmap_words(n) + 1, sizeof *dirs);
    for (uint32_t i = 0; i < n; i++)
        if (b->recs[order[i]].flags & RECORD_DIR) bit_set(dirs, i);
    sb_append(&out, (const char *)dirs, bitmap_words(n) * 8);
    free(dirs);
    begin_section(&out, &sec, SEC_UNICODE);
    sb_append(&out, (const char *)unicode, bitmap_words(distinct) * 8);
    end_sections(&out, &sec);

    uint64_t generation = new_generation();
    uint32_t version = SEGMENT_VERSION;
    char *h = out.data;
    memcpy(h, SEGMENT_MAGIC, 4);
    memcpy(h + 4, &version, 4);
    memcpy(h + 8, &generation, 8);
    memcpy(h + 16, &built_at, 8);
    memcpy(h + 24, &n, 4);
    memcpy(h + 28, &distinct, 4);
    for (int k = 0; k < SECTION_COUNT; k++) {
        memcpy(h + 32 + 16 * k, &sec.offs[k], 8);
        memcpy(h + 40 + 16 * k, &sec.lens[k], 8);
    }
    free(name_id);
    free(order);
    free(new_id);
    free(unicode);
    u32vec_free(&name_off);
    sb_free(&names);

    /* The new journal goes in first and the new file last: a reader ignores a journal of another generation. */
    char journal[12];
    memcpy(journal, JOURNAL_MAGIC, 4);
    memcpy(journal + 4, &generation, 8);
    StrBuf journal_path = {0};
    sb_printf(&journal_path, "%s.journal", path);
    bool ok = write_file_atomic(journal_path.data, journal, sizeof journal, 0644, err) &&
              write_file_atomic(path, out.data, out.len, 0644, err);
    sb_free(&journal_path);
    sb_free(&out);
    return ok;
}

/* ---- reading ---- */

static uint32_t le32(const char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t le64(const char *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/*
 * decode checks a mapped file and points the segment's columns into it,
 * validating every reference an accessor will follow so that a damaged file
 * is refused here rather than crashing a search. It returns false for one.
 */
static bool decode(Segment *s, const char *base, size_t len, StrList *roots, int64_t *built_at) {
    if (len < HEADER_SIZE || memcmp(base, SEGMENT_MAGIC, 4) != 0) return false;
    uint64_t n = le32(base + 24), d = le32(base + 28);
    const uint64_t elem[SECTION_COUNT] = {0, 0, 4, 4, 4, 8, 4, 4, 8, 8};
    const uint64_t count[SECTION_COUNT] = {0, 0, d, n, n, n, n, n, (n + 63) / 64, (d + 63) / 64};
    const char *sec[SECTION_COUNT];
    uint64_t sec_len[SECTION_COUNT];
    for (int k = 0; k < SECTION_COUNT; k++) {
        uint64_t off = le64(base + 32 + 16 * k), l = le64(base + 40 + 16 * k);
        if (off % 8 || off > len || l > len - off || (elem[k] && l != elem[k] * count[k])) return false;
        sec[k] = base + off;
        sec_len[k] = l;
    }
    const char *names = sec[SEC_NAMES];
    uint64_t names_len = sec_len[SEC_NAMES];
    const uint32_t *name_off = (const uint32_t *)sec[SEC_NAME_OFF];
    const uint32_t *name_id = (const uint32_t *)sec[SEC_NAME_ID];
    const uint32_t *parent = (const uint32_t *)sec[SEC_PARENT];
    const int64_t *size = (const int64_t *)sec[SEC_SIZE];
    const uint32_t *modified = (const uint32_t *)sec[SEC_MODIFIED];
    const uint32_t *created = (const uint32_t *)sec[SEC_CREATED];
    const uint64_t *dirs = (const uint64_t *)sec[SEC_DIRS];
    if (d > 0 && (names_len == 0 || names[names_len - 1] != '\0')) return false;
    for (uint64_t k = 0; k < d; k++)
        if (name_off[k] >= names_len || (k > 0 && name_off[k] <= name_off[k - 1])) return false;

    for (uint32_t i = 0; i < n; i++)
        if (name_id[i] >= d || (parent[i] != NO_PARENT && parent[i] >= i)) return false;
    for (const char *p = sec[SEC_ROOTS], *e = p + sec_len[SEC_ROOTS]; p < e; p += strnlen(p, (size_t)(e - p)) + 1)
        if (*p) strlist_push(roots, p);
    *built_at = (int64_t)le64(base + 16);
    s->generation = le64(base + 8);

    s->count = (uint32_t)n;
    s->names = names_len ? names : "";
    s->col = (Columns){.name_off = name_off,
                       .distinct = (uint32_t)d,
                       .names_len = (size_t)names_len,
                       .name_id = name_id,
                       .parent = parent,
                       .modified = modified,
                       .created = created,
                       .size = size,
                       .dirs = dirs};
    return true;
}

Segment *segment_open(const char *path, StrList *roots, int64_t *built_at, bool *missing, Err *err) {
    *missing = false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *missing = errno == ENOENT;
        err_set(err, "%s: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < HEADER_SIZE) {
        close(fd);
        err_set(err, "%s: corrupt index file", path);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        err_set(err, "%s: mmap: %s", path, strerror(errno));
        return NULL;
    }
    if (memcmp(map, SEGMENT_MAGIC, 4) == 0 && le32((const char *)map + 4) != SEGMENT_VERSION) {
        munmap(map, len);
        err_set(err, "index was written by an incompatible eind version; run `eind index`");
        return NULL;
    }
    Segment *s = xcalloc(1, sizeof *s);
    atomic_init(&s->refs, 1);
    if (!decode(s, map, len, roots, built_at)) {
        munmap(map, len);
        free(s);
        err_set(err, "%s: corrupt index file", path);
        return NULL;
    }
    s->map = map;
    s->map_len = len;
    return s;
}

void segment_set_times(Segment *s, uint32_t i, int64_t size, int64_t mtime, int64_t ctime) {
    if (!s->updated) {
        s->updated = xcalloc(bitmap_words(s->count) + 1, sizeof *s->updated);
        s->updated_size = xmalloc((s->count + 1) * sizeof *s->updated_size);
        s->updated_mtime = xmalloc((s->count + 1) * sizeof *s->updated_mtime);
        s->updated_ctime = xmalloc((s->count + 1) * sizeof *s->updated_ctime);
    }
    bit_set(s->updated, i);
    s->updated_size[i] = size;
    s->updated_mtime[i] = mtime;
    s->updated_ctime[i] = ctime;
}

void segment_retain(Segment *s) { atomic_fetch_add(&s->refs, 1); }

void segment_release(Segment *s) {
    if (!s || atomic_fetch_sub(&s->refs, 1) != 1) return;
    if (s->map) munmap(s->map, s->map_len);
    free(s->owned_recs);
    free(s->owned_names);
    free(s->updated);
    free(s->updated_size);
    free(s->updated_mtime);
    free(s->updated_ctime);
    free(s);
}

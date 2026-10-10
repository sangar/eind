#include "search.h"

#include <regex.h>
#include <string.h>

#include "mc/container/sort.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "mc/text/utf8.h"

enum { CANCEL_CHECK_INTERVAL = 8192, MIN_CHUNK = 4096 };

Error sort_key_parse(String s, SortKey *out, Err *err) {
    static const struct {
        const char *name;
        SortKey key;
    } keys[] = {
        {"path", SORT_PATH},          {"name", SORT_NAME},          {"size", SORT_SIZE},
        {"dm", SORT_MODIFIED},        {"modified", SORT_MODIFIED},  {"date-modified", SORT_MODIFIED},
        {"dc", SORT_CREATED},         {"created", SORT_CREATED},    {"date-created", SORT_CREATED},
        {"ext", SORT_EXT},            {"extension", SORT_EXT},      {"relevance", SORT_RELEVANCE},
        {"rank", SORT_RELEVANCE},
    };
    for (size_t i = 0; i < countof(keys); i++) {
        if (str_equal_ignore_case(s, S(keys[i].name))) {
            *out = keys[i].key;
            return ERR_OK;
        }
    }
    return err_set(err, ERR_INVALID_ARGUMENT, "unknown sort key \"%.*s\" (use path, name, size, dm, dc, ext or relevance)",
                   (int)s.len, s.data);
}

/* ---- matchers ---- */

typedef enum {
    M_ALL,
    M_NONE,
    M_AND,
    M_OR,
    M_NOT,
    M_TEXT,
    M_EXT,
    M_SIZE,
    M_MODIFIED,
    M_CREATED,
    M_NAMELEN,
    M_DEPTH,
    M_ISDIR,
    M_PARENT,
    M_INFOLDER,
} MatcherKind;

/*
 * A wildcard pattern simple enough to match without a regex: "*x", "x*" and
 * "*x*" are by far the most typed, and a leading "*" alone would otherwise
 * run a regex over every name in the index.
 */
typedef enum { GLOB_NONE, GLOB_SUFFIX, GLOB_PREFIX, GLOB_CONTAINS } GlobKind;

typedef struct Matcher {
    MatcherKind kind;
    struct Matcher **kids;
    uint32_t kid_count;
    struct Matcher *kid;
    String needle;
    regex_t *re;
    TextMode mode;
    GlobKind glob;
    bool path, cased;
    const String *exts;
    uint32_t ext_count;
    Range range;
    bool dir;
    uint32_t dir_id;
} Matcher;

typedef struct {
    regex_t **items;
    size_t count;
    size_t capacity;
} RegexList;

/* Compiled is a query's matcher tree; it and its regexes live in arena until compiled_free. */
typedef struct {
    Arena *arena;
    Matcher *root;
    RegexList regexes;
} Compiled;

/* MatchCtx carries one record through the matcher tree, building its path only if a matcher asks. */
typedef struct {
    const Snapshot *s;
    uint32_t id;
    StringBuilder path, path_lower;
    bool have_path, have_lower;
} MatchCtx;

static String ctx_path(MatchCtx *c) {
    if (!c->have_path) {
        snap_path(c->s, c->id, &c->path);
        c->have_path = true;
    }
    return (String){c->path.data, c->path.len};
}

static unsigned char fold_byte(char c) { return (unsigned char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }

static String ctx_path_lower(MatchCtx *c) {
    if (!c->have_lower) {
        String path = ctx_path(c);
        StringBuilder *lower = &c->path_lower;
        if (lower->capacity <= path.len) *lower = str_builder_create(lower->arena, 2 * path.len + 1);
        for (size_t i = 0; i < path.len; i++) lower->data[i] = (char)fold_byte(path.data[i]);
        lower->data[path.len] = '\0';
        lower->len = path.len;
        c->have_lower = true;
    }
    return (String){c->path_lower.data, c->path_lower.len};
}

/* Bytes of multi-byte UTF-8 sequences count as letters; underscores separate words so ww:main finds main_test.go. */
static bool is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}

/*
 * The name matchers fold case while they compare, so a scan reads each name
 * once in place and copies nothing. The needle is already lowercase when
 * fold is set.
 */
static bool fold_equal_at(const char *hay, String needle, bool fold) {
    for (size_t k = 0; k < needle.len; k++)
        if ((fold ? fold_byte(hay[k]) : (unsigned char)hay[k]) != (unsigned char)needle.data[k]) return false;
    return true;
}

/* find_in stores where needle first occurs in hay. */
static bool find_in(String hay, String needle, bool fold, size_t *at) {
    if (!fold) return str_find(hay, needle, at);
    if (needle.len == 0) {
        *at = 0;
        return true;
    }
    unsigned char first = (unsigned char)needle.data[0];
    String rest = str_slice(needle, 1, needle.len);
    for (size_t i = 0; i + needle.len <= hay.len; i++) {
        if (fold_byte(hay.data[i]) == first && fold_equal_at(hay.data + i + 1, rest, true)) {
            *at = i;
            return true;
        }
    }
    return false;
}

static bool contains_word(String hay, String needle, bool fold) {
    if (needle.len == 0) return true;
    for (size_t start = 0, at; find_in(str_slice(hay, start, hay.len), needle, fold, &at); start += at + 1) {
        size_t begin = start + at, end = begin + needle.len;
        bool before = begin == 0 || !is_word_byte((unsigned char)hay.data[begin - 1]);
        bool after = end >= hay.len || !is_word_byte((unsigned char)hay.data[end]);
        if (before && after) return true;
    }
    return false;
}

static bool in_range(int64_t v, Range r) { return v >= r.lo && v <= r.hi; }

static int64_t depth_of(String path) {
    while (path.len > 0 && path.data[path.len - 1] == '/') path.len--;
    return (int64_t)str_count_char(path, '/');
}

static bool match_text(const Matcher *m, MatchCtx *c) {
    String hay;
    bool fold = !m->cased;
    size_t at;
    if (m->re) {
        hay = m->path ? ctx_path(c) : snap_name_view(c->s, c->id);
        return regexec(m->re, hay.data, 0, nullptr, 0) == 0;
    }
    if (m->path) {
        hay = m->cased ? ctx_path(c) : ctx_path_lower(c);
        fold = false; /* the path copy is already lowercased */
    } else {
        hay = snap_name_view(c->s, c->id);
    }
    size_t n = m->needle.len;
    switch (m->glob) {
    case GLOB_SUFFIX: return n <= hay.len && fold_equal_at(hay.data + hay.len - n, m->needle, fold);
    case GLOB_PREFIX: return n <= hay.len && fold_equal_at(hay.data, m->needle, fold);
    case GLOB_CONTAINS: return find_in(hay, m->needle, fold, &at);
    case GLOB_NONE: break;
    }
    switch (m->mode) {
    case TEXT_WHOLENAME: return n == hay.len && fold_equal_at(hay.data, m->needle, fold);
    case TEXT_WHOLEWORD: return contains_word(hay, m->needle, fold);
    default: return find_in(hay, m->needle, fold, &at);
    }
}

static bool matches(const Matcher *m, MatchCtx *c) {
    switch (m->kind) {
    case M_ALL: return true;
    case M_NONE: return false;
    case M_AND:
        for (uint32_t k = 0; k < m->kid_count; k++)
            if (!matches(m->kids[k], c)) return false;
        return true;
    case M_OR:
        for (uint32_t k = 0; k < m->kid_count; k++)
            if (matches(m->kids[k], c)) return true;
        return false;
    case M_NOT: return !matches(m->kid, c);
    case M_TEXT: return match_text(m, c);
    case M_EXT: {
        String ext = snap_ext(c->s, c->id);
        for (uint32_t k = 0; k < m->ext_count; k++)
            if (str_equal_ignore_case(ext, m->exts[k])) return true;
        return false;
    }
    case M_SIZE: return in_range(snap_size(c->s, c->id), m->range);
    case M_MODIFIED: return in_range(snap_mtime(c->s, c->id), m->range);
    case M_CREATED: return in_range(snap_ctime(c->s, c->id), m->range);
    case M_NAMELEN: return in_range((int64_t)utf8_count(snap_name_view(c->s, c->id)), m->range);
    case M_DEPTH: return in_range(depth_of(ctx_path(c)), m->range);
    case M_ISDIR: return snap_is_dir(c->s, c->id) == m->dir;
    case M_PARENT: return snap_parent(c->s, c->id) == m->dir_id;
    case M_INFOLDER:
        for (uint32_t p = snap_parent(c->s, c->id); p != NO_PARENT; p = snap_parent(c->s, p))
            if (p == m->dir_id) return true;
        return false;
    }
    return false;
}

static GlobKind simple_glob(String pattern, String *inner) {
    if (str_contains(pattern, S("?"))) return GLOB_NONE;
    size_t lo = 0, hi = pattern.len;
    while (lo < pattern.len && pattern.data[lo] == '*') lo++;
    while (hi > lo && pattern.data[hi - 1] == '*') hi--;
    *inner = str_slice(pattern, lo, hi);
    if (str_contains(*inner, S("*")) || (lo == 0 && hi == pattern.len)) return GLOB_NONE;
    if (lo > 0 && hi < pattern.len) return GLOB_CONTAINS;
    return lo > 0 ? GLOB_SUFFIX : GLOB_PREFIX;
}

/* wildcard_regex anchors the pattern: a wildcard term must match the whole name. */
static void wildcard_regex(StringBuilder *out, String glob) {
    str_builder_append_char(out, '^');
    for (size_t i = 0; i < glob.len; i++) {
        char c = glob.data[i];
        if (c == '*') {
            str_builder_append(out, S(".*"));
        } else if (c == '?') {
            str_builder_append_char(out, '.');
        } else {
            if (str_contains_any((String){&c, 1}, S(".[]()*+?{}|^$\\"))) str_builder_append_char(out, '\\');
            str_builder_append_char(out, c);
        }
    }
    str_builder_append_char(out, '$');
}

/* posix_regex rewrites the Perl-style classes people type (\d, \w, \s) into POSIX bracket expressions. */
static void posix_regex(StringBuilder *out, String re) {
    bool in_bracket = false;
    for (size_t i = 0; i < re.len; i++) {
        char c = re.data[i];
        if (c == '\\' && i + 1 < re.len) {
            const char *cls = nullptr;
            bool negated = false;
            switch (re.data[i + 1]) {
            case 'd': cls = "0-9"; break;
            case 'D': cls = "0-9", negated = true; break;
            case 'w': cls = "[:alnum:]_"; break;
            case 'W': cls = "[:alnum:]_", negated = true; break;
            case 's': cls = "[:space:]"; break;
            case 'S': cls = "[:space:]", negated = true; break;
            }
            if (cls && in_bracket) {
                str_builder_append(out, S(cls));
            } else if (cls) {
                str_builder_append_format(out, "[%s%s]", negated ? "^" : "", cls);
            } else {
                str_builder_append(out, str_slice(re, i, i + 2));
            }
            i++;
            continue;
        }
        if (c == '[' && !in_bracket) {
            in_bracket = true;
            str_builder_append_char(out, c);
            if (i + 1 < re.len && re.data[i + 1] == '^') str_builder_append_char(out, re.data[++i]);
            if (i + 1 < re.len && re.data[i + 1] == ']') str_builder_append_char(out, re.data[++i]);
            continue;
        }
        if (c == ']' && in_bracket) in_bracket = false;
        str_builder_append_char(out, c);
    }
}

[[nodiscard]] static Error compile_regex(Compiled *cc, Matcher *m, String pattern, String original, bool cased, Err *err) {
    regex_t *re = arena_push(cc->arena, sizeof *re);
    StringBuilder posix = str_builder_create(cc->arena, pattern.len + 16);
    posix_regex(&posix, pattern);
    int rc = regcomp(re, str_builder_finish(&posix).data, REG_EXTENDED | REG_NOSUB | (cased ? 0 : REG_ICASE));
    if (rc != 0) {
        char why[256];
        regerror(rc, re, why, sizeof why);
        return err_set(err, ERR_PARSE, "invalid regex \"%.*s\": %s", (int)original.len, original.data, why);
    }
    cc->regexes.items = arena_grow(cc->arena, cc->regexes.items, &cc->regexes.capacity, cc->regexes.count,
                                   sizeof *cc->regexes.items);
    cc->regexes.items[cc->regexes.count++] = re;
    m->re = re;
    return ERR_OK;
}

static String lowered(Arena *a, String s, bool keep_case) { return keep_case ? str_copy(a, s) : str_lower_ascii(a, s); }

[[nodiscard]] static Error compile(Compiled *cc, const Snapshot *s, const QueryNode *n, Matcher **out, Err *err);
/* resolve_dir finds the directory record for a path, ignoring case. */
static bool resolve_dir(Arena *arena, const Snapshot *s, String path, uint32_t *out);

[[nodiscard]] static Error compile_text(Compiled *cc, Matcher *m, const QueryNode *n, Err *err) {
    m->kind = M_TEXT;
    m->mode = n->mode;
    m->path = n->match_path;
    m->cased = n->case_sensitive;
    if (n->mode == TEXT_WILDCARD) {
        String inner;
        GlobKind kind = simple_glob(n->text, &inner);
        if (kind != GLOB_NONE) {
            m->glob = kind;
            m->needle = lowered(cc->arena, inner, n->case_sensitive);
            return ERR_OK;
        }
    }
    if (n->mode == TEXT_REGEX) return compile_regex(cc, m, n->text, n->text, n->case_sensitive, err);
    if (n->mode == TEXT_WILDCARD) {
        StringBuilder pattern = str_builder_create(cc->arena, n->text.len + 8);
        wildcard_regex(&pattern, n->text);
        return compile_regex(cc, m, str_builder_finish(&pattern), n->text, n->case_sensitive, err);
    }
    m->needle = lowered(cc->arena, n->text, n->case_sensitive);
    return ERR_OK;
}

[[nodiscard]] static Error compile(Compiled *cc, const Snapshot *s, const QueryNode *n, Matcher **out, Err *err) {
    Matcher *m = arena_push(cc->arena, sizeof *m);
    *out = m;
    Error e = ERR_OK;
    switch (n->kind) {
    case Q_AND:
    case Q_OR:
        if (n->kind == Q_AND && n->kid_count == 0) {
            m->kind = M_ALL;
            return ERR_OK;
        }
        m->kind = n->kind == Q_AND ? M_AND : M_OR;
        m->kid_count = n->kid_count;
        m->kids = arena_push(cc->arena, (n->kid_count + 1) * sizeof *m->kids);
        for (uint32_t k = 0; k < n->kid_count && e == ERR_OK; k++) e = compile(cc, s, n->kids[k], &m->kids[k], err);
        return e;
    case Q_NOT:
        m->kind = M_NOT;
        return compile(cc, s, n->kid, &m->kid, err);
    case Q_TEXT: return compile_text(cc, m, n, err);
    case Q_EXT:
        m->kind = M_EXT;
        m->exts = n->exts;
        m->ext_count = n->ext_count;
        return ERR_OK;
    case Q_SIZE: m->kind = M_SIZE; break;
    case Q_MODIFIED: m->kind = M_MODIFIED; break;
    case Q_CREATED: m->kind = M_CREATED; break;
    case Q_NAMELEN: m->kind = M_NAMELEN; break;
    case Q_DEPTH: m->kind = M_DEPTH; break;
    case Q_ISDIR:
        m->kind = M_ISDIR;
        m->dir = n->dir;
        return ERR_OK;
    case Q_PARENT:
    case Q_INFOLDER:
        m->kind = resolve_dir(cc->arena, s, n->path, &m->dir_id) ? (n->kind == Q_PARENT ? M_PARENT : M_INFOLDER) : M_NONE;
        return ERR_OK;
    }
    m->range = n->range;
    return ERR_OK;
}

static void compiled_free(Compiled *cc) {
    for (size_t i = 0; i < cc->regexes.count; i++) regfree(cc->regexes.items[i]);
    arena_destroy(cc->arena);
}

/* ---- parallel evaluation ---- */

/* A ChunkScan is one chunk's private state: its hits and path buffers live in its own arena. */
typedef struct {
    Arena *arena;
    IdList hits;
} ChunkScan;

typedef struct {
    const Snapshot *s;
    const Matcher *root;
    Cancel *cancel;
    ChunkScan *chunks;
} ScanJob;

static void scan_chunk(void *context, size_t lo, size_t hi, size_t chunk) {
    ScanJob *job = context;
    ChunkScan *out = &job->chunks[chunk];
    MatchCtx c = {.s = job->s, .path = str_builder_create(out->arena, 256), .path_lower = str_builder_create(out->arena, 256)};
    for (size_t i = lo; i < hi; i++) {
        if ((i - lo) % CANCEL_CHECK_INTERVAL == 0 && job->cancel && cancel_requested(job->cancel)) break;
        uint32_t id = (uint32_t)i;
        if (!snap_live(job->s, id)) continue;
        c.id = id;
        c.have_path = c.have_lower = false;
        if (matches(job->root, &c)) idlist_push(out->arena, &out->hits, id);
    }
}

static void scan(ThreadPool *pool, Arena *scratch, ScanJob job, size_t n, Arena *arena, IdList *hits) {
    if (n == 0) return;
    ParallelPlan plan = parallel_plan(pool, n, MIN_CHUNK);
    job.chunks = arena_push(scratch, plan.chunks * sizeof *job.chunks);
    for (size_t k = 0; k < plan.chunks; k++) job.chunks[k].arena = arena_create(16 * 1024);
    threadpool_run_chunks(pool, scratch, plan, scan_chunk, &job);
    size_t total = 0;
    for (size_t k = 0; k < plan.chunks; k++) total += job.chunks[k].hits.count;
    hits->items = arena_push(arena, (total + 1) * sizeof *hits->items);
    hits->capacity = total + 1;
    for (size_t k = 0; k < plan.chunks; k++) {
        const IdList *part = &job.chunks[k].hits;
        if (part->count) memcpy(hits->items + hits->count, part->items, part->count * sizeof *part->items);
        hits->count += part->count;
        arena_destroy(job.chunks[k].arena);
    }
}

Error search_run(ThreadPool *pool, Arena *arena, const Snapshot *s, const QueryNode *query, Cancel *cancel, IdList *hits,
                 Err *err) {
    *hits = (IdList){0};
    Compiled cc = {.arena = arena_create(16 * 1024)};
    Error e = compile(&cc, s, query, &cc.root, err);
    if (e == ERR_OK) {
        ScanJob job = {.s = s, .root = cc.root, .cancel = cancel};
        scan(pool, cc.arena, job, s->total, arena, hits);
        if (cancel && cancel_requested(cancel)) e = ERR_CANCELLED;
    }
    compiled_free(&cc);
    return e;
}

/* ---- ordering ---- */

typedef struct {
    const Snapshot *s;
    SortKey key;
    bool descending;
} SortCtx;

static int compare_i64(int64_t a, int64_t b) { return a < b ? -1 : a > b; }

static int compare_names(const Snapshot *s, uint32_t a, uint32_t b) {
    String na = snap_name_view(s, a), nb = snap_name_view(s, b);
    int c = str_compare_ignore_case(na, nb);
    if (!c) c = str_compare(na, nb);
    return c ? c : compare_i64(a, b);
}

static int compare_by_key(const void *ctx, const void *pa, const void *pb) {
    const SortCtx *sc = ctx;
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    if (sc->descending) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    const Snapshot *s = sc->s;
    int c = 0;
    switch (sc->key) {
    case SORT_SIZE: c = compare_i64(snap_size(s, a), snap_size(s, b)); break;
    case SORT_MODIFIED: c = compare_i64(snap_mtime(s, a), snap_mtime(s, b)); break;
    case SORT_CREATED: c = compare_i64(snap_ctime(s, a), snap_ctime(s, b)); break;
    case SORT_EXT: c = str_compare_ignore_case(snap_ext(s, a), snap_ext(s, b)); break;
    default: break;
    }
    return c ? c : compare_names(s, a, b);
}

typedef struct {
    String path;
    uint32_t hit;
} KeyedPath;

static int compare_keyed(const void *ctx, const void *pa, const void *pb) {
    const SortCtx *sc = ctx;
    const KeyedPath *a = pa, *b = pb;
    if (sc->descending) {
        const KeyedPath *t = a;
        a = b;
        b = t;
    }
    int c = str_compare(a->path, b->path);
    return c ? c : compare_names(sc->s, a->hit, b->hit);
}

/* top_by_path builds each lowercased path once, which beats walking parent chains on every comparison. */
static size_t top_by_path(Arena *scratch, const Snapshot *s, uint32_t *hits, size_t n, size_t k, bool descending) {
    KeyedPath *rows = arena_push(scratch, (n + 1) * sizeof *rows);
    StringBuilder path = str_builder_create(scratch, 256);
    for (size_t i = 0; i < n; i++) {
        rows[i].path = str_lower_ascii(scratch, snap_path(s, hits[i], &path));
        rows[i].hit = hits[i];
    }
    SortCtx sc = {.s = s, .key = SORT_PATH, .descending = descending};
    k = sort_top(scratch, rows, n, sizeof *rows, k, compare_keyed, &sc);
    for (size_t i = 0; i < k; i++) hits[i] = rows[i].hit;
    return k;
}

/*
 * The index file stores its records in path order (docs/file-format.md), so
 * hits that all come from the base segment, in id order as the scan leaves
 * them, are already sorted by path.
 */
static bool in_path_order(const Snapshot *s, const uint32_t *hits, size_t n) {
    if (n == 0) return true;
    if (hits[n - 1] >= s->segs[0]->count) return false;
    for (size_t i = 1; i < n; i++)
        if (hits[i] <= hits[i - 1]) return false;
    return true;
}

static void reverse(uint32_t *hits, size_t n) {
    for (size_t i = 0, j = n; i + 1 < j; i++, j--) {
        uint32_t t = hits[i];
        hits[i] = hits[j - 1];
        hits[j - 1] = t;
    }
}

static size_t kept_count(size_t n, int64_t keep) { return keep < 0 || (uint64_t)keep > n ? n : (size_t)keep; }

size_t search_top(Arena *scratch, const Snapshot *s, uint32_t *hits, size_t n, SortKey key, bool descending, int64_t keep) {
    size_t k = kept_count(n, keep);
    if (key == SORT_RELEVANCE) key = SORT_NAME;
    if (key == SORT_PATH && in_path_order(s, hits, n)) {
        if (descending) reverse(hits, n);
        return k;
    }
    if (key == SORT_PATH) return top_by_path(scratch, s, hits, n, k, descending);
    SortCtx sc = {.s = s, .key = key, .descending = descending};
    return sort_top(scratch, hits, n, sizeof *hits, k, compare_by_key, &sc);
}

/* ---- relevance ---- */

enum { SCORE_EXACT = 4, SCORE_PREFIX = 3, SCORE_WORD_START = 2, SCORE_CONTAINS = 1 };

typedef struct {
    uint32_t hit;
    int score, depth;
} Ranked;

static bool starts_word_in(String hay, String needle) {
    for (size_t start = 0, at; str_find(str_slice(hay, start, hay.len), needle, &at); start += at + 1) {
        size_t begin = start + at;
        if (begin == 0 || !is_word_byte((unsigned char)hay.data[begin - 1])) return true;
    }
    return false;
}

static int score(String lower_name, const StringList *terms) {
    size_t dot;
    String stem = str_find_last_char(lower_name, '.', &dot) && dot > 0 ? str_slice(lower_name, 0, dot) : lower_name;
    int total = 0;
    for (size_t i = 0; i < terms->count; i++) {
        String t = terms->items[i];
        if (str_equal(lower_name, t) || str_equal(stem, t)) {
            total += SCORE_EXACT;
        } else if (str_starts_with(lower_name, t)) {
            total += SCORE_PREFIX;
        } else if (starts_word_in(lower_name, t)) {
            total += SCORE_WORD_START;
        } else if (str_contains(lower_name, t)) {
            total += SCORE_CONTAINS;
        }
    }
    return total;
}

static int depth_in_tree(const Snapshot *s, uint32_t id) {
    int d = 0;
    for (uint32_t p = snap_parent(s, id); p != NO_PARENT; p = snap_parent(s, p)) d++;
    return d;
}

/* plain_terms collects the name terms a user typed; negated, path, wildcard and regex terms carry no ranking signal. */
static void plain_terms(Arena *a, const QueryNode *n, StringList *terms) {
    if (n->kind == Q_AND || n->kind == Q_OR) {
        for (uint32_t k = 0; k < n->kid_count; k++) plain_terms(a, n->kids[k], terms);
        return;
    }
    if (n->kind != Q_TEXT || n->match_path || n->mode == TEXT_WILDCARD || n->mode == TEXT_REGEX) return;
    strlist_push(a, terms, str_lower_ascii(a, n->text));
}

static int compare_ranked(const void *ctx, const void *pa, const void *pb) {
    const Snapshot *s = ctx;
    const Ranked *a = pa, *b = pb;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    int c = str_compare_ignore_case(snap_name_view(s, a->hit), snap_name_view(s, b->hit));
    return c ? c : compare_i64(a->hit, b->hit);
}

size_t search_rank(Arena *scratch, const Snapshot *s, uint32_t *hits, size_t n, const QueryNode *query, int64_t keep) {
    size_t k = kept_count(n, keep);
    StringList terms = {0};
    plain_terms(scratch, query, &terms);
    Ranked *rows = arena_push(scratch, (n + 1) * sizeof *rows);
    StringBuilder lower = str_builder_create(scratch, 256);
    for (size_t i = 0; i < n; i++) {
        rows[i] = (Ranked){.hit = hits[i]};
        if (terms.count) {
            lower.len = 0;
            str_builder_append(&lower, snap_name_view(s, hits[i]));
            for (size_t b = 0; b < lower.len; b++) lower.data[b] = (char)fold_byte(lower.data[b]);
            rows[i].score = score((String){lower.data, lower.len}, &terms);
            rows[i].depth = depth_in_tree(s, hits[i]);
        }
    }
    k = sort_top(scratch, rows, n, sizeof *rows, k, compare_ranked, s);
    for (size_t i = 0; i < k; i++) hits[i] = rows[i].hit;
    return k;
}

/* ---- folders ---- */

static bool find_child_dir(const Snapshot *s, uint32_t parent, String name, uint32_t *out) {
    for (uint32_t id = parent + 1; id < s->total; id++) {
        if (snap_parent(s, id) == parent && snap_is_dir(s, id) && snap_live(s, id) &&
            str_equal_ignore_case(snap_name_view(s, id), name)) {
            *out = id;
            return true;
        }
    }
    return false;
}

static bool resolve_dir(Arena *arena, const Snapshot *s, String path, uint32_t *out) {
    String target = str_lower_ascii(arena, path_absolute(arena, path_expand_home(arena, path, env_home(arena))));
    for (uint32_t id = 0; id < s->total; id++) {
        if (snap_parent(s, id) != NO_PARENT || !snap_is_dir(s, id) || !snap_live(s, id)) continue;
        String root = str_lower_ascii(arena, snap_name_view(s, id));
        while (root.len > 1 && root.data[root.len - 1] == '/') root.len--;
        if (str_equal(target, root)) {
            *out = id;
            return true;
        }
        String below;
        bool slash_root = str_equal(root, S("/"));
        if (slash_root ? !str_starts_with(target, root) : !path_relative(root, target, &below)) continue;
        if (slash_root) below = str_slice(target, 1, target.len);
        uint32_t cur = id;
        bool found = true;
        String part, rest = below;
        while (found && rest.len) {
            str_cut(rest, '/', &part, &rest);
            if (part.len) found = find_child_dir(s, cur, part, &cur);
        }
        if (found) {
            *out = cur;
            return true;
        }
    }
    return false;
}

#include "search.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../core/arena.h"
#include "../core/sort.h"
#include "../core/threadpool.h"

#define CANCEL_CHECK_INTERVAL 8192
#define MIN_CHUNK 4096

bool sort_key_parse(const char *s, SortKey *out, Err *err) {
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
        if (strcasecmp(s, keys[i].name) == 0) {
            *out = keys[i].key;
            return true;
        }
    }
    err_set(err, "unknown sort key \"%s\" (use path, name, size, dm, dc, ext or relevance)", s);
    return false;
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
    const char *needle;
    size_t needle_len;
    regex_t *re;
    TextMode mode;
    GlobKind glob;
    bool path, cased;
    const char **exts;
    uint32_t ext_count;
    Range range;
    bool dir;
    uint32_t dir_id;
} Matcher;

typedef struct {
    Arena arena;
    Matcher *root;
    regex_t **regexes;
    size_t regex_count, regex_cap;
} Compiled;

/* MatchCtx carries one record through the matcher tree, building its path only if a matcher asks. */
typedef struct {
    const Snapshot *s;
    uint32_t id;
    StrBuf path, path_lower;
    bool have_path, have_lower;
} MatchCtx;

static const char *ctx_path(MatchCtx *c) {
    if (!c->have_path) {
        snap_path(c->s, c->id, &c->path);
        c->have_path = true;
    }
    return c->path.data;
}

static const char *ctx_path_lower(MatchCtx *c) {
    if (!c->have_lower) {
        const char *p = ctx_path(c);
        sb_clear(&c->path_lower);
        sb_append(&c->path_lower, p, c->path.len);
        ascii_lower(c->path_lower.data, c->path_lower.data, c->path_lower.len);
        c->have_lower = true;
    }
    return c->path_lower.data;
}

/* Bytes of multi-byte UTF-8 sequences count as letters; underscores separate words so ww:main finds main_test.go. */
static bool is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}

static bool contains_word(const char *hay, const char *needle, size_t needle_len) {
    if (needle_len == 0) return true;
    size_t hay_len = strlen(hay);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++) {
        size_t start = (size_t)(p - hay), end = start + needle_len;
        bool before = start == 0 || !is_word_byte((unsigned char)hay[start - 1]);
        bool after = end >= hay_len || !is_word_byte((unsigned char)hay[end]);
        if (before && after) return true;
    }
    return false;
}

static int64_t depth_of(const char *path) {
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;
    int64_t d = 0;
    for (size_t i = 0; i < n; i++)
        if (path[i] == '/') d++;
    return d;
}

static bool match_text(const Matcher *m, MatchCtx *c) {
    const char *hay;
    if (m->re) {
        hay = m->path ? ctx_path(c) : snap_name(c->s, c->id);
        return regexec(m->re, hay, 0, NULL, 0) == 0;
    }
    if (m->path) {
        hay = m->cased ? ctx_path(c) : ctx_path_lower(c);
    } else {
        hay = m->cased ? snap_name(c->s, c->id) : snap_lower(c->s, c->id);
    }
    switch (m->glob) {
    case GLOB_SUFFIX: return has_suffix(hay, m->needle);
    case GLOB_PREFIX: return strncmp(hay, m->needle, m->needle_len) == 0;
    case GLOB_CONTAINS: return strstr(hay, m->needle) != NULL;
    case GLOB_NONE: break;
    }
    switch (m->mode) {
    case TEXT_WHOLENAME: return strcmp(hay, m->needle) == 0;
    case TEXT_WHOLEWORD: return contains_word(hay, m->needle, m->needle_len);
    default: return strstr(hay, m->needle) != NULL;
    }
}

static bool matches(const Matcher *m, MatchCtx *c) {
    const FileRecord *r;
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
        size_t len;
        const char *ext = snap_ext(c->s, c->id, &len);
        for (uint32_t k = 0; k < m->ext_count; k++)
            if (strcmp(m->exts[k], ext) == 0) return true;
        return false;
    }
    case M_SIZE: r = snap_record(c->s, c->id); return r->size >= m->range.lo && r->size <= m->range.hi;
    case M_MODIFIED: r = snap_record(c->s, c->id); return r->mtime >= m->range.lo && r->mtime <= m->range.hi;
    case M_CREATED: r = snap_record(c->s, c->id); return r->ctime >= m->range.lo && r->ctime <= m->range.hi;
    case M_NAMELEN: {
        r = snap_record(c->s, c->id);
        int64_t len = utf8_count(snap_name(c->s, c->id), r->name_len);
        return len >= m->range.lo && len <= m->range.hi;
    }
    case M_DEPTH: {
        int64_t d = depth_of(ctx_path(c));
        return d >= m->range.lo && d <= m->range.hi;
    }
    case M_ISDIR: return record_is_dir(snap_record(c->s, c->id)) == m->dir;
    case M_PARENT: return snap_record(c->s, c->id)->parent == m->dir_id;
    case M_INFOLDER:
        for (uint32_t p = snap_record(c->s, c->id)->parent; p != NO_PARENT; p = snap_record(c->s, p)->parent)
            if (p == m->dir_id) return true;
        return false;
    }
    return false;
}

static GlobKind simple_glob(const char *pattern, const char **inner, size_t *inner_len) {
    if (strchr(pattern, '?')) return GLOB_NONE;
    size_t n = strlen(pattern);
    size_t lo = 0, hi = n;
    while (lo < n && pattern[lo] == '*') lo++;
    while (hi > lo && pattern[hi - 1] == '*') hi--;
    if (memchr(pattern + lo, '*', hi - lo) || (lo == 0 && hi == n)) return GLOB_NONE;
    *inner = pattern + lo;
    *inner_len = hi - lo;
    if (lo > 0 && hi < n) return GLOB_CONTAINS;
    return lo > 0 ? GLOB_SUFFIX : GLOB_PREFIX;
}

/* wildcard_regex anchors the pattern: a wildcard term must match the whole name. */
static void wildcard_regex(StrBuf *out, const char *glob) {
    sb_putc(out, '^');
    for (const char *p = glob; *p; p++) {
        if (*p == '*') {
            sb_puts(out, ".*");
        } else if (*p == '?') {
            sb_putc(out, '.');
        } else {
            if (strchr(".[]()*+?{}|^$\\", *p)) sb_putc(out, '\\');
            sb_putc(out, *p);
        }
    }
    sb_putc(out, '$');
}

/* posix_regex rewrites the Perl-style classes people type (\d, \w, \s) into POSIX bracket expressions. */
static void posix_regex(StrBuf *out, const char *re) {
    bool in_bracket = false;
    for (const char *p = re; *p; p++) {
        if (*p == '\\' && p[1]) {
            const char *cls = NULL;
            bool negated = false;
            switch (p[1]) {
            case 'd': cls = "0-9"; break;
            case 'D': cls = "0-9", negated = true; break;
            case 'w': cls = "[:alnum:]_"; break;
            case 'W': cls = "[:alnum:]_", negated = true; break;
            case 's': cls = "[:space:]"; break;
            case 'S': cls = "[:space:]", negated = true; break;
            }
            if (cls && in_bracket) {
                sb_puts(out, cls);
            } else if (cls) {
                sb_printf(out, "[%s%s]", negated ? "^" : "", cls);
            } else {
                sb_append(out, p, 2);
            }
            p++;
            continue;
        }
        if (*p == '[' && !in_bracket) {
            in_bracket = true;
            sb_putc(out, *p);
            if (p[1] == '^') sb_putc(out, *++p);
            if (p[1] == ']') sb_putc(out, *++p);
            continue;
        }
        if (*p == ']' && in_bracket) in_bracket = false;
        sb_putc(out, *p);
    }
}

static bool compile_regex(Compiled *cc, Matcher *m, const char *pattern, const char *original, bool cased, Err *err) {
    regex_t *re = xmalloc(sizeof *re);
    StrBuf posix = {0};
    posix_regex(&posix, pattern);
    int rc = regcomp(re, sb_cstr(&posix), REG_EXTENDED | REG_NOSUB | (cased ? 0 : REG_ICASE));
    sb_free(&posix);
    if (rc != 0) {
        char why[256];
        regerror(rc, re, why, sizeof why);
        err_set(err, "invalid regex \"%s\": %s", original, why);
        free(re);
        return false;
    }
    if (cc->regex_count == cc->regex_cap) {
        cc->regex_cap = cc->regex_cap ? cc->regex_cap * 2 : 4;
        cc->regexes = xrealloc(cc->regexes, cc->regex_cap * sizeof *cc->regexes);
    }
    cc->regexes[cc->regex_count++] = re;
    m->re = re;
    return true;
}

static char *lowered(Arena *a, const char *s, size_t n, bool keep_case) {
    char *out = arena_strndup(a, s, n);
    if (!keep_case) ascii_lower(out, out, n);
    return out;
}

static Matcher *compile(Compiled *cc, const Snapshot *s, const QueryNode *n, Err *err);
/* search_resolve_dir finds the directory record for a path, ignoring case. */
static bool search_resolve_dir(const Snapshot *s, const char *path, uint32_t *out);

static Matcher *compile_text(Compiled *cc, Matcher *m, const QueryNode *n, Err *err) {
    m->kind = M_TEXT;
    m->mode = n->mode;
    m->path = n->match_path;
    m->cased = n->case_sensitive;
    if (n->mode == TEXT_WILDCARD) {
        const char *inner;
        size_t len;
        GlobKind kind = simple_glob(n->text, &inner, &len);
        if (kind != GLOB_NONE) {
            m->glob = kind;
            m->needle = lowered(&cc->arena, inner, len, n->case_sensitive);
            m->needle_len = len;
            return m;
        }
    }
    if (n->mode == TEXT_REGEX || n->mode == TEXT_WILDCARD) {
        StrBuf pattern = {0};
        if (n->mode == TEXT_WILDCARD) {
            wildcard_regex(&pattern, n->text);
        } else {
            sb_puts(&pattern, n->text);
        }
        bool ok = compile_regex(cc, m, sb_cstr(&pattern), n->text, n->case_sensitive, err);
        sb_free(&pattern);
        return ok ? m : NULL;
    }
    m->needle_len = strlen(n->text);
    m->needle = lowered(&cc->arena, n->text, m->needle_len, n->case_sensitive);
    return m;
}

static Matcher *compile(Compiled *cc, const Snapshot *s, const QueryNode *n, Err *err) {
    Matcher *m = arena_calloc(&cc->arena, 1, sizeof *m);
    switch (n->kind) {
    case Q_AND:
    case Q_OR:
        if (n->kind == Q_AND && n->kid_count == 0) {
            m->kind = M_ALL;
            return m;
        }
        m->kind = n->kind == Q_AND ? M_AND : M_OR;
        m->kid_count = n->kid_count;
        m->kids = arena_alloc(&cc->arena, (n->kid_count ? n->kid_count : 1) * sizeof *m->kids);
        for (uint32_t k = 0; k < n->kid_count; k++)
            if (!(m->kids[k] = compile(cc, s, n->kids[k], err))) return NULL;
        return m;
    case Q_NOT:
        m->kind = M_NOT;
        return (m->kid = compile(cc, s, n->kid, err)) ? m : NULL;
    case Q_TEXT: return compile_text(cc, m, n, err);
    case Q_EXT:
        m->kind = M_EXT;
        m->exts = n->exts;
        m->ext_count = n->ext_count;
        return m;
    case Q_SIZE: m->kind = M_SIZE; break;
    case Q_MODIFIED: m->kind = M_MODIFIED; break;
    case Q_CREATED: m->kind = M_CREATED; break;
    case Q_NAMELEN: m->kind = M_NAMELEN; break;
    case Q_DEPTH: m->kind = M_DEPTH; break;
    case Q_ISDIR:
        m->kind = M_ISDIR;
        m->dir = n->dir;
        return m;
    case Q_PARENT:
    case Q_INFOLDER:
        m->kind = search_resolve_dir(s, n->path, &m->dir_id) ? (n->kind == Q_PARENT ? M_PARENT : M_INFOLDER) : M_NONE;
        return m;
    }
    m->range = n->range;
    return m;
}

static void compiled_free(Compiled *cc) {
    for (size_t i = 0; i < cc->regex_count; i++) {
        regfree(cc->regexes[i]);
        free(cc->regexes[i]);
    }
    free(cc->regexes);
    arena_free(&cc->arena);
}

/* ---- parallel evaluation ---- */

typedef struct {
    const Snapshot *s;
    const Matcher *root;
    const atomic_int *cancel;
    uint32_t first_id;
    U32Vec *parts;
} ScanJob;

static void scan_chunk(void *arg, size_t lo, size_t hi, size_t chunk) {
    ScanJob *job = arg;
    MatchCtx c = {.s = job->s};
    U32Vec *out = &job->parts[chunk];
    for (size_t i = lo; i < hi; i++) {
        if ((i - lo) % CANCEL_CHECK_INTERVAL == 0 && job->cancel && atomic_load(job->cancel)) break;
        uint32_t id = job->first_id + (uint32_t)i;
        if (!snap_live(job->s, id)) continue;
        c.id = id;
        c.have_path = c.have_lower = false;
        if (matches(job->root, &c)) u32vec_push(out, id);
    }
    sb_free(&c.path);
    sb_free(&c.path_lower);
}

static void scan(ThreadPool *pool, ScanJob job, size_t n, U32Vec *hits) {
    if (n == 0) return;
    ParallelPlan plan = parallel_plan(pool, n, MIN_CHUNK);
    job.parts = xcalloc(plan.chunks, sizeof *job.parts);
    threadpool_run_chunks(pool, plan, scan_chunk, &job);
    for (size_t c = 0; c < plan.chunks; c++) {
        for (size_t i = 0; i < job.parts[c].len; i++) u32vec_push(hits, job.parts[c].data[i]);
        u32vec_free(&job.parts[c]);
    }
    free(job.parts);
}

SearchStatus search_run(ThreadPool *pool, const Snapshot *s, const QueryNode *query, const atomic_int *cancel, U32Vec *hits,
                        Err *err) {
    Compiled cc = {0};
    arena_init(&cc.arena, 4096);
    cc.root = compile(&cc, s, query, err);
    if (!cc.root) {
        compiled_free(&cc);
        return SEARCH_ERROR;
    }
    hits->len = 0;
    ScanJob job = {.s = s, .root = cc.root, .cancel = cancel};
    scan(pool, job, s->total, hits);
    compiled_free(&cc);
    return cancel && atomic_load(cancel) ? SEARCH_CANCELLED : SEARCH_OK;
}

/* ---- ordering ---- */

typedef struct {
    const Snapshot *s;
    SortKey key;
    bool descending;
} SortCtx;

static int compare_i64(int64_t a, int64_t b) { return a < b ? -1 : a > b; }

static int compare_names(const Snapshot *s, uint32_t a, uint32_t b) {
    int c = strcmp(snap_lower(s, a), snap_lower(s, b));
    if (!c) c = strcmp(snap_name(s, a), snap_name(s, b));
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
    case SORT_SIZE: c = compare_i64(snap_record(s, a)->size, snap_record(s, b)->size); break;
    case SORT_MODIFIED: c = compare_i64(snap_record(s, a)->mtime, snap_record(s, b)->mtime); break;
    case SORT_CREATED: c = compare_i64(snap_record(s, a)->ctime, snap_record(s, b)->ctime); break;
    case SORT_EXT: {
        size_t la, lb;
        c = strcmp(snap_ext(s, a, &la), snap_ext(s, b, &lb));
        break;
    }
    default: break;
    }
    return c ? c : compare_names(s, a, b);
}

typedef struct {
    char *path;
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
    int c = strcmp(a->path, b->path);
    return c ? c : compare_names(sc->s, a->hit, b->hit);
}

/* top_by_path builds each lowercased path once, which beats walking parent chains on every comparison. */
static size_t top_by_path(const Snapshot *s, uint32_t *hits, size_t n, size_t k, bool descending) {
    Arena arena;
    arena_init(&arena, 1 << 20);
    KeyedPath *rows = xmalloc(n * sizeof *rows);
    StrBuf sb = {0};
    for (size_t i = 0; i < n; i++) {
        snap_path(s, hits[i], &sb);
        rows[i].path = lowered(&arena, sb.data, sb.len, false);
        rows[i].hit = hits[i];
    }
    sb_free(&sb);
    SortCtx sc = {.s = s, .key = SORT_PATH, .descending = descending};
    k = sort_top(rows, n, sizeof *rows, k, compare_keyed, &sc);
    for (size_t i = 0; i < k; i++) hits[i] = rows[i].hit;
    free(rows);
    arena_free(&arena);
    return k;
}

size_t search_top(const Snapshot *s, uint32_t *hits, size_t n, SortKey key, bool descending, long keep) {
    size_t k = keep < 0 || (size_t)keep > n ? n : (size_t)keep;
    if (key == SORT_RELEVANCE) key = SORT_NAME;
    if (key == SORT_PATH) return top_by_path(s, hits, n, k, descending);
    SortCtx sc = {.s = s, .key = key, .descending = descending};
    return sort_top(hits, n, sizeof *hits, k, compare_by_key, &sc);
}

/* ---- relevance ---- */

enum { SCORE_EXACT = 4, SCORE_PREFIX = 3, SCORE_WORD_START = 2, SCORE_CONTAINS = 1 };

typedef struct {
    uint32_t hit;
    int score, depth;
} Ranked;

static bool starts_word_in(const char *hay, const char *needle) {
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++)
        if (p == hay || !is_word_byte((unsigned char)p[-1])) return true;
    return false;
}

static int score(const char *lower_name, const char **terms, size_t term_count) {
    const char *dot = strrchr(lower_name, '.');
    size_t stem_len = dot && dot != lower_name ? (size_t)(dot - lower_name) : strlen(lower_name);
    int total = 0;
    for (size_t i = 0; i < term_count; i++) {
        const char *t = terms[i];
        size_t tlen = strlen(t);
        if (strcmp(lower_name, t) == 0 || (stem_len == tlen && strncmp(lower_name, t, tlen) == 0)) {
            total += SCORE_EXACT;
        } else if (strncmp(lower_name, t, tlen) == 0) {
            total += SCORE_PREFIX;
        } else if (starts_word_in(lower_name, t)) {
            total += SCORE_WORD_START;
        } else if (strstr(lower_name, t)) {
            total += SCORE_CONTAINS;
        }
    }
    return total;
}

static int depth_in_tree(const Snapshot *s, uint32_t id) {
    int d = 0;
    for (uint32_t p = snap_record(s, id)->parent; p != NO_PARENT; p = snap_record(s, p)->parent) d++;
    return d;
}

/* plain_terms collects the name terms a user typed; negated, path, wildcard and regex terms carry no ranking signal. */
static void plain_terms(Arena *a, const QueryNode *n, const char ***terms, size_t *count, size_t *cap) {
    if (n->kind == Q_AND || n->kind == Q_OR) {
        for (uint32_t k = 0; k < n->kid_count; k++) plain_terms(a, n->kids[k], terms, count, cap);
        return;
    }
    if (n->kind != Q_TEXT || n->match_path || n->mode == TEXT_WILDCARD || n->mode == TEXT_REGEX) return;
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *terms = xrealloc(*terms, *cap * sizeof **terms);
    }
    (*terms)[(*count)++] = lowered(a, n->text, strlen(n->text), false);
}

static int compare_ranked(const void *ctx, const void *pa, const void *pb) {
    const Snapshot *s = ctx;
    const Ranked *a = pa, *b = pb;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    int c = strcmp(snap_lower(s, a->hit), snap_lower(s, b->hit));
    return c ? c : compare_i64(a->hit, b->hit);
}

size_t search_rank(const Snapshot *s, uint32_t *hits, size_t n, const QueryNode *query, long keep) {
    size_t k = keep < 0 || (size_t)keep > n ? n : (size_t)keep;
    Arena arena;
    arena_init(&arena, 4096);
    const char **terms = NULL;
    size_t term_count = 0, term_cap = 0;
    plain_terms(&arena, query, &terms, &term_count, &term_cap);
    Ranked *rows = xmalloc((n ? n : 1) * sizeof *rows);
    for (size_t i = 0; i < n; i++) {
        rows[i] = (Ranked){.hit = hits[i]};
        if (term_count) {
            rows[i].score = score(snap_lower(s, hits[i]), terms, term_count);
            rows[i].depth = depth_in_tree(s, hits[i]);
        }
    }
    k = sort_top(rows, n, sizeof *rows, k, compare_ranked, s);
    for (size_t i = 0; i < k; i++) hits[i] = rows[i].hit;
    free(rows);
    free(terms);
    arena_free(&arena);
    return k;
}

/* ---- folders ---- */

static bool find_child_dir(const Snapshot *s, uint32_t parent, const char *lower_name, size_t len, uint32_t *out) {
    for (uint32_t id = parent + 1; id < s->total; id++) {
        const FileRecord *r = snap_record(s, id);
        if (r->parent == parent && record_is_dir(r) && snap_live(s, id) && r->name_len == len &&
            memcmp(snap_lower(s, id), lower_name, len) == 0) {
            *out = id;
            return true;
        }
    }
    return false;
}

static bool search_resolve_dir(const Snapshot *s, const char *path, uint32_t *out) {
    char *target = path_abs(path);
    ascii_lower(target, target, strlen(target));
    bool found = false;
    for (uint32_t id = 0; id < s->total && !found; id++) {
        const FileRecord *r = snap_record(s, id);
        if (r->parent != NO_PARENT || !record_is_dir(r) || !snap_live(s, id)) continue;
        char *root = xstrdup(snap_lower(s, id));
        size_t root_len = strlen(root);
        while (root_len > 1 && root[root_len - 1] == '/') root[--root_len] = '\0';
        if (strcmp(target, root) == 0) {
            *out = id;
            found = true;
        } else {
            size_t prefix = strcmp(root, "/") == 0 ? 1 : root_len + 1;
            if (strncmp(target, root, root_len) == 0 && (root_len == 1 || target[root_len] == '/')) {
                uint32_t cur = id;
                bool ok = true;
                for (const char *p = target + prefix; ok && *p;) {
                    size_t len = strcspn(p, "/");
                    ok = find_child_dir(s, cur, p, len, &cur);
                    p += len;
                    if (*p == '/') p++;
                }
                if (ok) {
                    *out = cur;
                    found = true;
                }
            }
        }
        free(root);
    }
    free(target);
    return found;
}

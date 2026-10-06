#include "query.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---- lexer ---- */

typedef enum { TOK_WORD, TOK_OR, TOK_NOT, TOK_OPEN, TOK_CLOSE } TokenKind;

typedef struct {
    TokenKind kind;
    const char *text;
} Token;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/*
 * lex_word reads one word. Inside a word, "<" and ">" are literal once a ":"
 * has been seen, so size:>1mb and dm:<2020 work while <a|b> still groups.
 * Quoted stretches are copied verbatim.
 */
static const char *lex_word(Arena *arena, const char *s, size_t *i) {
    StrBuf b = {0};
    bool saw_colon = false;
    size_t n = strlen(s);
    while (*i < n) {
        char c = s[*i];
        if (c == '"') {
            const char *end = strchr(s + *i + 1, '"');
            if (!end) {
                sb_puts(&b, s + *i + 1);
                *i = n;
                break;
            }
            sb_append(&b, s + *i + 1, (size_t)(end - (s + *i + 1)));
            *i = (size_t)(end - s) + 1;
            continue;
        }
        if (is_space(c) || c == '|') break;
        if ((c == '<' || c == '>') && !saw_colon) break;
        if (c == ':') saw_colon = true;
        sb_putc(&b, c);
        (*i)++;
    }
    const char *word = arena_strndup(arena, b.data ? b.data : "", b.len);
    sb_free(&b);
    return word;
}

static Token *lex(Arena *arena, const char *s, size_t *count) {
    size_t n = strlen(s), cap = 8;
    Token *toks = xmalloc(cap * sizeof *toks);
    *count = 0;
    size_t i = 0;
    while (i < n) {
        if (*count == cap) {
            cap *= 2;
            toks = xrealloc(toks, cap * sizeof *toks);
        }
        char c = s[i];
        Token t = {.kind = TOK_WORD};
        if (is_space(c)) {
            i++;
            continue;
        }
        switch (c) {
        case '|': t.kind = TOK_OR; i++; break;
        case '!': t.kind = TOK_NOT; i++; break;
        case '<': t.kind = TOK_OPEN; i++; break;
        case '>': t.kind = TOK_CLOSE; i++; break;
        default: t.text = lex_word(arena, s, &i);
        }
        toks[(*count)++] = t;
    }
    Token *out = arena_alloc(arena, (*count ? *count : 1) * sizeof *out);
    memcpy(out, toks, *count * sizeof *out);
    free(toks);
    return out;
}

/* ---- values ---- */

static const char *trim(char *buf, size_t cap, const char *s, bool lower) {
    while (is_space(*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && is_space(s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    if (lower) ascii_lower(buf, buf, n);
    return buf;
}

static int64_t saturating_inc(int64_t v) { return v == INT64_MAX ? v : v + 1; }
static int64_t saturating_dec(int64_t v) { return v == INT64_MIN ? v : v - 1; }

bool parse_range(const char *s, ValueParser parse, time_t now, Range *out, Err *err) {
    char buf[256];
    s = trim(buf, sizeof buf, s, false);
    Range v;
    if (has_prefix(s, ">=")) {
        if (!parse(s + 2, now, &v, err)) return false;
        *out = (Range){v.lo, INT64_MAX};
        return true;
    }
    if (has_prefix(s, "<=")) {
        if (!parse(s + 2, now, &v, err)) return false;
        *out = (Range){INT64_MIN, v.hi};
        return true;
    }
    if (s[0] == '>') {
        if (!parse(s + 1, now, &v, err)) return false;
        *out = (Range){saturating_inc(v.hi), INT64_MAX};
        return true;
    }
    if (s[0] == '<') {
        if (!parse(s + 1, now, &v, err)) return false;
        *out = (Range){INT64_MIN, saturating_dec(v.lo)};
        return true;
    }
    if (s[0] == '=') return parse(s + 1, now, out, err);
    const char *dots = strstr(s, "..");
    if (!dots) return parse(s, now, out, err);
    Range r = {INT64_MIN, INT64_MAX};
    if (dots > s) {
        char lo[256];
        memcpy(lo, s, (size_t)(dots - s));
        lo[dots - s] = '\0';
        if (!parse(lo, now, &v, err)) return false;
        r.lo = v.lo;
    }
    if (dots[2]) {
        if (!parse(dots + 2, now, &v, err)) return false;
        r.hi = v.hi;
    }
    *out = r;
    return true;
}

bool parse_int_value(const char *s, time_t now, Range *out, Err *err) {
    (void)now;
    int64_t n;
    if (!parse_int64(s, &n)) {
        err_set(err, "expected a number, got \"%s\"", s);
        return false;
    }
    *out = (Range){n, n};
    return true;
}

#define KB ((int64_t)1 << 10)
#define MB (KB << 10)
#define GB (MB << 10)
#define TB (GB << 10)
#define PB (TB << 10)

bool parse_size_value(const char *s, time_t now, Range *out, Err *err) {
    (void)now;
    static const struct {
        const char *name;
        Range range;
    } named[] = {
        {"empty", {0, 0}},
        {"tiny", {0, 10 * KB}},
        {"small", {10 * KB + 1, 100 * KB}},
        {"medium", {100 * KB + 1, MB}},
        {"large", {MB + 1, 16 * MB}},
        {"huge", {16 * MB + 1, 128 * MB}},
        {"gigantic", {128 * MB + 1, INT64_MAX}},
    };
    static const struct {
        const char *unit;
        int64_t factor;
    } units[] = {
        {"", 1},       {"b", 1},       {"k", KB},     {"kb", KB},     {"kib", KB},   {"m", MB},
        {"mb", MB},    {"mib", MB},    {"g", GB},     {"gb", GB},     {"gib", GB},   {"t", TB},
        {"tb", TB},    {"tib", TB},    {"p", PB},     {"pb", PB},     {"pib", PB},
    };
    char buf[128];
    s = trim(buf, sizeof buf, s, true);
    for (size_t i = 0; i < ARRAY_LEN(named); i++) {
        if (strcmp(s, named[i].name) == 0) {
            *out = named[i].range;
            return true;
        }
    }
    const char *p = s;
    while (isdigit((unsigned char)*p)) p++;
    bool valid = p > s;
    if (valid && *p == '.') {
        const char *frac = ++p;
        while (isdigit((unsigned char)*p)) p++;
        valid = p > frac;
    }
    const char *number_end = p;
    while (is_space(*p)) p++;
    const char *unit = p;
    while (*p >= 'a' && *p <= 'z') p++;
    if (!valid || *p) {
        err_set(err, "expected a size such as 10mb, got \"%s\"", s);
        return false;
    }
    (void)number_end;
    for (size_t i = 0; i < ARRAY_LEN(units); i++) {
        if (strcmp(unit, units[i].unit) == 0) {
            int64_t n = (int64_t)(strtod(s, NULL) * (double)units[i].factor);
            *out = (Range){n, n};
            return true;
        }
    }
    err_set(err, "unknown size unit \"%s\"", unit);
    return false;
}

/* local_time builds a local time from possibly out-of-range fields, normalising like Go's time.Date. */
static int64_t local_time(int year, int month, int day, int hour, int min, int sec) {
    struct tm tm = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                    .tm_hour = hour, .tm_min = min, .tm_sec = sec, .tm_isdst = -1};
    return (int64_t)mktime(&tm);
}

static Range span(int64_t from, int64_t to) { return (Range){from, to - 1}; }

static bool parse_relative(const char *s, time_t now, Range *out) {
    const char *p;
    if (has_prefix(s, "last")) {
        p = s + 4;
    } else if (has_prefix(s, "past")) {
        p = s + 4;
    } else {
        return false;
    }
    if (!isdigit((unsigned char)*p)) return false;
    int n = 0;
    while (isdigit((unsigned char)*p)) n = n * 10 + (*p++ - '0');
    static const char *units[] = {"day", "week", "month", "year"};
    size_t unit = ARRAY_LEN(units);
    for (size_t i = 0; i < ARRAY_LEN(units); i++) {
        size_t len = strlen(units[i]);
        if (strncmp(p, units[i], len) == 0 && (strcmp(p + len, "") == 0 || strcmp(p + len, "s") == 0)) unit = i;
    }
    if (unit == ARRAY_LEN(units)) return false;
    struct tm tm;
    localtime_r(&now, &tm);
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
    int64_t from;
    switch (unit) {
    case 0: from = local_time(y, m, d - n, tm.tm_hour, tm.tm_min, tm.tm_sec); break;
    case 1: from = local_time(y, m, d - 7 * n, tm.tm_hour, tm.tm_min, tm.tm_sec); break;
    case 2: from = local_time(y, m - n, d, tm.tm_hour, tm.tm_min, tm.tm_sec); break;
    default: from = local_time(y - n, m, d, tm.tm_hour, tm.tm_min, tm.tm_sec); break;
    }
    *out = (Range){from, (int64_t)now};
    return true;
}

static bool parse_absolute_date(const char *s, Range *out, Err *err) {
    int nums[3];
    int count = 0;
    const char *p = s;
    while (*p) {
        while (*p == '-' || *p == '/' || *p == '.') p++;
        if (!*p) break;
        if (count == 3) {
            err_set(err, "expected a date such as 2024, 2024-03 or 2024-03-15");
            return false;
        }
        const char *start = p;
        while (*p && *p != '-' && *p != '/' && *p != '.') p++;
        char part[32];
        size_t len = MIN((size_t)(p - start), sizeof part - 1);
        memcpy(part, start, len);
        part[len] = '\0';
        int64_t v;
        if (!parse_int64(part, &v) || strchr(part, ' ')) {
            err_set(err, "unknown date \"%s\"", s);
            return false;
        }
        nums[count++] = (int)v;
    }
    if (count == 0) {
        err_set(err, "expected a date such as 2024, 2024-03 or 2024-03-15");
        return false;
    }
    if (nums[0] < 1000) {
        err_set(err, "year must come first in \"%s\"", s);
        return false;
    }
    switch (count) {
    case 1: *out = span(local_time(nums[0], 1, 1, 0, 0, 0), local_time(nums[0] + 1, 1, 1, 0, 0, 0)); break;
    case 2: *out = span(local_time(nums[0], nums[1], 1, 0, 0, 0), local_time(nums[0], nums[1] + 1, 1, 0, 0, 0)); break;
    default:
        *out = span(local_time(nums[0], nums[1], nums[2], 0, 0, 0), local_time(nums[0], nums[1], nums[2] + 1, 0, 0, 0));
    }
    return true;
}

/*
 * parse_date_value covers today, yesterday, thisweek, lastweek, thismonth,
 * lastmonth, thisyear, lastyear, last<N>days/weeks/months/years, and
 * absolute dates YYYY, YYYY-MM and YYYY-MM-DD. Weeks start on Monday.
 */
bool parse_date_value(const char *s, time_t now, Range *out, Err *err) {
    char buf[128];
    s = trim(buf, sizeof buf, s, true);
    struct tm tm;
    localtime_r(&now, &tm);
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
    int monday = d - (tm.tm_wday + 6) % 7;

    if (strcmp(s, "today") == 0) {
        *out = span(local_time(y, m, d, 0, 0, 0), local_time(y, m, d + 1, 0, 0, 0));
    } else if (strcmp(s, "yesterday") == 0) {
        *out = span(local_time(y, m, d - 1, 0, 0, 0), local_time(y, m, d, 0, 0, 0));
    } else if (strcmp(s, "thisweek") == 0) {
        *out = span(local_time(y, m, monday, 0, 0, 0), local_time(y, m, monday + 7, 0, 0, 0));
    } else if (strcmp(s, "lastweek") == 0) {
        *out = span(local_time(y, m, monday - 7, 0, 0, 0), local_time(y, m, monday, 0, 0, 0));
    } else if (strcmp(s, "thismonth") == 0) {
        *out = span(local_time(y, m, 1, 0, 0, 0), local_time(y, m + 1, 1, 0, 0, 0));
    } else if (strcmp(s, "lastmonth") == 0) {
        *out = span(local_time(y, m - 1, 1, 0, 0, 0), local_time(y, m, 1, 0, 0, 0));
    } else if (strcmp(s, "thisyear") == 0) {
        *out = span(local_time(y, 1, 1, 0, 0, 0), local_time(y + 1, 1, 1, 0, 0, 0));
    } else if (strcmp(s, "lastyear") == 0) {
        *out = span(local_time(y - 1, 1, 1, 0, 0, 0), local_time(y, 1, 1, 0, 0, 0));
    } else if (!parse_relative(s, now, out)) {
        return parse_absolute_date(s, out, err);
    }
    return true;
}

/* ---- parser ---- */

typedef struct {
    Arena *arena;
    Token *toks;
    size_t count, pos;
    QueryDefaults d;
    Err *err;
    bool failed;
} Parser;

static QueryNode *node_new(Parser *p, QueryKind kind) {
    QueryNode *n = arena_calloc(p->arena, 1, sizeof *n);
    n->kind = kind;
    return n;
}

static QueryNode *match_all(Parser *p) { return node_new(p, Q_AND); }

typedef struct {
    QueryNode **items;
    size_t len, cap;
} NodeList;

static void nodes_push(NodeList *l, QueryNode *n) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->len++] = n;
}

/* nodes_finish turns a list into its single element, or a node of kind over all of them. */
static QueryNode *nodes_finish(Parser *p, NodeList *l, QueryKind kind) {
    QueryNode *n;
    if (l->len == 1) {
        n = l->items[0];
    } else {
        n = node_new(p, kind);
        n->kid_count = (uint32_t)l->len;
        n->kids = arena_alloc(p->arena, (l->len ? l->len : 1) * sizeof *n->kids);
        memcpy(n->kids, l->items, l->len * sizeof *n->kids);
    }
    free(l->items);
    return n;
}

static QueryNode *pair(Parser *p, QueryNode *a, QueryNode *b) {
    NodeList l = {0};
    nodes_push(&l, a);
    nodes_push(&l, b);
    return nodes_finish(p, &l, Q_AND);
}

static const Token *peek(Parser *p) { return p->pos < p->count ? &p->toks[p->pos] : NULL; }

static QueryNode *parse_or(Parser *p);

static QueryNode *fail(Parser *p, const char *key, const Err *why) {
    if (!p->failed) err_set(p->err, "%s: %s", key, why->msg);
    p->failed = true;
    return match_all(p);
}

static QueryNode *text_node(Parser *p, QueryNode t, const char *s, bool mode_set, bool no_wildcards) {
    QueryNode *n = node_new(p, Q_TEXT);
    *n = t;
    n->kind = Q_TEXT;
    n->text = arena_strdup(p->arena, s);
    if (!mode_set) {
        if (p->d.regex) {
            n->mode = TEXT_REGEX;
        } else if (!no_wildcards && strpbrk(s, "*?")) {
            n->mode = TEXT_WILDCARD;
        } else if (p->d.whole_word) {
            n->mode = TEXT_WHOLEWORD;
        }
    }
    return n;
}

static QueryNode *ext_node(Parser *p, const char *arg) {
    QueryNode *n = node_new(p, Q_EXT);
    size_t cap = strlen(arg) + 1;
    n->exts = arena_alloc(p->arena, cap * sizeof *n->exts);
    const char *s = arg;
    while (*s) {
        size_t len = strcspn(s, ";,");
        const char *e = s;
        size_t elen = len;
        while (elen && is_space(*e)) e++, elen--;
        while (elen && is_space(e[elen - 1])) elen--;
        if (elen && *e == '.') e++, elen--;
        if (elen) {
            char *ext = arena_strndup(p->arena, e, elen);
            ascii_lower(ext, ext, elen);
            n->exts[n->ext_count++] = ext;
        }
        s += len;
        if (*s) s++;
    }
    return n;
}

static QueryNode *range_node(Parser *p, QueryKind kind, const char *key, const char *arg, ValueParser parse) {
    Err why;
    Range r;
    if (!parse_range(arg, parse, p->d.now, &r, &why)) return fail(p, key, &why);
    QueryNode *n = node_new(p, kind);
    n->range = r;
    return n;
}

/* parse_word peels modifier and function prefixes off a word such as "folder:case:regex:^foo". */
static QueryNode *parse_word(Parser *p, const char *w) {
    QueryNode t = {.kind = Q_TEXT, .match_path = p->d.match_path, .case_sensitive = p->d.case_sensitive};
    QueryNode *type_filter = NULL;
    bool mode_set = false, no_wildcards = false;
    const char *rest = w;
#define WRAP(n) (type_filter ? pair(p, type_filter, (n)) : (n))
    for (;;) {
        const char *colon = strchr(rest, ':');
        if (!colon) break;
        char key[32];
        size_t klen = (size_t)(colon - rest);
        if (klen >= sizeof key) break;
        ascii_lower(key, rest, klen);
        key[klen] = '\0';
        const char *arg = colon + 1;
        if (strcmp(key, "case") == 0) {
            t.case_sensitive = true;
        } else if (strcmp(key, "nocase") == 0) {
            t.case_sensitive = false;
        } else if (strcmp(key, "regex") == 0) {
            t.mode = TEXT_REGEX, mode_set = true;
        } else if (!strcmp(key, "wfn") || !strcmp(key, "wholefilename") || !strcmp(key, "exact")) {
            t.mode = TEXT_WHOLENAME, mode_set = true;
        } else if (!strcmp(key, "ww") || !strcmp(key, "wholeword")) {
            t.mode = TEXT_WHOLEWORD, mode_set = true;
        } else if (strcmp(key, "wildcards") == 0) {
            t.mode = TEXT_WILDCARD, mode_set = true;
        } else if (strcmp(key, "nowildcards") == 0) {
            no_wildcards = true;
        } else if (strcmp(key, "path") == 0) {
            t.match_path = true;
        } else if (strcmp(key, "nopath") == 0) {
            t.match_path = false;
        } else if (!strcmp(key, "file") || !strcmp(key, "files")) {
            type_filter = node_new(p, Q_ISDIR);
        } else if (!strcmp(key, "folder") || !strcmp(key, "folders") || !strcmp(key, "dir") || !strcmp(key, "dirs")) {
            type_filter = node_new(p, Q_ISDIR);
            type_filter->dir = true;
        } else if (strcmp(key, "ext") == 0) {
            return WRAP(ext_node(p, arg));
        } else if (strcmp(key, "size") == 0) {
            return WRAP(range_node(p, Q_SIZE, key, arg, parse_size_value));
        } else if (!strcmp(key, "dm") || !strcmp(key, "datemodified")) {
            return WRAP(range_node(p, Q_MODIFIED, key, arg, parse_date_value));
        } else if (!strcmp(key, "dc") || !strcmp(key, "datecreated")) {
            return WRAP(range_node(p, Q_CREATED, key, arg, parse_date_value));
        } else if (strcmp(key, "len") == 0) {
            return WRAP(range_node(p, Q_NAMELEN, key, arg, parse_int_value));
        } else if (!strcmp(key, "depth") || !strcmp(key, "parents")) {
            return WRAP(range_node(p, Q_DEPTH, key, arg, parse_int_value));
        } else if (!strcmp(key, "parent") || !strcmp(key, "infolder")) {
            QueryNode *n = node_new(p, key[0] == 'p' ? Q_PARENT : Q_INFOLDER);
            n->path = arena_strdup(p->arena, arg);
            return WRAP(n);
        } else {
            break;
        }
        rest = arg;
    }
    if (!*rest) return type_filter ? type_filter : match_all(p);
    return WRAP(text_node(p, t, rest, mode_set, no_wildcards));
#undef WRAP
}

static QueryNode *parse_unary(Parser *p) {
    const Token *t = &p->toks[p->pos++];
    switch (t->kind) {
    case TOK_NOT: {
        QueryNode *n = node_new(p, Q_NOT);
        n->kid = peek(p) ? parse_unary(p) : match_all(p);
        return n;
    }
    case TOK_OPEN: {
        QueryNode *n = parse_or(p);
        const Token *close = peek(p);
        if (close && close->kind == TOK_CLOSE) p->pos++;
        return n;
    }
    case TOK_WORD:
        return parse_word(p, t->text);
    default:
        return match_all(p);
    }
}

static QueryNode *parse_and(Parser *p) {
    NodeList kids = {0};
    for (const Token *t; (t = peek(p)) && t->kind != TOK_OR && t->kind != TOK_CLOSE;) nodes_push(&kids, parse_unary(p));
    return nodes_finish(p, &kids, Q_AND);
}

static QueryNode *parse_or(Parser *p) {
    NodeList kids = {0};
    nodes_push(&kids, parse_and(p));
    for (const Token *t; (t = peek(p)) && t->kind == TOK_OR;) {
        p->pos++;
        nodes_push(&kids, parse_and(p));
    }
    return nodes_finish(p, &kids, Q_OR);
}

QueryNode *query_parse(Arena *arena, const char *s, QueryDefaults defaults, Err *err) {
    if (!defaults.now) defaults.now = time(NULL);
    Parser p = {.arena = arena, .d = defaults, .err = err};
    p.toks = lex(arena, s, &p.count);
    QueryNode *n = parse_or(&p);
    /* A stray ">" ends a group that was never opened; skip it and keep going. */
    while (!p.failed && p.pos < p.count) {
        p.pos++;
        if (p.pos < p.count) n = pair(&p, n, parse_or(&p));
    }
    return p.failed ? NULL : n;
}

QueryNode *query_restrict(Arena *arena, QueryNode *node, const char *path, bool files_only, bool dirs_only) {
    Parser p = {.arena = arena};
    NodeList kids = {0};
    nodes_push(&kids, node);
    if (path && *path) {
        QueryNode *n = node_new(&p, Q_INFOLDER);
        n->path = arena_strdup(arena, path);
        nodes_push(&kids, n);
    }
    if (files_only) nodes_push(&kids, node_new(&p, Q_ISDIR));
    if (dirs_only) {
        QueryNode *n = node_new(&p, Q_ISDIR);
        n->dir = true;
        nodes_push(&kids, n);
    }
    return nodes_finish(&p, &kids, Q_AND);
}

bool query_is_match_all(const QueryNode *node) { return node->kind == Q_AND && node->kid_count == 0; }

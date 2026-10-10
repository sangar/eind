#include "query.h"

#include <string.h>

#include "mc/platform/platform.h"

/* ---- lexer ---- */

typedef enum { TOK_WORD, TOK_OR, TOK_NOT, TOK_OPEN, TOK_CLOSE } TokenKind;

typedef struct {
    TokenKind kind;
    String text;
} Token;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/*
 * lex_word reads one word. Inside a word, "<" and ">" are literal once a ":"
 * has been seen, so size:>1mb and dm:<2020 work while <a|b> still groups.
 * Quoted stretches are copied verbatim.
 */
static String lex_word(Arena *arena, String s, size_t *i) {
    StringBuilder word = str_builder_create(arena, 16);
    bool saw_colon = false;
    while (*i < s.len) {
        char c = s.data[*i];
        if (c == '"') {
            String rest = str_slice(s, *i + 1, s.len);
            size_t end;
            if (!str_find_char(rest, '"', &end)) {
                str_builder_append(&word, rest);
                *i = s.len;
                break;
            }
            str_builder_append(&word, str_slice(rest, 0, end));
            *i += end + 2;
            continue;
        }
        if (is_space(c) || c == '|') break;
        if ((c == '<' || c == '>') && !saw_colon) break;
        if (c == ':') saw_colon = true;
        str_builder_append_char(&word, c);
        (*i)++;
    }
    return str_builder_finish(&word);
}

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenList;

static TokenList lex(Arena *arena, String s) {
    TokenList tokens = {0};
    size_t i = 0;
    while (i < s.len) {
        char c = s.data[i];
        if (is_space(c)) {
            i++;
            continue;
        }
        Token t = {.kind = TOK_WORD};
        switch (c) {
        case '|': t.kind = TOK_OR; i++; break;
        case '!': t.kind = TOK_NOT; i++; break;
        case '<': t.kind = TOK_OPEN; i++; break;
        case '>': t.kind = TOK_CLOSE; i++; break;
        default: t.text = lex_word(arena, s, &i);
        }
        tokens.items = arena_grow(arena, tokens.items, &tokens.capacity, tokens.count, sizeof *tokens.items);
        tokens.items[tokens.count++] = t;
    }
    return tokens;
}

/* ---- values ---- */

/* ValueText holds a trimmed copy of a value, ASCII-lowercased when asked. */
typedef struct {
    char bytes[256];
} ValueText;

static String trimmed(ValueText *buf, String s, bool lower) {
    s = str_trim(s);
    size_t n = min_size(s.len, sizeof buf->bytes - 1);
    for (size_t i = 0; i < n; i++) {
        char c = s.data[i];
        buf->bytes[i] = lower && c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
    }
    buf->bytes[n] = '\0';
    return (String){buf->bytes, n};
}

static int64_t saturating_inc(int64_t v) { return v == INT64_MAX ? v : v + 1; }
static int64_t saturating_dec(int64_t v) { return v == INT64_MIN ? v : v - 1; }

Error parse_range(String s, ValueParser parse, time_t now, Range *out, Err *err) {
    ValueText buf;
    s = trimmed(&buf, s, false);
    Range v;
    Error e;
    if (str_starts_with(s, S(">="))) {
        if ((e = parse(str_slice(s, 2, s.len), now, &v, err)) != ERR_OK) return e;
        *out = (Range){v.lo, INT64_MAX};
        return ERR_OK;
    }
    if (str_starts_with(s, S("<="))) {
        if ((e = parse(str_slice(s, 2, s.len), now, &v, err)) != ERR_OK) return e;
        *out = (Range){INT64_MIN, v.hi};
        return ERR_OK;
    }
    if (str_starts_with(s, S(">"))) {
        if ((e = parse(str_slice(s, 1, s.len), now, &v, err)) != ERR_OK) return e;
        *out = (Range){saturating_inc(v.hi), INT64_MAX};
        return ERR_OK;
    }
    if (str_starts_with(s, S("<"))) {
        if ((e = parse(str_slice(s, 1, s.len), now, &v, err)) != ERR_OK) return e;
        *out = (Range){INT64_MIN, saturating_dec(v.lo)};
        return ERR_OK;
    }
    if (str_starts_with(s, S("="))) return parse(str_slice(s, 1, s.len), now, out, err);
    size_t dots;
    if (!str_find(s, S(".."), &dots)) return parse(s, now, out, err);
    Range r = {INT64_MIN, INT64_MAX};
    if (dots > 0) {
        if ((e = parse(str_slice(s, 0, dots), now, &v, err)) != ERR_OK) return e;
        r.lo = v.lo;
    }
    if (dots + 2 < s.len) {
        if ((e = parse(str_slice(s, dots + 2, s.len), now, &v, err)) != ERR_OK) return e;
        r.hi = v.hi;
    }
    *out = r;
    return ERR_OK;
}

[[nodiscard]] static Error parse_int_value(String s, time_t now, Range *out, Err *err) {
    unused(now);
    int64_t n;
    if (!str_parse_i64(str_trim(s), &n))
        return err_set(err, ERR_PARSE, "expected a number, got \"%.*s\"", (int)s.len, s.data);
    *out = (Range){n, n};
    return ERR_OK;
}

#define KB ((int64_t)1 << 10)
#define MB (KB << 10)
#define GB (MB << 10)
#define TB (GB << 10)
#define PB (TB << 10)

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

Error parse_size_value(String s, time_t now, Range *out, Err *err) {
    unused(now);
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
    ValueText buf;
    s = trimmed(&buf, s, true);
    for (size_t i = 0; i < countof(named); i++) {
        if (str_equal(s, S(named[i].name))) {
            *out = named[i].range;
            return ERR_OK;
        }
    }
    size_t p = 0;
    while (p < s.len && is_digit(s.data[p])) p++;
    bool valid = p > 0;
    if (valid && p < s.len && s.data[p] == '.') {
        size_t fraction = ++p;
        while (p < s.len && is_digit(s.data[p])) p++;
        valid = p > fraction;
    }
    String number = str_slice(s, 0, p);
    while (p < s.len && is_space(s.data[p])) p++;
    size_t unit_start = p;
    while (p < s.len && s.data[p] >= 'a' && s.data[p] <= 'z') p++;
    if (!valid || p < s.len)
        return err_set(err, ERR_PARSE, "expected a size such as 10mb, got \"%.*s\"", (int)s.len, s.data);
    String unit = str_slice(s, unit_start, s.len);
    double value;
    for (size_t i = 0; i < countof(units); i++) {
        if (str_equal(unit, S(units[i].unit)) && float_parse(number, &value)) {
            int64_t n = (int64_t)(value * (double)units[i].factor);
            *out = (Range){n, n};
            return ERR_OK;
        }
    }
    return err_set(err, ERR_PARSE, "unknown size unit \"%.*s\"", (int)unit.len, unit.data);
}

/* local_time builds a local time from possibly out-of-range fields, normalising like Go's time.Date. */
static int64_t local_time(int year, int month, int day, int hour, int min, int sec) {
    struct tm tm = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                    .tm_hour = hour, .tm_min = min, .tm_sec = sec, .tm_isdst = -1};
    return (int64_t)mktime(&tm);
}

static Range span(int64_t from, int64_t to) { return (Range){from, to - 1}; }

static bool parse_relative(String s, time_t now, Range *out) {
    if (!str_starts_with(s, S("last")) && !str_starts_with(s, S("past"))) return false;
    size_t p = 4;
    if (p >= s.len || !is_digit(s.data[p])) return false;
    int n = 0;
    while (p < s.len && is_digit(s.data[p])) n = n * 10 + (s.data[p++] - '0');
    String rest = str_slice(s, p, s.len);
    static const char *const units[] = {"day", "week", "month", "year"};
    size_t unit = countof(units);
    for (size_t i = 0; i < countof(units); i++)
        if (str_equal(rest, S(units[i])) || str_equal(str_trim_suffix(rest, S("s")), S(units[i]))) unit = i;
    if (unit == countof(units)) return false;
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

static bool is_date_separator(char c) { return c == '-' || c == '/' || c == '.'; }

[[nodiscard]] static Error parse_absolute_date(String s, Range *out, Err *err) {
    int nums[3];
    int count = 0;
    size_t p = 0;
    while (p < s.len) {
        while (p < s.len && is_date_separator(s.data[p])) p++;
        if (p == s.len) break;
        if (count == 3) return err_set(err, ERR_PARSE, "expected a date such as 2024, 2024-03 or 2024-03-15");
        size_t start = p;
        while (p < s.len && !is_date_separator(s.data[p])) p++;
        int64_t v;
        if (!str_parse_i64(str_slice(s, start, p), &v) || v < INT32_MIN || v > INT32_MAX)
            return err_set(err, ERR_PARSE, "unknown date \"%.*s\"", (int)s.len, s.data);
        nums[count++] = (int)v;
    }
    if (count == 0) return err_set(err, ERR_PARSE, "expected a date such as 2024, 2024-03 or 2024-03-15");
    if (nums[0] < 1000) return err_set(err, ERR_PARSE, "year must come first in \"%.*s\"", (int)s.len, s.data);
    switch (count) {
    case 1: *out = span(local_time(nums[0], 1, 1, 0, 0, 0), local_time(nums[0] + 1, 1, 1, 0, 0, 0)); break;
    case 2: *out = span(local_time(nums[0], nums[1], 1, 0, 0, 0), local_time(nums[0], nums[1] + 1, 1, 0, 0, 0)); break;
    default:
        *out = span(local_time(nums[0], nums[1], nums[2], 0, 0, 0), local_time(nums[0], nums[1], nums[2] + 1, 0, 0, 0));
    }
    return ERR_OK;
}

/*
 * parse_date_value covers today, yesterday, thisweek, lastweek, thismonth,
 * lastmonth, thisyear, lastyear, last<N>days/weeks/months/years, and
 * absolute dates YYYY, YYYY-MM and YYYY-MM-DD. Weeks start on Monday.
 */
Error parse_date_value(String s, time_t now, Range *out, Err *err) {
    ValueText buf;
    s = trimmed(&buf, s, true);
    struct tm tm;
    localtime_r(&now, &tm);
    int y = tm.tm_year + 1900, m = tm.tm_mon + 1, d = tm.tm_mday;
    int monday = d - (tm.tm_wday + 6) % 7;

    if (str_equal(s, S("today"))) {
        *out = span(local_time(y, m, d, 0, 0, 0), local_time(y, m, d + 1, 0, 0, 0));
    } else if (str_equal(s, S("yesterday"))) {
        *out = span(local_time(y, m, d - 1, 0, 0, 0), local_time(y, m, d, 0, 0, 0));
    } else if (str_equal(s, S("thisweek"))) {
        *out = span(local_time(y, m, monday, 0, 0, 0), local_time(y, m, monday + 7, 0, 0, 0));
    } else if (str_equal(s, S("lastweek"))) {
        *out = span(local_time(y, m, monday - 7, 0, 0, 0), local_time(y, m, monday, 0, 0, 0));
    } else if (str_equal(s, S("thismonth"))) {
        *out = span(local_time(y, m, 1, 0, 0, 0), local_time(y, m + 1, 1, 0, 0, 0));
    } else if (str_equal(s, S("lastmonth"))) {
        *out = span(local_time(y, m - 1, 1, 0, 0, 0), local_time(y, m, 1, 0, 0, 0));
    } else if (str_equal(s, S("thisyear"))) {
        *out = span(local_time(y, 1, 1, 0, 0, 0), local_time(y + 1, 1, 1, 0, 0, 0));
    } else if (str_equal(s, S("lastyear"))) {
        *out = span(local_time(y - 1, 1, 1, 0, 0, 0), local_time(y, 1, 1, 0, 0, 0));
    } else if (!parse_relative(s, now, out)) {
        return parse_absolute_date(s, out, err);
    }
    return ERR_OK;
}

/* ---- parser ---- */

typedef struct {
    Arena *arena;
    TokenList tokens;
    size_t pos;
    QueryDefaults d;
    Err *err;
    Error error; /* the first failure, which ends the parse */
} Parser;

static QueryNode *node_new(Arena *arena, QueryKind kind) {
    QueryNode *n = arena_push(arena, sizeof *n);
    n->kind = kind;
    return n;
}

static QueryNode *match_all(Parser *p) { return node_new(p->arena, Q_AND); }

typedef struct {
    QueryNode **items;
    size_t count;
    size_t capacity;
} NodeList;

static void nodes_push(Arena *arena, NodeList *l, QueryNode *n) {
    l->items = arena_grow(arena, l->items, &l->capacity, l->count, sizeof *l->items);
    l->items[l->count++] = n;
}

/* nodes_finish turns a list into its single element, or a node of kind over all of them. */
static QueryNode *nodes_finish(Arena *arena, NodeList *l, QueryKind kind) {
    if (l->count == 1) return l->items[0];
    QueryNode *n = node_new(arena, kind);
    n->kid_count = (uint32_t)l->count;
    n->kids = l->items;
    return n;
}

static QueryNode *pair(Arena *arena, QueryNode *a, QueryNode *b) {
    NodeList l = {0};
    nodes_push(arena, &l, a);
    nodes_push(arena, &l, b);
    return nodes_finish(arena, &l, Q_AND);
}

/* with_filter pairs a node with the file: or folder: filter that preceded it in the same word. */
static QueryNode *with_filter(Parser *p, QueryNode *filter, QueryNode *n) { return filter ? pair(p->arena, filter, n) : n; }

static const Token *peek(const Parser *p) { return p->pos < p->tokens.count ? &p->tokens.items[p->pos] : nullptr; }

static QueryNode *parse_or(Parser *p);

static QueryNode *fail(Parser *p, String key, const Err *why) {
    if (p->error == ERR_OK) p->error = err_set(p->err, ERR_PARSE, "%.*s: %s", (int)key.len, key.data, why->msg);
    return match_all(p);
}

static QueryNode *text_node(Parser *p, QueryNode t, String s, bool mode_set, bool no_wildcards) {
    QueryNode *n = node_new(p->arena, Q_TEXT);
    *n = t;
    n->kind = Q_TEXT;
    n->text = str_copy(p->arena, s);
    if (!mode_set) {
        if (p->d.regex) {
            n->mode = TEXT_REGEX;
        } else if (!no_wildcards && str_contains_any(s, S("*?"))) {
            n->mode = TEXT_WILDCARD;
        } else if (p->d.whole_word) {
            n->mode = TEXT_WHOLEWORD;
        }
    }
    return n;
}

static QueryNode *ext_node(Parser *p, String arg) {
    QueryNode *n = node_new(p->arena, Q_EXT);
    n->exts = arena_push(p->arena, (arg.len + 1) * sizeof *n->exts);
    size_t start = 0;
    for (size_t i = 0; i <= arg.len; i++) {
        if (i < arg.len && arg.data[i] != ';' && arg.data[i] != ',') continue;
        String ext = str_trim(str_slice(arg, start, i));
        ext = str_trim_prefix(ext, S("."));
        if (ext.len) n->exts[n->ext_count++] = str_lower_ascii(p->arena, ext);
        start = i + 1;
    }
    return n;
}

static QueryNode *range_node(Parser *p, QueryKind kind, String key, String arg, ValueParser parse) {
    Err why;
    Range r;
    if (parse_range(arg, parse, p->d.now, &r, &why) != ERR_OK) return fail(p, key, &why);
    QueryNode *n = node_new(p->arena, kind);
    n->range = r;
    return n;
}

static bool is_key(String key, const char *name) { return str_equal(key, S(name)); }

/* parse_word peels modifier and function prefixes off a word such as "folder:case:regex:^foo". */
static QueryNode *parse_word(Parser *p, String w) {
    QueryNode t = {.kind = Q_TEXT, .match_path = p->d.match_path, .case_sensitive = p->d.case_sensitive};
    QueryNode *type_filter = nullptr;
    bool mode_set = false, no_wildcards = false;
    String rest = w;
    for (;;) {
        size_t colon;
        if (!str_find_char(rest, ':', &colon)) break;
        char key_bytes[32];
        if (colon >= sizeof key_bytes) break;
        for (size_t i = 0; i < colon; i++) {
            char c = rest.data[i];
            key_bytes[i] = c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
        }
        String key = {key_bytes, colon};
        String arg = str_slice(rest, colon + 1, rest.len);
        if (is_key(key, "case")) {
            t.case_sensitive = true;
        } else if (is_key(key, "nocase")) {
            t.case_sensitive = false;
        } else if (is_key(key, "regex")) {
            t.mode = TEXT_REGEX, mode_set = true;
        } else if (is_key(key, "wfn") || is_key(key, "wholefilename") || is_key(key, "exact")) {
            t.mode = TEXT_WHOLENAME, mode_set = true;
        } else if (is_key(key, "ww") || is_key(key, "wholeword")) {
            t.mode = TEXT_WHOLEWORD, mode_set = true;
        } else if (is_key(key, "wildcards")) {
            t.mode = TEXT_WILDCARD, mode_set = true;
        } else if (is_key(key, "nowildcards")) {
            no_wildcards = true;
        } else if (is_key(key, "path")) {
            t.match_path = true;
        } else if (is_key(key, "nopath")) {
            t.match_path = false;
        } else if (is_key(key, "file") || is_key(key, "files")) {
            type_filter = node_new(p->arena, Q_ISDIR);
        } else if (is_key(key, "folder") || is_key(key, "folders") || is_key(key, "dir") || is_key(key, "dirs")) {
            type_filter = node_new(p->arena, Q_ISDIR);
            type_filter->dir = true;
        } else if (is_key(key, "ext")) {
            return with_filter(p, type_filter, ext_node(p, arg));
        } else if (is_key(key, "size")) {
            return with_filter(p, type_filter, range_node(p, Q_SIZE, key, arg, parse_size_value));
        } else if (is_key(key, "dm") || is_key(key, "datemodified")) {
            return with_filter(p, type_filter, range_node(p, Q_MODIFIED, key, arg, parse_date_value));
        } else if (is_key(key, "dc") || is_key(key, "datecreated")) {
            return with_filter(p, type_filter, range_node(p, Q_CREATED, key, arg, parse_date_value));
        } else if (is_key(key, "len")) {
            return with_filter(p, type_filter, range_node(p, Q_NAMELEN, key, arg, parse_int_value));
        } else if (is_key(key, "depth") || is_key(key, "parents")) {
            return with_filter(p, type_filter, range_node(p, Q_DEPTH, key, arg, parse_int_value));
        } else if (is_key(key, "parent") || is_key(key, "infolder")) {
            QueryNode *n = node_new(p->arena, is_key(key, "parent") ? Q_PARENT : Q_INFOLDER);
            n->path = str_copy(p->arena, arg);
            return with_filter(p, type_filter, n);
        } else {
            break;
        }
        rest = arg;
    }
    if (rest.len == 0) return type_filter ? type_filter : match_all(p);
    return with_filter(p, type_filter, text_node(p, t, rest, mode_set, no_wildcards));
}

static QueryNode *parse_unary(Parser *p) {
    const Token *t = &p->tokens.items[p->pos++];
    switch (t->kind) {
    case TOK_NOT: {
        QueryNode *n = node_new(p->arena, Q_NOT);
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
    for (const Token *t; (t = peek(p)) && t->kind != TOK_OR && t->kind != TOK_CLOSE;)
        nodes_push(p->arena, &kids, parse_unary(p));
    return nodes_finish(p->arena, &kids, Q_AND);
}

static QueryNode *parse_or(Parser *p) {
    NodeList kids = {0};
    nodes_push(p->arena, &kids, parse_and(p));
    for (const Token *t; (t = peek(p)) && t->kind == TOK_OR;) {
        p->pos++;
        nodes_push(p->arena, &kids, parse_and(p));
    }
    return nodes_finish(p->arena, &kids, Q_OR);
}

Error query_parse(Arena *arena, String text, QueryDefaults defaults, QueryNode **query, Err *err) {
    *query = nullptr;
    if (!defaults.now) defaults.now = time(nullptr);
    Parser p = {.arena = arena, .d = defaults, .err = err};
    p.tokens = lex(arena, text);
    QueryNode *n = parse_or(&p);
    /* A stray ">" ends a group that was never opened; skip it and keep going. */
    while (p.error == ERR_OK && p.pos < p.tokens.count) {
        p.pos++;
        if (p.pos < p.tokens.count) n = pair(arena, n, parse_or(&p));
    }
    if (p.error == ERR_OK) *query = n;
    return p.error;
}

QueryNode *query_restrict(Arena *arena, QueryNode *node, String path, bool files_only, bool dirs_only) {
    NodeList kids = {0};
    nodes_push(arena, &kids, node);
    if (path.len) {
        QueryNode *n = node_new(arena, Q_INFOLDER);
        n->path = str_copy(arena, path);
        nodes_push(arena, &kids, n);
    }
    if (files_only) nodes_push(arena, &kids, node_new(arena, Q_ISDIR));
    if (dirs_only) {
        QueryNode *n = node_new(arena, Q_ISDIR);
        n->dir = true;
        nodes_push(arena, &kids, n);
    }
    return nodes_finish(arena, &kids, Q_AND);
}

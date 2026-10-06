#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    Arena *arena;
    const char *p, *end;
    Err *err;
    bool failed;
    int depth;
} Parser;

static void fail(Parser *ps, const char *what) {
    if (!ps->failed) err_set(ps->err, "%s", what);
    ps->failed = true;
}

static void skip_space(Parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static bool literal(Parser *ps, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) return false;
    ps->p += n;
    return true;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_utf8(StrBuf *sb, uint32_t cp) {
    char b[4];
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | cp >> 6);
        b[1] = (char)(0x80 | (cp & 0x3F));
        sb_append(sb, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | cp >> 12);
        b[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        sb_append(sb, b, 3);
    } else {
        b[0] = (char)(0xF0 | cp >> 18);
        b[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        b[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        sb_append(sb, b, 4);
    }
}

static bool read_hex4(Parser *ps, uint32_t *out) {
    if (ps->end - ps->p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(ps->p[i]);
        if (d < 0) return false;
        v = v << 4 | (uint32_t)d;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static const char *parse_string(Parser *ps, size_t *len) {
    ps->p++; /* opening quote */
    StrBuf sb = {0};
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') {
            sb_putc(&sb, c);
            continue;
        }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
        case '"': sb_putc(&sb, '"'); break;
        case '\\': sb_putc(&sb, '\\'); break;
        case '/': sb_putc(&sb, '/'); break;
        case 'b': sb_putc(&sb, '\b'); break;
        case 'f': sb_putc(&sb, '\f'); break;
        case 'n': sb_putc(&sb, '\n'); break;
        case 'r': sb_putc(&sb, '\r'); break;
        case 't': sb_putc(&sb, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(ps, &cp)) {
                fail(ps, "invalid \\u escape");
                sb_free(&sb);
                return NULL;
            }
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                ps->p += 2;
                uint32_t lo;
                if (read_hex4(ps, &lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            put_utf8(&sb, cp);
            break;
        }
        default:
            fail(ps, "invalid escape in string");
            sb_free(&sb);
            return NULL;
        }
    }
    if (ps->p >= ps->end) {
        fail(ps, "unterminated string");
        sb_free(&sb);
        return NULL;
    }
    ps->p++;
    *len = sb.len;
    const char *s = arena_strndup(ps->arena, sb.data ? sb.data : "", sb.len);
    sb_free(&sb);
    return s;
}

static JsonValue *parse_value(Parser *ps);

typedef struct {
    JsonValue **items;
    const char **keys;
    size_t len, cap;
} Members;

static void members_push(Members *m, const char *key, JsonValue *v) {
    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->items = xrealloc(m->items, m->cap * sizeof *m->items);
        m->keys = xrealloc(m->keys, m->cap * sizeof *m->keys);
    }
    m->items[m->len] = v;
    m->keys[m->len] = key;
    m->len++;
}

static void finish_members(Parser *ps, JsonValue *v, Members *m, bool keep_keys) {
    v->count = m->len;
    v->items = arena_alloc(ps->arena, (m->len ? m->len : 1) * sizeof *v->items);
    memcpy(v->items, m->items, m->len * sizeof *v->items);
    if (keep_keys) {
        v->keys = arena_alloc(ps->arena, (m->len ? m->len : 1) * sizeof *v->keys);
        memcpy(v->keys, m->keys, m->len * sizeof *v->keys);
    }
    free(m->items);
    free(m->keys);
}

static void parse_container(Parser *ps, JsonValue *v, bool object) {
    char close = object ? '}' : ']';
    ps->p++;
    Members m = {0};
    skip_space(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        finish_members(ps, v, &m, object);
        return;
    }
    for (;;) {
        const char *key = NULL;
        if (object) {
            skip_space(ps);
            size_t klen;
            if (ps->p >= ps->end || *ps->p != '"') {
                fail(ps, "expected object key");
                break;
            }
            key = parse_string(ps, &klen);
            if (!key) break;
            skip_space(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                fail(ps, "expected ':' after object key");
                break;
            }
            ps->p++;
        }
        JsonValue *item = parse_value(ps);
        if (!item) break;
        members_push(&m, key, item);
        skip_space(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            break;
        }
        fail(ps, object ? "expected ',' or '}'" : "expected ',' or ']'");
        break;
    }
    finish_members(ps, v, &m, object);
}

static JsonValue *parse_value(Parser *ps) {
    skip_space(ps);
    if (ps->p >= ps->end) {
        fail(ps, "unexpected end of JSON input");
        return NULL;
    }
    if (++ps->depth > 64) {
        fail(ps, "JSON nested too deeply");
        return NULL;
    }
    JsonValue *v = arena_calloc(ps->arena, 1, sizeof *v);
    const char *start = ps->p;
    char c = *ps->p;
    if (c == '{' || c == '[') {
        v->type = c == '{' ? JSON_OBJECT : JSON_ARRAY;
        parse_container(ps, v, c == '{');
    } else if (c == '"') {
        v->type = JSON_STRING;
        v->str = parse_string(ps, &v->str_len);
    } else if (literal(ps, "true")) {
        v->type = JSON_BOOL;
        v->boolean = true;
    } else if (literal(ps, "false")) {
        v->type = JSON_BOOL;
    } else if (literal(ps, "null")) {
        v->type = JSON_NULL;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof buf - 1 && strchr("+-.eE0123456789", *ps->p)) buf[n++] = *ps->p++;
        buf[n] = '\0';
        char *endp;
        v->type = JSON_NUMBER;
        v->number = strtod(buf, &endp);
        if (*endp) fail(ps, "invalid number");
    } else {
        fail(ps, "invalid character in JSON");
    }
    ps->depth--;
    if (ps->failed) return NULL;
    v->raw = start;
    v->raw_len = (size_t)(ps->p - start);
    return v;
}

JsonValue *json_parse(Arena *arena, const char *text, size_t len, Err *err) {
    Parser ps = {.arena = arena, .p = text, .end = text + len, .err = err};
    JsonValue *v = parse_value(&ps);
    if (!v) return NULL;
    skip_space(&ps);
    if (ps.p != ps.end) {
        err_set(err, "invalid character after top-level value");
        return NULL;
    }
    return v;
}

const JsonValue *json_get(const JsonValue *object, const char *key) {
    if (!object || object->type != JSON_OBJECT) return NULL;
    for (size_t i = object->count; i-- > 0;)
        if (strcmp(object->keys[i], key) == 0) return object->items[i];
    return NULL;
}

const char *json_string(const JsonValue *object, const char *key, const char *fallback) {
    const JsonValue *v = json_get(object, key);
    return v && v->type == JSON_STRING ? v->str : fallback;
}

bool json_bool(const JsonValue *object, const char *key) {
    const JsonValue *v = json_get(object, key);
    return v && v->type == JSON_BOOL && v->boolean;
}

double json_number(const JsonValue *object, const char *key, double fallback) {
    const JsonValue *v = json_get(object, key);
    return v && v->type == JSON_NUMBER ? v->number : fallback;
}

void json_write_string(StrBuf *sb, const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    sb_grow(sb, len + 2);
    sb_putc(sb, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_append(sb, "\\\"", 2); break;
        case '\\': sb_append(sb, "\\\\", 2); break;
        case '\n': sb_append(sb, "\\n", 2); break;
        case '\r': sb_append(sb, "\\r", 2); break;
        case '\t': sb_append(sb, "\\t", 2); break;
        case '<': sb_append(sb, "\\u003c", 6); break;
        case '>': sb_append(sb, "\\u003e", 6); break;
        case '&': sb_append(sb, "\\u0026", 6); break;
        default:
            if (c < 0x20) {
                char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
                sb_append(sb, esc, 6);
            } else {
                sb_putc(sb, (char)c);
            }
        }
    }
    sb_putc(sb, '"');
}

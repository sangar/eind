#ifndef EIND_JSON_H
#define EIND_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "util.h"

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct JsonValue {
    JsonType type;
    bool boolean;
    double number;
    const char *str; /* decoded, NUL-terminated */
    size_t str_len;
    struct JsonValue **items; /* array elements or object values */
    const char **keys;        /* object keys */
    size_t count;
    const char *raw; /* the value exactly as it appeared in the input */
    size_t raw_len;
} JsonValue;

/* json_parse builds the value tree in the arena. */
JsonValue *json_parse(Arena *arena, const char *text, size_t len, Err *err);
const JsonValue *json_get(const JsonValue *object, const char *key);
const char *json_string(const JsonValue *object, const char *key, const char *fallback);
bool json_bool(const JsonValue *object, const char *key);
double json_number(const JsonValue *object, const char *key, double fallback);

void json_write_string(StrBuf *sb, const char *s, size_t len);

#endif

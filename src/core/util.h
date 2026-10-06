#ifndef EIND_UTIL_H
#define EIND_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Err carries a human readable message up to whoever can report it. */
typedef struct {
    char msg[512];
} Err;

void err_set(Err *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

typedef struct {
    char *data;
    size_t len, cap;
} StrBuf;

void sb_grow(StrBuf *sb, size_t extra);
void sb_append(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_putc(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_clear(StrBuf *sb);
const char *sb_cstr(StrBuf *sb);
void sb_free(StrBuf *sb);

typedef struct {
    uint32_t *data;
    size_t len, cap;
} U32Vec;

void u32vec_push(U32Vec *v, uint32_t x);
void u32vec_free(U32Vec *v);

/* StrList owns its strings. */
typedef struct {
    char **items;
    size_t len, cap;
} StrList;

void strlist_push(StrList *l, const char *s);
void strlist_push_owned(StrList *l, char *s);
void strlist_clear(StrList *l);
void strlist_free(StrList *l);
void strlist_copy(StrList *dst, const StrList *src);
bool strlist_contains(const StrList *l, const char *s);

int64_t monotonic_ms(void);
double elapsed_ms_since(int64_t start_us);
int64_t monotonic_us(void);

void ascii_lower(char *dst, const char *src, size_t n);
bool has_prefix(const char *s, const char *prefix);
bool has_suffix(const char *s, const char *suffix);
bool parse_int64(const char *s, int64_t *out);

const char *home_dir(void);
bool mkdir_p(const char *path, mode_t mode, Err *err);
char *path_join(const char *dir, const char *name);
char *path_dir(const char *path);
const char *path_base(const char *path);
char *path_clean(const char *path);
char *path_abs(const char *path);
char *expand_home(const char *path);
bool read_file(const char *path, StrBuf *out, Err *err);
bool write_file_atomic(const char *path, const void *data, size_t len, mode_t mode, Err *err);

/* commas formats 1234567 as "1,234,567". */
const char *commas(int64_t n, char buf[32]);
/* human_size formats a byte count as "1.5 MB". */
const char *human_size(int64_t n, char buf[32]);
/* format_rfc3339 writes local time such as 2024-03-13T10:00:00+01:00. */
const char *format_rfc3339(int64_t t, char buf[40]);
const char *format_short_time(int64_t t, char buf[32]);
int utf8_count(const char *s, size_t n);

#endif

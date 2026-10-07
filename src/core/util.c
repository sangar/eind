#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void out_of_memory(void) {
    fputs("eind: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

static char *xstrndup(const char *s, size_t n);

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void err_set(Err *err, const char *fmt, ...) {
    if (!err) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
}

void sb_grow(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < sb->len + extra + 1) cap *= 2;
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
}

void sb_append(StrBuf *sb, const char *s, size_t n) {
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }

void sb_putc(StrBuf *sb, char c) { sb_append(sb, &c, 1); }

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n > 0) {
        sb_grow(sb, (size_t)n);
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
        sb->len += (size_t)n;
    }
    va_end(ap);
}

void sb_clear(StrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

const char *sb_cstr(StrBuf *sb) {
    sb_grow(sb, 0);
    sb->data[sb->len] = '\0';
    return sb->data;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    *sb = (StrBuf){0};
}

void u32vec_push(U32Vec *v, uint32_t x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->data = xrealloc(v->data, v->cap * sizeof *v->data);
    }
    v->data[v->len++] = x;
}

void u32vec_free(U32Vec *v) {
    free(v->data);
    *v = (U32Vec){0};
}

void strlist_push_owned(StrList *l, char *s) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->len++] = s;
}

void strlist_push(StrList *l, const char *s) { strlist_push_owned(l, xstrdup(s)); }

void strlist_clear(StrList *l) {
    for (size_t i = 0; i < l->len; i++) free(l->items[i]);
    l->len = 0;
}

void strlist_free(StrList *l) {
    strlist_clear(l);
    free(l->items);
    *l = (StrList){0};
}

void strlist_copy(StrList *dst, const StrList *src) {
    for (size_t i = 0; i < src->len; i++) strlist_push(dst, src->items[i]);
}

int64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t monotonic_ms(void) { return monotonic_us() / 1000; }

double elapsed_ms_since(int64_t start_us) { return (double)(monotonic_us() - start_us) / 1000.0; }

void ascii_lower(char *dst, const char *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
}

static unsigned char lower_byte(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int ascii_casecmp(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = min_size(alen, blen);
    for (size_t i = 0; i < n; i++) {
        int x = lower_byte((unsigned char)a[i]), y = lower_byte((unsigned char)b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return (alen > blen) - (alen < blen);
}

bool has_prefix(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

bool parse_int64(const char *s, int64_t *out) {
    while (*s == ' ') s++;
    if (!*s) return false;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    while (*end == ' ') end++;
    if (errno || *end) return false;
    *out = v;
    return true;
}

const char *home_dir(void) {
    const char *home = getenv("HOME");
    if (home && *home) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : ".";
}

bool mkdir_p(const char *path, mode_t mode, Err *err) {
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (mkdir(p, mode) != 0 && errno != EEXIST) {
            err_set(err, "mkdir %s: %s", p, strerror(errno));
            free(p);
            return false;
        }
        *s = '/';
    }
    bool ok = mkdir(p, mode) == 0 || errno == EEXIST;
    if (!ok) err_set(err, "mkdir %s: %s", p, strerror(errno));
    free(p);
    return ok;
}

char *path_join(const char *dir, const char *name) {
    size_t n = strlen(dir);
    StrBuf sb = {0};
    sb_append(&sb, dir, n);
    if (n == 0 || dir[n - 1] != '/') sb_putc(&sb, '/');
    sb_puts(&sb, name);
    return sb.data;
}

char *path_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

const char *path_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

/* path_clean resolves ".", ".." and repeated slashes lexically, like Go's filepath.Clean. */
char *path_clean(const char *path) {
    if (!*path) return xstrdup(".");
    bool rooted = path[0] == '/';
    size_t n = strlen(path);
    char *out = xmalloc(n + 2);
    size_t w = 0, dotdot = 0;
    if (rooted) {
        out[w++] = '/';
        dotdot = 1;
    }
    size_t r = rooted ? 1 : 0;
    while (r < n) {
        if (path[r] == '/') {
            r++;
        } else if (path[r] == '.' && (r + 1 == n || path[r + 1] == '/')) {
            r++;
        } else if (path[r] == '.' && path[r + 1] == '.' && (r + 2 == n || path[r + 2] == '/')) {
            r += 2;
            if (w > dotdot) {
                w--;
                while (w > dotdot && out[w] != '/') w--;
            } else if (!rooted) {
                if (w > 0) out[w++] = '/';
                out[w++] = '.';
                out[w++] = '.';
                dotdot = w;
            }
        } else {
            if ((rooted && w != 1) || (!rooted && w != 0)) out[w++] = '/';
            while (r < n && path[r] != '/') out[w++] = path[r++];
        }
    }
    if (w == 0) out[w++] = '.';
    out[w] = '\0';
    return out;
}

char *path_abs(const char *path) {
    char *expanded = expand_home(path);
    if (expanded[0] == '/') {
        char *clean = path_clean(expanded);
        free(expanded);
        return clean;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    char *joined = path_join(cwd, expanded);
    char *clean = path_clean(joined);
    free(joined);
    free(expanded);
    return clean;
}

char *expand_home(const char *path) {
    if (strcmp(path, "~") == 0) return xstrdup(home_dir());
    if (has_prefix(path, "~/")) return path_join(home_dir(), path + 2);
    return xstrdup(path);
}

bool read_file(const char *path, StrBuf *out, Err *err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        err_set(err, "%s: %s", path, strerror(errno));
        return false;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_append(out, buf, n);
    bool ok = !ferror(f);
    if (!ok) err_set(err, "%s: read error", path);
    fclose(f);
    sb_cstr(out);
    return ok;
}

bool write_file_atomic(const char *path, const void *data, size_t len, mode_t mode, Err *err) {
    char *dir = path_dir(path);
    bool ok = mkdir_p(dir, 0755, err);
    free(dir);
    if (!ok) return false;
    StrBuf tmp = {0};
    sb_printf(&tmp, "%s.tmp", path);
    int fd = open(tmp.data, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        err_set(err, "%s: %s", tmp.data, strerror(errno));
        sb_free(&tmp);
        return false;
    }
    const char *p = data;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            err_set(err, "%s: %s", tmp.data, strerror(errno));
            close(fd);
            unlink(tmp.data);
            sb_free(&tmp);
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    close(fd);
    ok = rename(tmp.data, path) == 0;
    if (!ok) err_set(err, "rename %s: %s", path, strerror(errno));
    sb_free(&tmp);
    return ok;
}

const char *commas(int64_t n, char buf[32]) {
    char digits[32];
    snprintf(digits, sizeof digits, "%lld", (long long)(n < 0 ? -n : n));
    size_t len = strlen(digits), w = 0;
    if (n < 0) buf[w++] = '-';
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) buf[w++] = ',';
        buf[w++] = digits[i];
    }
    buf[w] = '\0';
    return buf;
}

const char *human_size(int64_t n, char buf[32]) {
    if (n < 1024) {
        snprintf(buf, 32, "%lld B", (long long)n);
        return buf;
    }
    static const char *suffixes[] = {"KB", "MB", "GB", "TB", "PB"};
    double f = (double)n;
    for (size_t i = 0; i < countof(suffixes); i++) {
        f /= 1024;
        if (f < 1024) {
            snprintf(buf, 32, f < 10 ? "%.1f %s" : "%.0f %s", f, suffixes[i]);
            return buf;
        }
    }
    snprintf(buf, 32, "%.0f EB", f / 1024);
    return buf;
}

const char *format_rfc3339(int64_t t, char buf[40]) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    size_t n = strftime(buf, 40, "%Y-%m-%dT%H:%M:%S", &tm);
    long off = tm.tm_gmtoff;
    if (off == 0) {
        snprintf(buf + n, 40 - n, "Z");
    } else {
        char sign = off < 0 ? '-' : '+';
        if (off < 0) off = -off;
        snprintf(buf + n, 40 - n, "%c%02ld:%02ld", sign, off / 3600, off / 60 % 60);
    }
    return buf;
}

const char *format_short_time(int64_t t, char buf[32]) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(buf, 32, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

int utf8_count(const char *s, size_t n) {
    int count = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) count++;
    return count;
}

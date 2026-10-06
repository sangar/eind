#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fs.h"

static bool has_meta(const char *p) { return strpbrk(p, "*?[\\") != NULL; }

bool glob_valid(const char *pattern) {
    for (const char *p = pattern; *p; p++) {
        if (*p == '\\') {
            if (!*++p) return false;
        } else if (*p == '[') {
            const char *q = p + 1;
            if (*q == '!' || *q == '^') q++;
            if (*q == ']') q++;
            while (*q && *q != ']') q++;
            if (!*q) return false;
            p = q;
        }
    }
    return true;
}

/* match_class matches c against the bracket expression at *p and advances *p past it. */
static bool match_class(const char **pp, char c) {
    const char *p = *pp + 1;
    bool negated = *p == '!' || *p == '^';
    if (negated) p++;
    bool matched = false, first = true;
    while (*p && (*p != ']' || first)) {
        first = false;
        char lo = *p == '\\' && p[1] ? *++p : *p;
        p++;
        char hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            hi = *p == '\\' && p[1] ? *++p : *p;
            p++;
        }
        if (c >= lo && c <= hi) matched = true;
    }
    *pp = *p ? p + 1 : p;
    return matched != negated;
}

static bool at_segment_start(const char *pattern, const char *p) { return p == pattern || p[-1] == '/'; }

static bool match_from(const char *pattern, const char *p, const char *s) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*' && at_segment_start(pattern, p) && (p[2] == '/' || p[2] == '\0')) {
            if (p[2] == '\0') return true;
            const char *rest = p + 3;
            if (match_from(pattern, rest, s)) return true;
            for (const char *q = s; *q; q++)
                if (*q == '/' && match_from(pattern, rest, q + 1)) return true;
            return false;
        }
        switch (*p) {
        case '*':
            while (*p == '*') p++;
            for (const char *q = s;; q++) {
                if (match_from(pattern, p, q)) return true;
                if (!*q || *q == '/') return false;
            }
        case '?':
            if (!*s || *s == '/') return false;
            p++, s++;
            break;
        case '[':
            if (!*s || *s == '/' || !match_class(&p, *s)) return false;
            s++;
            break;
        case '\\':
            if (p[1]) p++;
            /* fall through */
        default:
            if (*p != *s) return false;
            p++, s++;
        }
    }
    return *s == '\0';
}

bool glob_match(const char *pattern, const char *s) { return match_from(pattern, pattern, s); }

bool excludes_init(Excludes *ex, const StrList *patterns, Err *err) {
    *ex = (Excludes){0};
    for (size_t i = 0; i < patterns->len; i++) {
        const char *raw = patterns->items[i];
        while (*raw == ' ' || *raw == '\t') raw++;
        if (!*raw) continue;
        char *p = expand_home(raw);
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = '\0';
        if (!glob_valid(p)) {
            err_set(err, "invalid exclude pattern: %s", p);
            free(p);
            excludes_free(ex);
            return false;
        }
        if (strchr(p, '/')) {
            while (n > 1 && p[n - 1] == '/') p[--n] = '\0';
            strlist_push_owned(&ex->paths, p);
        } else {
            strlist_push_owned(&ex->names, p);
        }
    }
    return true;
}

void excludes_free(Excludes *ex) {
    strlist_free(&ex->names);
    strlist_free(&ex->paths);
}

bool excludes_empty(const Excludes *ex) { return ex->names.len == 0 && ex->paths.len == 0; }

static bool path_pattern_matches(const char *pattern, const char *path) {
    if (!has_meta(pattern)) {
        size_t n = strlen(pattern);
        return strncmp(path, pattern, n) == 0 && (path[n] == '\0' || path[n] == '/' || (n == 1 && *pattern == '/'));
    }
    if (glob_match(pattern, path)) return true;
    StrBuf deep = {0};
    sb_puts(&deep, pattern);
    sb_puts(&deep, "/**");
    bool ok = glob_match(deep.data, path);
    sb_free(&deep);
    return ok;
}

bool excludes_match(const Excludes *ex, const char *path, const char *name) {
    for (size_t i = 0; i < ex->names.len; i++)
        if (glob_match(ex->names.items[i], name)) return true;
    for (size_t i = 0; i < ex->paths.len; i++)
        if (path_pattern_matches(ex->paths.items[i], path)) return true;
    return false;
}

int64_t stat_birthtime(const struct stat *st) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__)
    return (int64_t)st->st_birthtimespec.tv_sec;
#else
    (void)st;
    return 0;
#endif
}

bool fs_read_dir_portable(int dirfd, DirSkipFn skip, DirEntryFn add, void *ctx) {
    int fd = dup(dirfd);
    DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
    if (!dir) {
        if (fd >= 0) close(fd);
        return false;
    }
    bool complete = true;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) {
            complete = errno == 0;
            break;
        }
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        if (skip(ctx, name)) continue;
        struct stat st;
        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) add(ctx, name, &st);
    }
    closedir(dir);
    return complete;
}

#ifndef __APPLE__
bool fs_read_dir(int dirfd, DirSkipFn skip, DirEntryFn add, void *ctx) {
    return fs_read_dir_portable(dirfd, skip, add, ctx);
}
#endif

#include "excludes.h"

#include "mc/text/glob.h"
#include "mc/text/path.h"

static bool has_meta(String pattern) { return str_contains_any(pattern, S("*?[\\")); }

Error excludes_init(Arena *arena, Excludes *ex, StringList patterns, String home, Err *err) {
    *ex = (Excludes){0};
    for (size_t i = 0; i < patterns.count; i++) {
        String raw = str_trim(patterns.items[i]);
        if (raw.len == 0) continue;
        String p = path_expand_home(arena, raw, home);
        if (!glob_valid(p)) return err_set(err, ERR_INVALID_ARGUMENT, "invalid exclude pattern: %.*s", (int)p.len, p.data);
        if (str_contains(p, S("/"))) {
            while (p.len > 1 && p.data[p.len - 1] == '/') p.len--;
            strlist_push(arena, &ex->paths, str_copy(arena, p));
        } else {
            strlist_push(arena, &ex->names, p);
        }
    }
    return ERR_OK;
}

bool excludes_empty(const Excludes *ex) { return ex->names.count == 0 && ex->paths.count == 0; }

/* below_glob matches what pattern excludes below itself, without building a pattern that ends in a slash and a double star. */
static bool below_glob(String pattern, String path) {
    for (size_t i = 0; i < path.len; i++)
        if (path.data[i] == '/' && glob_match(pattern, str_slice(path, 0, i))) return true;
    return false;
}

static bool path_pattern_matches(String pattern, String path) {
    if (!has_meta(pattern)) {
        if (!str_starts_with(path, pattern)) return false;
        return path.len == pattern.len || path.data[pattern.len] == '/' || str_equal(pattern, S("/"));
    }
    return glob_match(pattern, path) || below_glob(pattern, path);
}

bool excludes_match(const Excludes *ex, String path, String name) {
    for (size_t i = 0; i < ex->names.count; i++)
        if (glob_match(ex->names.items[i], name)) return true;
    for (size_t i = 0; i < ex->paths.count; i++)
        if (path_pattern_matches(ex->paths.items[i], path)) return true;
    return false;
}

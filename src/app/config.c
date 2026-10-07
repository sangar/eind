#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* home_relative writes a path under the home directory with a leading ~, the way users write excludes. */
static char *home_relative(const char *p) {
    const char *home = home_dir();
    size_t n = strlen(home);
    if (n > 0 && strncmp(p, home, n) == 0 && p[n] == '/') {
        StrBuf sb = {0};
        sb_printf(&sb, "~%s", p + n);
        return sb.data;
    }
    return xstrdup(p);
}

/*
 * The defaults leave out what a launcher never wants to offer: dependency
 * trees and version control internals, the platform's cache directory and
 * trash, and its virtual or foreign filesystems. On macOS they also leave out
 * sandboxed app data, because reading ~/Library/Containers makes macOS ask
 * for permission to "access data from other apps" on every rebuild.
 */
static void config_default_excludes(StrList *out) {
    strlist_push(out, "node_modules");
    strlist_push(out, ".git");
    strlist_push(out, ".cache");
#if defined(__APPLE__)
    char *cache = path_join(home_dir(), "Library/Caches");
    strlist_push_owned(out, home_relative(cache));
    free(cache);
    static const char *platform[] = {"~/.Trash", "~/Library/Containers", "~/Library/Group Containers", "/dev",
                                     "/System/Volumes", "/Volumes", "/private/var/vm"};
#elif defined(__linux__)
    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && *xdg_cache && strcmp(path_base(xdg_cache), ".cache") != 0) strlist_push_owned(out, home_relative(xdg_cache));
    static const char *platform[] = {"~/.local/share/Trash", "/proc", "/sys", "/dev", "/run"};
#else
    static const char *platform[] = {"~/.local/share/Trash", "/proc", "/dev"};
#endif
    for (size_t i = 0; i < countof(platform); i++) strlist_push(out, platform[i]);
}

static void config_default(Config *cfg) {
    *cfg = (Config){0};
    strlist_push(&cfg->roots, home_dir());
    config_default_excludes(&cfg->excludes);
}

void config_free(Config *cfg) {
    strlist_free(&cfg->roots);
    strlist_free(&cfg->excludes);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';
    return s;
}

bool config_load(const char *path, Config *cfg, Err *err) {
    StrBuf text = {0};
    struct stat st;
    if (stat(path, &st) != 0 && errno == ENOENT) {
        config_default(cfg);
        return true;
    }
    if (!read_file(path, &text, err)) return false;
    *cfg = (Config){0};
    int line_no = 0;
    char *save = NULL;
    bool ok = true;
    for (char *line = text.data; line && ok; line = save) {
        char *nl = strchr(line, '\n');
        save = nl ? nl + 1 : NULL;
        if (nl) *nl = '\0';
        line_no++;
        char *t = trim(line);
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) {
            err_set(err, "%s:%d: expected key = value", path, line_no);
            ok = false;
            break;
        }
        *eq = '\0';
        char *key = trim(t), *value = trim(eq + 1);
        if (strcmp(key, "root") == 0) {
            strlist_push_owned(&cfg->roots, expand_home(value));
        } else if (strcmp(key, "exclude") == 0) {
            strlist_push(&cfg->excludes, value);
        } else {
            err_set(err, "%s:%d: unknown key \"%s\"", path, line_no, key);
            ok = false;
        }
    }
    sb_free(&text);
    if (!ok) {
        config_free(cfg);
        return false;
    }
    if (cfg->roots.len == 0) strlist_push(&cfg->roots, home_dir());
    if (cfg->excludes.len == 0) config_default_excludes(&cfg->excludes);
    return true;
}

void config_render(const Config *cfg, StrBuf *out) {
    sb_puts(out,
            "# eind configuration\n"
            "#\n"
            "# root    = directory to index (repeat for several roots)\n"
            "# exclude = pattern to leave out. Without a slash it matches names\n"
            "#           (node_modules, *.tmp); with a slash it matches full paths\n"
            "#           and everything below (/proc, ~/Library/Caches, **/.git).\n"
            "#           Delete a line to index that location again. A file with\n"
            "#           no exclude lines at all uses the defaults listed here.\n"
            "#\n"
            "# Run `eind index` after changing this file.\n\n");
    for (size_t i = 0; i < cfg->roots.len; i++) sb_printf(out, "root = %s\n", cfg->roots.items[i]);
    sb_putc(out, '\n');
    for (size_t i = 0; i < cfg->excludes.len; i++) sb_printf(out, "exclude = %s\n", cfg->excludes.items[i]);
}

bool config_write_default(const char *path, bool *created, Err *err) {
    struct stat st;
    *created = false;
    if (stat(path, &st) == 0) return true;
    Config cfg;
    config_default(&cfg);
    StrBuf text = {0};
    config_render(&cfg, &text);
    config_free(&cfg);
    bool ok = write_file_atomic(path, text.data, text.len, 0644, err);
    sb_free(&text);
    *created = ok;
    return ok;
}

static char *xdg_dir(const char *env, const char *fallback) {
    const char *dir = getenv(env);
    if (dir && *dir) return xstrdup(dir);
    return path_join(home_dir(), fallback);
}

char *config_path(void) {
    const char *p = getenv("EIND_CONFIG");
    if (p && *p) return xstrdup(p);
    char *dir = xdg_dir("XDG_CONFIG_HOME", ".config");
    char *path = path_join(dir, "eind/config");
    free(dir);
    return path;
}

char *index_path(void) {
    const char *p = getenv("EIND_INDEX");
    if (p && *p) return xstrdup(p);
    char *dir = xdg_dir("XDG_DATA_HOME", ".local/share");
    char *path = path_join(dir, "eind/index.bin");
    free(dir);
    return path;
}

#include "config.h"

#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "../platform/defaults.h"

/* home_relative writes a path under the home directory with a leading ~, the way users write excludes. */
static String home_relative(Arena *arena, String path, String home) {
    String below;
    if (home.len && path_relative(home, path, &below)) return str_concat(arena, S("~/"), below);
    return path;
}

/*
 * The defaults leave out what a launcher never wants to offer: dependency
 * trees and version control internals, plus what the platform adds.
 */
static void config_default_excludes(Arena *arena, StringList *out) {
    strlist_push(arena, out, S("node_modules"));
    strlist_push(arena, out, S(".git"));
    strlist_push(arena, out, S(".cache"));
    StringList platform = {0};
    platform_default_excludes(arena, &platform);
    String home = env_home(arena);
    for (size_t i = 0; i < platform.count; i++) strlist_push(arena, out, home_relative(arena, platform.items[i], home));
}

static void config_default(Arena *arena, Config *cfg) {
    *cfg = (Config){0};
    strlist_push(arena, &cfg->roots, env_home(arena));
    config_default_excludes(arena, &cfg->excludes);
}

Error config_load(Arena *arena, String path, Config *cfg, Err *err) {
    String text;
    Error e = file_read_all(arena, path, &text, err);
    if (e == ERR_NOT_FOUND) {
        config_default(arena, cfg);
        return ERR_OK;
    }
    if (e != ERR_OK) return e;
    *cfg = (Config){0};
    String home = env_home(arena);
    StringList lines = strlist_from_lines(arena, text);
    for (size_t i = 0; i < lines.count; i++) {
        String line = str_trim(lines.items[i]);
        if (line.len == 0 || line.data[0] == '#') continue;
        String key, value;
        if (!str_cut(line, '=', &key, &value))
            return err_set(err, ERR_PARSE, "%.*s:%zu: expected key = value", (int)path.len, path.data, i + 1);
        key = str_trim(key);
        value = str_trim(value);
        if (str_equal(key, S("root"))) {
            strlist_push(arena, &cfg->roots, path_expand_home(arena, value, home));
        } else if (str_equal(key, S("exclude"))) {
            strlist_push(arena, &cfg->excludes, value);
        } else {
            return err_set(err, ERR_PARSE, "%.*s:%zu: unknown key \"%.*s\"", (int)path.len, path.data, i + 1,
                           (int)key.len, key.data);
        }
    }
    if (cfg->roots.count == 0) strlist_push(arena, &cfg->roots, home);
    if (cfg->excludes.count == 0) config_default_excludes(arena, &cfg->excludes);
    return ERR_OK;
}

void config_render(const Config *cfg, StringBuilder *out) {
    str_builder_append(out, S("# eind configuration\n"
                               "#\n"
                               "# root    = directory to index (repeat for several roots)\n"
                               "# exclude = pattern to leave out. Without a slash it matches names\n"
                               "#           (node_modules, *.tmp); with a slash it matches full paths\n"
                               "#           and everything below (/proc, ~/Library/Caches, **/.git).\n"
                               "#           Delete a line to index that location again. A file with\n"
                               "#           no exclude lines at all uses the defaults listed here.\n"
                               "#\n"
                               "# Run `eind index` after changing this file.\n\n"));
    for (size_t i = 0; i < cfg->roots.count; i++)
        str_builder_append_format(out, "root = %.*s\n", (int)cfg->roots.items[i].len, cfg->roots.items[i].data);
    str_builder_append_char(out, '\n');
    for (size_t i = 0; i < cfg->excludes.count; i++)
        str_builder_append_format(out, "exclude = %.*s\n", (int)cfg->excludes.items[i].len, cfg->excludes.items[i].data);
}

Error config_write_default(Arena *scratch, String path, bool *created, Err *err) {
    *created = false;
    if (file_exists(path)) return ERR_OK;
    Config cfg;
    config_default(scratch, &cfg);
    StringBuilder text = str_builder_create(scratch, 1024);
    config_render(&cfg, &text);
    Error e = file_write_atomic(path, str_builder_finish(&text), 0644, err);
    *created = e == ERR_OK;
    return e;
}

/* xdg_file is name in the XDG directory env names, or below fallback in the home directory. */
static String xdg_file(Arena *arena, const char *env, const char *fallback, const char *name) {
    String dir;
    if (!env_get(arena, S(env), &dir) || dir.len == 0) dir = path_join(arena, env_home(arena), S(fallback));
    return path_join(arena, dir, S(name));
}

static bool override(Arena *arena, const char *env, String *path) { return env_get(arena, S(env), path) && path->len; }

String config_path(Arena *arena) {
    String path;
    return override(arena, "EIND_CONFIG", &path) ? path : xdg_file(arena, "XDG_CONFIG_HOME", ".config", "eind/config");
}

String index_path(Arena *arena) {
    String path;
    return override(arena, "EIND_INDEX", &path) ? path : xdg_file(arena, "XDG_DATA_HOME", ".local/share", "eind/index.bin");
}

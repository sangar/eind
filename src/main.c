/* eind is an instant file search for the command line. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "mc/concurrency/threadpool.h"
#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/platform/service.h"
#include "mc/platform/terminal.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"
#include "app/build.h"
#include "app/config.h"
#include "app/daemon.h"
#include "app/output.h"
#include "app/server.h"
#include "app/tui.h"
#include "index/search.h"

#ifndef EIND_VERSION
#define EIND_VERSION "dev"
#endif

static const char usage_text[] =
    "eind - instant file search for the terminal\n"
    "\n"
    "Usage:\n"
    "  eind [options] [query...]     search; with no query, open the interactive view\n"
    "  eind index [--root DIR]...    build the index from the configured roots\n"
    "  eind watch                    keep the index up to date from filesystem events\n"
    "  eind serve                    watch, and answer queries over a Unix socket (for GUIs)\n"
    "  eind service enable|disable   run eind serve at login (launchd agent or systemd user unit)\n"
    "  eind status                   show where the index and config live, and their size\n"
    "  eind config [--init]          show the effective config, or write a default file\n"
    "  eind config edit              open the config in $VISUAL or $EDITOR, then check it\n"
    "  eind tui                      open the interactive view\n"
    "  eind version                  show the version\n"
    "\n"
    "Search options:\n"
    "  -r, --regex          treat terms as regular expressions\n"
    "  -i, --case           match case\n"
    "  -w, --whole-word     match whole words only\n"
    "  -p, --match-path     match against the full path instead of the name\n"
    "  -n, --max-results N  print at most N results\n"
    "  -o, --offset N       skip the first N results\n"
    "  -s, --sort KEY       path (default), name, size, dm, dc, ext or relevance\n"
    "  -d, --descending     reverse the sort order\n"
    "      --path DIR       only results inside DIR\n"
    "      --files          only files\n"
    "      --dirs           only folders\n"
    "\n"
    "Output options:\n"
    "      --json           JSON array          --csv          CSV with header\n"
    "  -0, --null           NUL-separated       --name-only    print names, not paths\n"
    "      --size           add a size column   --dm, --dc     add date columns\n"
    "      --count          print only the number of results\n"
    "      --color WHEN     auto (default), always or never\n"
    "\n"
    "Global options:\n"
    "      --config FILE    config file  (default %s, env EIND_CONFIG)\n"
    "      --index FILE     index file   (default %s, env EIND_INDEX)\n"
    "      --socket FILE    daemon socket (default %s, env EIND_SOCKET)\n"
    "  -h, --help           show this help\n"
    "      --version        show the version\n"
    "\n"
    "Query syntax:\n"
    "  space = AND    |  = OR      ! = NOT     <a b|c> = grouping   \"quoted words\"\n"
    "  *.txt          wildcards match the whole name\n"
    "  ext:pdf;docx   size:>10mb   size:1mb..5mb   size:large    dm:today   dm:2024-03\n"
    "  dm:lastweek    dm:last30days   dc:2024   len:>40   depth:3   file:   folder:\n"
    "  case:Readme    regex:^draft_\\d+   ww:log   wfn:Makefile   path:src/main\n"
    "  parent:~/Documents   infolder:~/Projects\n";

/* Exit statuses: 1 for failures, 2 for mistakes on the command line. */
enum { EXIT_USAGE = 2 };

static const char SERVICE_NAME[] = "eind";

static void print_usage(Arena *arena, FILE *out) {
    fprintf(out, usage_text, str_cstr(arena, config_path(arena)), str_cstr(arena, index_path(arena)),
            str_cstr(arena, default_socket_path(arena)));
}

static int fail(const char *msg) {
    fprintf(stderr, "eind: %s\n", msg);
    return EXIT_FAILURE;
}

static int usage_fail(const char *msg) {
    fprintf(stderr, "eind: %s\n", msg);
    return EXIT_USAGE;
}

/* ---- flags ---- */

typedef enum { FLAG_BOOL, FLAG_INT, FLAG_STRING, FLAG_LIST, FLAG_DURATION } FlagType;

/* A Flag's dst is a bool, an int64_t, a String, a StringList or, for a duration, an int64_t of milliseconds. */
typedef struct {
    const char *name;
    FlagType type;
    void *dst;
} Flag;

/* FlagError is the message for a flag that could not be read. */
typedef struct {
    char msg[300];
} FlagError;

/* parse_duration accepts Go-style durations such as 10s, 500ms, 1m30s. */
static bool parse_duration(String s, int64_t *ms) {
    int64_t ns;
    if (!fmt_parse_duration(s, &ns) || ns <= 0) return false;
    *ms = ns / NS_PER_MILLISECOND;
    return true;
}

static const Flag *find_flag(const Flag *flags, size_t count, String name) {
    for (size_t i = 0; i < count; i++)
        if (str_equal(S(flags[i].name), name)) return &flags[i];
    return nullptr;
}

static bool set_flag(Arena *arena, const Flag *f, String value, bool has_value, FlagError *why) {
    switch (f->type) {
    case FLAG_BOOL:
        if (!has_value || str_equal(value, S("true")) || str_equal(value, S("1"))) {
            *(bool *)f->dst = true;
        } else if (str_equal(value, S("false")) || str_equal(value, S("0"))) {
            *(bool *)f->dst = false;
        } else {
            snprintf(why->msg, sizeof why->msg, "invalid boolean value \"%.*s\" for -%s", (int)value.len, value.data, f->name);
            return false;
        }
        return true;
    case FLAG_INT:
        if (!str_parse_i64(str_trim(value), (int64_t *)f->dst)) {
            snprintf(why->msg, sizeof why->msg, "invalid value \"%.*s\" for flag -%s: parse error", (int)value.len,
                     value.data, f->name);
            return false;
        }
        return true;
    case FLAG_STRING: *(String *)f->dst = value; return true;
    case FLAG_LIST: strlist_push(arena, (StringList *)f->dst, value); return true;
    case FLAG_DURATION:
        if (!parse_duration(value, (int64_t *)f->dst)) {
            snprintf(why->msg, sizeof why->msg, "invalid value \"%.*s\" for flag -%s: parse error", (int)value.len,
                     value.data, f->name);
            return false;
        }
        return true;
    }
    return false;
}

/* Globals are the options every command accepts: where the config, index and socket live. */
typedef struct {
    String config_path, index_path, socket_path;
} Globals;

static Globals globals_default(Arena *arena) {
    return (Globals){config_path(arena), index_path(arena), default_socket_path(arena)};
}

/*
 * parse_flags reads the global flags and the command's own the way Go's
 * flag package does: -name, --name, -name=value, or -name value for flags
 * that take one; it stops at the first argument that is not a flag and
 * collects the rest as positional. It returns 0 on success, or the exit
 * status to stop with.
 */
static int parse_flags(Arena *arena, Globals *g, const Flag *flags, size_t count, StringList args, StringList *positional) {
    const Flag global[] = {{"config", FLAG_STRING, &g->config_path},
                           {"index", FLAG_STRING, &g->index_path},
                           {"socket", FLAG_STRING, &g->socket_path}};
    size_t i = 0;
    for (; i < args.count; i++) {
        String a = args.items[i];
        if (a.len < 2 || a.data[0] != '-') break;
        if (str_equal(a, S("--"))) {
            i++;
            break;
        }
        String name = str_trim_prefix(str_trim_prefix(a, S("-")), S("-"));
        String value;
        bool has_value = str_cut(name, '=', &name, &value);
        if (str_equal(name, S("h")) || str_equal(name, S("help"))) {
            print_usage(arena, stdout);
            exit(EXIT_SUCCESS);
        }
        const Flag *f = find_flag(global, countof(global), name);
        if (!f) f = find_flag(flags, count, name);
        FlagError why;
        if (!f) {
            snprintf(why.msg, sizeof why.msg, "flag provided but not defined: -%.*s (see eind --help)", (int)name.len,
                     name.data);
            return usage_fail(why.msg);
        }
        if (!has_value && f->type != FLAG_BOOL) {
            if (i + 1 >= args.count) {
                snprintf(why.msg, sizeof why.msg, "flag needs an argument: -%s (see eind --help)", f->name);
                return usage_fail(why.msg);
            }
            value = args.items[++i];
            has_value = true;
        }
        if (!set_flag(arena, f, has_value ? value : S(""), has_value, &why)) {
            fprintf(stderr, "eind: %s (see eind --help)\n", why.msg);
            return EXIT_USAGE;
        }
    }
    for (; i < args.count; i++) strlist_push(arena, positional, args.items[i]);
    return 0;
}

/* Flags that take a value, so the pre-pass keeps the next argument with them. */
static bool takes_value(String name) {
    static const char *const names[] = {"n", "max-results", "o", "offset", "s", "sort", "path", "color",
                                        "config", "index", "root", "save-interval", "socket"};
    for (size_t i = 0; i < countof(names); i++)
        if (str_equal(S(names[i]), name)) return true;
    return false;
}

/*
 * split_args lets options appear anywhere on the line, while flag parsing
 * reads them only before the first word. Everything after "--" is query
 * text; *literal says the first word came after "--", so it is never taken
 * as a subcommand.
 */
static void split_args(Arena *arena, int argc, char **argv, StringList *positional, StringList *flags, bool *literal) {
    *literal = false;
    for (int i = 0; i < argc; i++) {
        String a = S(argv[i]);
        if (str_equal(a, S("--"))) {
            *literal = positional->count == 0;
            for (i++; i < argc; i++) strlist_push(arena, positional, S(argv[i]));
            break;
        }
        String number = str_slice(a, 1, a.len);
        if (a.len < 2 || a.data[0] != '-' || str_starts_with(a, S("-.")) ||
            (str_is_digits(number) && !str_equal(a, S("-0")))) {
            strlist_push(arena, positional, a);
            continue;
        }
        strlist_push(arena, flags, a);
        String name = a;
        while (str_starts_with(name, S("-"))) name = str_slice(name, 1, name.len);
        if (!str_contains(name, S("=")) && takes_value(name) && i + 1 < argc) strlist_push(arena, flags, S(argv[++i]));
    }
}

/* ---- search ---- */

typedef struct {
    bool regex, case_sensitive, whole_word, match_path;
    int64_t max_results, offset;
    String sort;
    bool descending;
    String path;
    bool files_only, dirs_only;
    bool json, csv, null_sep, name_only;
    bool show_size, show_modified, show_created;
    bool count;
    String color;
    bool show_version;
} SearchFlags;

static bool use_color(Arena *arena, String mode) {
    if (str_equal(mode, S("always"))) return true;
    if (str_equal(mode, S("never"))) return false;
    String no_color;
    return terminal_is_terminal(1) && !env_get(arena, S("NO_COLOR"), &no_color);
}

static OutputOptions output_options(Arena *arena, const SearchFlags *f) {
    OutputOptions o = {.null_sep = f->null_sep,
                       .name_only = f->name_only,
                       .show_size = f->show_size,
                       .show_modified = f->show_modified,
                       .show_created = f->show_created,
                       .color = use_color(arena, f->color)};
    if (f->json) o.format = FORMAT_JSON;
    else if (f->csv) o.format = FORMAT_CSV;
    return o;
}

/* print_hits orders and prints the hits of one query the way the flags ask. */
static int print_hits(Arena *arena, const Snapshot *s, IdList hits, const QueryNode *query, SortKey sort,
                      const SearchFlags *f) {
    if (f->count) {
        printf("%zu\n", hits.count);
        return EXIT_SUCCESS;
    }
    int64_t keep = f->max_results > 0 ? f->offset + f->max_results : -1;
    size_t kept = sort == SORT_RELEVANCE ? search_rank(arena, s, hits.items, hits.count, query, keep)
                                         : search_top(arena, s, hits.items, hits.count, sort, f->descending, keep);
    size_t from = min_size(f->offset > 0 ? (size_t)f->offset : 0, kept);
    size_t count = kept - from;
    if (f->max_results > 0 && (uint64_t)f->max_results < count) count = (size_t)f->max_results;
    OutputOptions o = output_options(arena, f);
    Err err;
    fflush(stdout);
    if (output_write_hits(1, arena, s, hits.items + from, count, &o, &err) != ERR_OK) return fail(err.msg);
    return EXIT_SUCCESS;
}

static int search_and_print(Arena *arena, const Snapshot *s, String text, QueryDefaults defaults, SortKey sort,
                            const SearchFlags *f) {
    Err err;
    QueryNode *query;
    if (query_parse(arena, text, defaults, &query, &err) != ERR_OK) return fail(err.msg);
    ThreadPool *cpu;
    if (threadpool_create(0, &cpu, &err) != ERR_OK) return fail(err.msg);
    IdList hits;
    Error e = search_run(cpu, arena, s, query_restrict(arena, query, f->path, f->files_only, f->dirs_only), nullptr,
                         &hits, &err);
    threadpool_destroy(cpu);
    return e == ERR_OK ? print_hits(arena, s, hits, query, sort, f) : fail(err.msg);
}

static int cmd_search(Arena *arena, StringList flag_args, StringList terms, bool force_tui) {
    Globals g = globals_default(arena);
    SearchFlags f = {.sort = S("path"), .color = S("auto")};
    const Flag flags[] = {
        {"r", FLAG_BOOL, &f.regex},          {"regex", FLAG_BOOL, &f.regex},
        {"i", FLAG_BOOL, &f.case_sensitive}, {"case", FLAG_BOOL, &f.case_sensitive},
        {"w", FLAG_BOOL, &f.whole_word},     {"whole-word", FLAG_BOOL, &f.whole_word},
        {"p", FLAG_BOOL, &f.match_path},     {"match-path", FLAG_BOOL, &f.match_path},
        {"n", FLAG_INT, &f.max_results},     {"max-results", FLAG_INT, &f.max_results},
        {"o", FLAG_INT, &f.offset},          {"offset", FLAG_INT, &f.offset},
        {"s", FLAG_STRING, &f.sort},         {"sort", FLAG_STRING, &f.sort},
        {"d", FLAG_BOOL, &f.descending},     {"descending", FLAG_BOOL, &f.descending},
        {"0", FLAG_BOOL, &f.null_sep},       {"null", FLAG_BOOL, &f.null_sep},
        {"path", FLAG_STRING, &f.path},      {"files", FLAG_BOOL, &f.files_only},
        {"dirs", FLAG_BOOL, &f.dirs_only},   {"json", FLAG_BOOL, &f.json},
        {"csv", FLAG_BOOL, &f.csv},          {"name-only", FLAG_BOOL, &f.name_only},
        {"size", FLAG_BOOL, &f.show_size},   {"dm", FLAG_BOOL, &f.show_modified},
        {"dc", FLAG_BOOL, &f.show_created},  {"count", FLAG_BOOL, &f.count},
        {"color", FLAG_STRING, &f.color},    {"version", FLAG_BOOL, &f.show_version},
    };
    StringList extra = {0};
    int rc = parse_flags(arena, &g, flags, countof(flags), flag_args, &extra);
    if (rc) return rc;
    if (f.show_version) {
        printf("eind %s\n", EIND_VERSION);
        return EXIT_SUCCESS;
    }
    Err err;
    SortKey sort;
    if (sort_key_parse(f.sort, &sort, &err) != ERR_OK) return fail(err.msg);
    bool want_tui = force_tui || (terms.count == 0 && terminal_is_terminal(1) && !f.count && !f.json && !f.csv);
    Snapshot *s;
    if (load_or_build(g.config_path, g.index_path, &s, &err) != ERR_OK) return fail(err.msg);
    QueryDefaults defaults = {.regex = f.regex, .case_sensitive = f.case_sensitive, .whole_word = f.whole_word,
                              .match_path = f.match_path};
    if (want_tui) {
        String chosen;
        rc = tui_run(arena, s, defaults, &chosen, &err) != ERR_OK ? fail(err.msg) : EXIT_SUCCESS;
        if (chosen.len) printf("%.*s\n", (int)chosen.len, chosen.data);
    } else {
        rc = search_and_print(arena, s, str_join(arena, terms, S(" ")), defaults, sort, &f);
    }
    snapshot_release(s);
    return rc;
}

/* ---- other commands ---- */

static int cmd_index(Arena *arena, StringList args) {
    Globals g = globals_default(arena);
    StringList roots = {0}, extra = {0};
    const Flag flags[] = {{"root", FLAG_LIST, &roots}};
    int rc = parse_flags(arena, &g, flags, countof(flags), args, &extra);
    if (rc) return rc;
    Config cfg;
    Err err;
    if (config_load(arena, g.config_path, &cfg, &err) != ERR_OK) return fail(err.msg);
    if (roots.count) {
        cfg.roots = (StringList){0};
        String home = env_home(arena);
        for (size_t i = 0; i < roots.count; i++) strlist_push(arena, &cfg.roots, path_expand_home(arena, roots.items[i], home));
    }
    Snapshot *s;
    if (build_index(&cfg, g.index_path, &s, &err) != ERR_OK) return fail(err.msg);
    snapshot_release(s);
    return EXIT_SUCCESS;
}

static int cmd_daemon(Arena *arena, StringList args, bool serve) {
    Globals g = globals_default(arena);
    int64_t interval_ms = 10000;
    const Flag flags[] = {{"save-interval", FLAG_DURATION, &interval_ms}};
    StringList extra = {0};
    int rc = parse_flags(arena, &g, flags, countof(flags), args, &extra);
    if (rc) return rc;
    DaemonOptions o = {.config_path = g.config_path,
                       .index_path = g.index_path,
                       .socket_path = g.socket_path,
                       .serve = serve,
                       .save_interval_ms = interval_ms};
    Err err;
    return daemon_run(&o, &err) != ERR_OK ? fail(err.msg) : EXIT_SUCCESS;
}

/* eind_service runs `eind serve` from executable; launchd keeps its output in a log file, systemd in its journal. */
static Service eind_service(Arena *arena, const ServiceManager *manager, String executable) {
    Service service = {.name = S(SERVICE_NAME), .description = S("eind file index daemon")};
    strlist_push(arena, &service.argv, executable);
    strlist_push(arena, &service.argv, S("serve"));
    if (manager->kind == SERVICE_LAUNCHD) service.log_path = path_join(arena, env_home(arena), S("Library/Logs/eind.log"));
    return service;
}

static int cmd_service(Arena *arena, StringList args) {
    bool enable = args.count == 1 && str_equal(args.items[0], S("enable"));
    bool disable = args.count == 1 && str_equal(args.items[0], S("disable"));
    if (!enable && !disable) return usage_fail("usage: eind service enable|disable");
    Err err;
    ServiceManager manager;
    if (service_manager(arena, &manager, &err) != ERR_OK) return fail(err.msg);
    String definition = service_path(arena, &manager, S(SERVICE_NAME));
    if (disable) {
        if (service_disable(arena, &manager, S(SERVICE_NAME), &err) != ERR_OK) return fail(err.msg);
        printf("stopped eind serve and removed %.*s\n", (int)definition.len, definition.data);
        return EXIT_SUCCESS;
    }
    String executable;
    if (service_executable_path(arena, S(SERVICE_NAME), &executable, &err) != ERR_OK) return fail(err.msg);
    Service service = eind_service(arena, &manager, executable);
    if (service_enable(arena, &manager, &service, &err) != ERR_OK) return fail(err.msg);
    printf("eind serve now runs at login; definition at %.*s\n", (int)definition.len, definition.data);
    return EXIT_SUCCESS;
}

static void print_service_status(Arena *arena) {
    ServiceManager manager;
    if (service_manager(arena, &manager, nullptr) == ERR_OK && service_installed(arena, &manager, S(SERVICE_NAME))) {
        String definition = service_path(arena, &manager, S(SERVICE_NAME));
        printf("service: enabled, %.*s\n", (int)definition.len, definition.data);
    } else {
        printf("service: not enabled (run `eind service enable` to start eind serve at login)\n");
    }
}

static int cmd_status(Arena *arena, StringList args) {
    Globals g = globals_default(arena);
    StringList extra = {0};
    int rc = parse_flags(arena, &g, nullptr, 0, args, &extra);
    if (rc) return rc;
    printf("config: %.*s%s\n", (int)g.config_path.len, g.config_path.data,
           file_exists(g.config_path) ? "" : " (not present, using defaults)");
    if (server_running(g.socket_path)) {
        printf("daemon: running at %.*s\n", (int)g.socket_path.len, g.socket_path.data);
    } else {
        printf("daemon: not running (start with `eind serve`, socket %.*s)\n", (int)g.socket_path.len, g.socket_path.data);
    }
    print_service_status(arena);
    printf("index:  %.*s", (int)g.index_path.len, g.index_path.data);
    FileInfo info;
    if (file_info_follow(g.index_path, &info, nullptr) != ERR_OK || !info.exists) {
        printf(" (not built yet; run `eind index`)\n");
        return EXIT_SUCCESS;
    }
    String size = fmt_bytes(arena, info.size);
    printf(" (%.*s)\n", (int)size.len, size.data);
    int64_t start = clock_monotonic_ns();
    Err err;
    Snapshot *s;
    if (index_load(g.index_path, &s, &err) != ERR_OK) return fail(err.msg);
    int64_t files, dirs;
    snap_stats(s, &files, &dirs);
    String took = format_elapsed(arena, clock_monotonic_ns() - start);
    char built[32];
    time_t when = (time_t)s->built_at;
    struct tm tm;
    localtime_r(&when, &tm);
    strftime(built, sizeof built, "%Y-%m-%d %H:%M:%S", &tm);
    String files_text = fmt_thousands(arena, files), dirs_text = fmt_thousands(arena, dirs);
    printf("built:  %s\n", built);
    printf("loaded: %.*s files, %.*s folders in %.*s\n", (int)files_text.len, files_text.data, (int)dirs_text.len,
           dirs_text.data, (int)took.len, took.data);
    for (size_t i = 0; i < s->roots.count; i++) printf("root:   %.*s\n", (int)s->roots.items[i].len, s->roots.items[i].data);
    snapshot_release(s);
    return EXIT_SUCCESS;
}

/* run_editor runs $VISUAL or $EDITOR through the shell, so values with arguments such as "code --wait" work. */
[[nodiscard]] static Error run_editor(Arena *arena, String path, Err *err) {
    String editor;
    if (!env_get(arena, S("VISUAL"), &editor) || editor.len == 0) {
        if (!env_get(arena, S("EDITOR"), &editor) || editor.len == 0) editor = S("vi");
    }
    StringList argv = {0};
    strlist_push(arena, &argv, S("/bin/sh"));
    strlist_push(arena, &argv, S("-c"));
    strlist_push(arena, &argv, str_concat(arena, editor, S(" \"$1\"")));
    strlist_push(arena, &argv, S("sh"));
    strlist_push(arena, &argv, path);
    int exit_code;
    Error e = process_run_interactive(arena, argv, &exit_code, err);
    if (e == ERR_OK && exit_code != 0) e = err_set(err, ERR_IO, "editor \"%.*s\" failed", (int)editor.len, editor.data);
    return e;
}

static int config_edit(Arena *arena, String path) {
    Err err;
    bool created;
    if (config_write_default(arena, path, &created, &err) != ERR_OK || run_editor(arena, path, &err) != ERR_OK)
        return fail(err.msg);
    Config cfg;
    if (config_load(arena, path, &cfg, &err) != ERR_OK) {
        fprintf(stderr, "eind: %s\nrun `eind config edit` again to fix it\n", err.msg);
        return EXIT_FAILURE;
    }
    printf("config is valid; run `eind index` to apply it\n");
    return EXIT_SUCCESS;
}

static int cmd_config(Arena *arena, StringList args) {
    Globals g = globals_default(arena);
    bool initialize = false;
    const Flag flags[] = {{"init", FLAG_BOOL, &initialize}};
    StringList rest = {0};
    int rc = parse_flags(arena, &g, flags, countof(flags), args, &rest);
    if (rc) return rc;
    if (rest.count == 1 && str_equal(rest.items[0], S("edit"))) return config_edit(arena, g.config_path);
    if (rest.count > 0) {
        String action = rest.items[0];
        fprintf(stderr, "eind: unknown config action \"%.*s\" (want edit)\n", (int)action.len, action.data);
        return EXIT_USAGE;
    }
    Err err;
    if (initialize) {
        bool created;
        if (config_write_default(arena, g.config_path, &created, &err) != ERR_OK) return fail(err.msg);
        printf(created ? "wrote %.*s\n" : "%.*s already exists\n", (int)g.config_path.len, g.config_path.data);
        return EXIT_SUCCESS;
    }
    Config cfg;
    if (config_load(arena, g.config_path, &cfg, &err) != ERR_OK) return fail(err.msg);
    StringBuilder text = str_builder_create(arena, 1024);
    config_render(&cfg, &text);
    printf("# %.*s\n%.*s", (int)g.config_path.len, g.config_path.data, (int)text.len, text.data);
    return EXIT_SUCCESS;
}

static StringList concat_lists(Arena *arena, StringList a, StringList b, size_t b_from) {
    StringList all = strlist_copy(arena, a);
    for (size_t i = b_from; i < b.count; i++) strlist_push(arena, &all, b.items[i]);
    return all;
}

static int run(Arena *arena, int argc, char **argv) {
    StringList positional = {0}, flags = {0};
    bool literal;
    split_args(arena, argc, argv, &positional, &flags, &literal);
    String cmd = positional.count && !literal ? positional.items[0] : S("");
    StringList rest = concat_lists(arena, (StringList){0}, positional, 1);
    StringList with_rest = concat_lists(arena, flags, positional, 1);
    if (str_equal(cmd, S("index"))) return cmd_index(arena, with_rest);
    if (str_equal(cmd, S("config"))) return cmd_config(arena, with_rest);
    if (str_equal(cmd, S("watch"))) return cmd_daemon(arena, with_rest, false);
    if (str_equal(cmd, S("serve"))) return cmd_daemon(arena, with_rest, true);
    if (str_equal(cmd, S("status"))) return cmd_status(arena, flags);
    if (str_equal(cmd, S("service"))) return cmd_service(arena, rest);
    if (str_equal(cmd, S("tui"))) return cmd_search(arena, flags, (StringList){0}, true);
    if (str_equal(cmd, S("search"))) return cmd_search(arena, flags, rest, false);
    if (str_equal(cmd, S("version"))) {
        printf("eind %s\n", EIND_VERSION);
        return EXIT_SUCCESS;
    }
    if (str_equal(cmd, S("help"))) {
        print_usage(arena, stdout);
        return EXIT_SUCCESS;
    }
    return cmd_search(arena, flags, positional, false);
}

int main(int argc, char **argv) {
    Arena *arena = arena_create(0);
    int rc = run(arena, argc - 1, argv + 1);
    arena_destroy(arena);
    return rc;
}

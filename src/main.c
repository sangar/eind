/* eind is an instant file search for the command line. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app/build.h"
#include "app/config.h"
#include "app/daemon.h"
#include "app/output.h"
#include "app/server.h"
#include "app/service.h"
#include "app/tui.h"
#include "core/arena.h"
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

static void print_usage(FILE *out) {
    char *cfg = config_path(), *ix = index_path(), *sock = default_socket_path();
    fprintf(out, usage_text, cfg, ix, sock);
    free(cfg);
    free(ix);
    free(sock);
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

typedef struct {
    const char *name;
    FlagType type;
    void *dst;
} Flag;

/* parse_duration accepts Go-style durations such as 10s, 500ms, 1m30s. */
static bool parse_duration(const char *s, int *ms) {
    double total = 0;
    const char *p = s;
    if (!*p) return false;
    while (*p) {
        char *end;
        double v = strtod(p, &end);
        if (end == p) return false;
        p = end;
        double unit;
        if (!strncmp(p, "ms", 2)) {
            unit = 1, p += 2;
        } else if (*p == 's') {
            unit = 1000, p++;
        } else if (*p == 'm') {
            unit = 60000, p++;
        } else if (*p == 'h') {
            unit = 3600000, p++;
        } else {
            return false;
        }
        total += v * unit;
    }
    *ms = (int)total;
    return total > 0;
}

static const Flag *find_flag(const Flag *flags, size_t count, const char *name, size_t len) {
    for (size_t i = 0; i < count; i++)
        if (strlen(flags[i].name) == len && strncmp(flags[i].name, name, len) == 0) return &flags[i];
    return NULL;
}

static bool set_flag(const Flag *f, const char *value, bool has_value, char *err, size_t errlen) {
    switch (f->type) {
    case FLAG_BOOL:
        if (!has_value || !strcmp(value, "true") || !strcmp(value, "1")) {
            *(bool *)f->dst = true;
        } else if (!strcmp(value, "false") || !strcmp(value, "0")) {
            *(bool *)f->dst = false;
        } else {
            snprintf(err, errlen, "invalid boolean value \"%s\" for -%s", value, f->name);
            return false;
        }
        return true;
    case FLAG_INT: {
        int64_t v;
        if (!parse_int64(value, &v)) {
            snprintf(err, errlen, "invalid value \"%s\" for flag -%s: parse error", value, f->name);
            return false;
        }
        *(long *)f->dst = (long)v;
        return true;
    }
    case FLAG_STRING: *(const char **)f->dst = value; return true;
    case FLAG_LIST: strlist_push((StrList *)f->dst, value); return true;
    case FLAG_DURATION:
        if (!parse_duration(value, (int *)f->dst)) {
            snprintf(err, errlen, "invalid value \"%s\" for flag -%s: parse error", value, f->name);
            return false;
        }
        return true;
    }
    return false;
}

/*
 * parse_flags reads flags the way Go's flag package does: -name, --name,
 * -name=value, or -name value for flags that take one; it stops at the first
 * argument that is not a flag and collects the rest as positional.
 * It returns 0 on success, or the exit status to stop with.
 */
static int parse_flags(const Flag *flags, size_t count, int argc, char **argv, StrList *positional) {
    int i = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        const char *name = a + (a[1] == '-' ? 2 : 1);
        const char *eq = strchr(name, '=');
        size_t len = eq ? (size_t)(eq - name) : strlen(name);
        if ((len == 1 && name[0] == 'h') || (len == 4 && !strncmp(name, "help", 4))) {
            print_usage(stdout);
            exit(EXIT_SUCCESS);
        }
        const Flag *f = find_flag(flags, count, name, len);
        char err[300];
        if (!f) {
            snprintf(err, sizeof err, "flag provided but not defined: -%.*s (see eind --help)", (int)len, name);
            return usage_fail(err);
        }
        const char *value = eq ? eq + 1 : NULL;
        if (!value && f->type != FLAG_BOOL) {
            if (i + 1 >= argc) {
                snprintf(err, sizeof err, "flag needs an argument: -%s (see eind --help)", f->name);
                return usage_fail(err);
            }
            value = argv[++i];
        }
        if (!set_flag(f, value ? value : "", value != NULL, err, sizeof err)) {
            strncat(err, " (see eind --help)", sizeof err - strlen(err) - 1);
            return usage_fail(err);
        }
    }
    for (; i < argc; i++) strlist_push(positional, argv[i]);
    return 0;
}

/* Flags that take a value, so the pre-pass keeps the next argument with them. */
static bool takes_value(const char *name) {
    static const char *names[] = {"n", "max-results", "o", "offset", "s", "sort", "path", "color",
                                  "config", "index", "root", "save-interval", "socket"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++)
        if (!strcmp(names[i], name)) return true;
    return false;
}

static bool is_number(const char *s) {
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

/*
 * split_args lets options appear anywhere on the line, while flag parsing
 * reads them only before the first word. Everything after "--" is query
 * text; *literal says the first word came after "--", so it is never taken
 * as a subcommand.
 */
static void split_args(int argc, char **argv, StrList *positional, StrList *flags, bool *literal) {
    *literal = false;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            *literal = positional->len == 0;
            for (i++; i < argc; i++) strlist_push(positional, argv[i]);
            break;
        }
        if (strlen(a) < 2 || a[0] != '-' || has_prefix(a, "-.") || (is_number(a + 1) && strcmp(a, "-0") != 0)) {
            strlist_push(positional, a);
            continue;
        }
        strlist_push(flags, a);
        const char *name = a;
        while (*name == '-') name++;
        if (!strchr(name, '=') && takes_value(name) && i + 1 < argc) strlist_push(flags, argv[++i]);
    }
}

typedef struct {
    const char *config_path, *index_path, *socket_path;
    char *owned[3];
} Globals;

static void globals_init(Globals *g) {
    g->owned[0] = config_path();
    g->owned[1] = index_path();
    g->owned[2] = default_socket_path();
    g->config_path = g->owned[0];
    g->index_path = g->owned[1];
    g->socket_path = g->owned[2];
}

static void globals_free(Globals *g) {
    for (size_t i = 0; i < ARRAY_LEN(g->owned); i++) free(g->owned[i]);
}

#define GLOBAL_FLAGS(g)                                                                                   \
    {"config", FLAG_STRING, &(g).config_path}, {"index", FLAG_STRING, &(g).index_path},                \
        {"socket", FLAG_STRING, &(g).socket_path}

/* ---- search ---- */

typedef struct {
    bool regex, case_sensitive, whole_word, match_path;
    long max_results, offset;
    const char *sort;
    bool descending;
    const char *path;
    bool files_only, dirs_only;
    bool json, csv, null_sep, name_only;
    bool show_size, show_modified, show_created;
    bool count;
    const char *color;
    bool show_version;
} SearchFlags;

static bool use_color(const char *mode) {
    if (!strcmp(mode, "always")) return true;
    if (!strcmp(mode, "never")) return false;
    return isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
}

static OutputOptions output_options(const SearchFlags *f) {
    OutputOptions o = {.null_sep = f->null_sep,
                       .name_only = f->name_only,
                       .show_size = f->show_size,
                       .show_modified = f->show_modified,
                       .show_created = f->show_created,
                       .color = use_color(f->color)};
    if (f->json) o.format = FORMAT_JSON;
    else if (f->csv) o.format = FORMAT_CSV;
    return o;
}

static char *join_terms(const StrList *terms) {
    StrBuf sb = {0};
    for (size_t i = 0; i < terms->len; i++) {
        if (i) sb_putc(&sb, ' ');
        sb_puts(&sb, terms->items[i]);
    }
    sb_cstr(&sb);
    return sb.data;
}

static int cmd_search(StrList *flag_args, StrList *terms, bool force_tui) {
    Globals g;
    globals_init(&g);
    SearchFlags f = {.sort = "path", .color = "auto"};
    const Flag flags[] = {
        GLOBAL_FLAGS(g),
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
    StrList extra = {0};
    int rc = parse_flags(flags, ARRAY_LEN(flags), (int)flag_args->len, flag_args->items, &extra);
    strlist_free(&extra);
    if (rc) {
        globals_free(&g);
        return rc;
    }
    if (f.show_version) {
        printf("eind %s\n", EIND_VERSION);
        globals_free(&g);
        return 0;
    }
    Err err;
    SortKey sort;
    if (!sort_key_parse(f.sort, &sort, &err)) {
        globals_free(&g);
        return fail(err.msg);
    }
    char *query = join_terms(terms);
    bool want_tui = force_tui || (terms->len == 0 && isatty(STDOUT_FILENO) && !f.count && !f.json && !f.csv);
    rc = EXIT_SUCCESS;
    Snapshot *s = load_or_build(g.config_path, g.index_path, &err);
    if (!s) {
        free(query);
        globals_free(&g);
        return fail(err.msg);
    }
    QueryDefaults defaults = {.regex = f.regex, .case_sensitive = f.case_sensitive, .whole_word = f.whole_word,
                              .match_path = f.match_path};
    if (want_tui) {
        char *chosen;
        if (!tui_run(s, defaults, &chosen, &err)) {
            rc = fail(err.msg);
        } else if (chosen) {
            printf("%s\n", chosen);
            free(chosen);
        }
    } else {
        Arena arena;
        arena_init(&arena, 4096);
        QueryNode *node = query_parse(&arena, query, defaults, &err);
        U32Vec hits = {0};
        if (!node || search_run(s, query_restrict(&arena, node, f.path, f.files_only, f.dirs_only), NULL, &hits, &err) !=
                         SEARCH_OK) {
            rc = fail(err.msg);
        } else if (f.count) {
            printf("%zu\n", hits.len);
        } else {
            long keep = f.max_results > 0 ? f.offset + f.max_results : -1;
            size_t kept = sort == SORT_RELEVANCE ? search_rank(s, hits.data, hits.len, node, keep)
                                                 : search_top(s, hits.data, hits.len, sort, f.descending, keep);
            size_t from = MIN((size_t)MAX(f.offset, 0), kept);
            size_t count = kept - from;
            if (f.max_results > 0 && (size_t)f.max_results < count) count = (size_t)f.max_results;
            OutputOptions o = output_options(&f);
            if (!output_write_hits(stdout, s, hits.data + from, count, &o) && errno != EPIPE)
                rc = fail(strerror(errno));
        }
        u32vec_free(&hits);
        arena_free(&arena);
    }
    snapshot_release(s);
    free(query);
    globals_free(&g);
    return rc;
}

/* ---- other commands ---- */

static int cmd_index(StrList *args) {
    Globals g;
    globals_init(&g);
    StrList roots = {0};
    const Flag flags[] = {GLOBAL_FLAGS(g), {"root", FLAG_LIST, &roots}};
    StrList extra = {0};
    int rc = parse_flags(flags, ARRAY_LEN(flags), (int)args->len, args->items, &extra);
    Config cfg;
    Err err;
    if (!rc && !config_load(g.config_path, &cfg, &err)) rc = fail(err.msg);
    if (!rc) {
        if (roots.len) {
            strlist_free(&cfg.roots);
            for (size_t i = 0; i < roots.len; i++) strlist_push_owned(&cfg.roots, expand_home(roots.items[i]));
        }
        Snapshot *s = build_index(&cfg, g.index_path, &err);
        if (s) snapshot_release(s);
        else rc = fail(err.msg);
        config_free(&cfg);
    }
    strlist_free(&roots);
    strlist_free(&extra);
    globals_free(&g);
    return rc;
}

static int cmd_daemon(StrList *args, bool serve) {
    Globals g;
    globals_init(&g);
    int interval = 10000;
    const Flag flags[] = {GLOBAL_FLAGS(g), {"save-interval", FLAG_DURATION, &interval}};
    StrList extra = {0};
    int rc = parse_flags(flags, ARRAY_LEN(flags), (int)args->len, args->items, &extra);
    if (!rc) {
        DaemonOptions o = {.config_path = g.config_path,
                           .index_path = g.index_path,
                           .socket_path = g.socket_path,
                           .serve = serve,
                           .save_interval_ms = interval};
        Err err;
        if (!daemon_run(&o, &err)) rc = fail(err.msg);
    }
    strlist_free(&extra);
    globals_free(&g);
    return rc;
}

static int cmd_service(StrList *args) {
    if (args->len != 1 || (strcmp(args->items[0], "enable") && strcmp(args->items[0], "disable")))
        return usage_fail("usage: eind service enable|disable");
    Err err;
    char *unit = service_unit_path(&err);
    if (!unit) return fail(err.msg);
    int rc = EXIT_SUCCESS;
    if (!strcmp(args->items[0], "disable")) {
        if (service_disable(&err)) printf("stopped eind serve and removed %s\n", unit);
        else rc = fail(err.msg);
    } else {
        char *exe = service_executable_path(&err);
        if (!exe || !service_enable(exe, &err)) rc = fail(err.msg);
        else printf("eind serve now runs at login; definition at %s\n", unit);
        free(exe);
    }
    free(unit);
    return rc;
}

static int cmd_status(StrList *args) {
    Globals g;
    globals_init(&g);
    const Flag flags[] = {GLOBAL_FLAGS(g)};
    StrList extra = {0};
    int rc = parse_flags(flags, ARRAY_LEN(flags), (int)args->len, args->items, &extra);
    strlist_free(&extra);
    if (rc) {
        globals_free(&g);
        return rc;
    }
    struct stat st;
    printf("config: %s%s\n", g.config_path, stat(g.config_path, &st) ? " (not present, using defaults)" : "");
    if (server_running(g.socket_path)) {
        printf("daemon: running at %s\n", g.socket_path);
    } else {
        printf("daemon: not running (start with `eind serve`, socket %s)\n", g.socket_path);
    }
    Err err;
    char *unit = service_unit_path(&err);
    if (unit && service_installed()) {
        printf("service: enabled, %s\n", unit);
    } else {
        printf("service: not enabled (run `eind service enable` to start eind serve at login)\n");
    }
    free(unit);
    printf("index:  %s", g.index_path);
    if (stat(g.index_path, &st) != 0) {
        printf(" (not built yet; run `eind index`)\n");
        globals_free(&g);
        return 0;
    }
    char size[32], files_s[32], dirs_s[32], took[32], built[32];
    printf(" (%s)\n", human_size(st.st_size, size));
    int64_t start = monotonic_us();
    bool missing;
    Snapshot *s = index_load(g.index_path, &missing, &err);
    if (!s) {
        globals_free(&g);
        return fail(err.msg);
    }
    int64_t files, dirs;
    snap_stats(s, &files, &dirs);
    time_t when = (time_t)s->built_at;
    struct tm tm;
    localtime_r(&when, &tm);
    strftime(built, sizeof built, "%Y-%m-%d %H:%M:%S", &tm);
    printf("built:  %s\n", built);
    printf("loaded: %s files, %s folders in %s\n", commas(files, files_s), commas(dirs, dirs_s),
           format_duration(elapsed_ms_since(start), took));
    for (size_t i = 0; i < s->roots.len; i++) printf("root:   %s\n", s->roots.items[i]);
    snapshot_release(s);
    globals_free(&g);
    return 0;
}

/* run_editor runs $VISUAL or $EDITOR through the shell, so values with arguments such as "code --wait" work. */
static bool run_editor(const char *path, Err *err) {
    const char *editor = getenv("VISUAL");
    if (!editor || !*editor) editor = getenv("EDITOR");
    if (!editor || !*editor) editor = "vi";
    pid_t pid = fork();
    if (pid == 0) {
        StrBuf cmd = {0};
        sb_printf(&cmd, "%s \"$1\"", editor);
        execl("/bin/sh", "sh", "-c", cmd.data, "sh", path, (char *)NULL);
        _exit(127);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err_set(err, "editor \"%s\" failed", editor);
        return false;
    }
    return true;
}

static int cmd_config(StrList *args) {
    Globals g;
    globals_init(&g);
    bool initialize = false;
    const Flag flags[] = {GLOBAL_FLAGS(g), {"init", FLAG_BOOL, &initialize}};
    StrList rest = {0};
    int rc = parse_flags(flags, ARRAY_LEN(flags), (int)args->len, args->items, &rest);
    Err err;
    bool created;
    if (rc) {
        /* already reported */
    } else if (rest.len == 1 && !strcmp(rest.items[0], "edit")) {
        if (!config_write_default(g.config_path, &created, &err) || !run_editor(g.config_path, &err)) {
            rc = fail(err.msg);
        } else {
            Config cfg;
            if (config_load(g.config_path, &cfg, &err)) {
                config_free(&cfg);
                printf("config is valid; run `eind index` to apply it\n");
            } else {
                fprintf(stderr, "eind: %s\nrun `eind config edit` again to fix it\n", err.msg);
                rc = EXIT_FAILURE;
            }
        }
    } else if (rest.len > 0) {
        char msg[300];
        snprintf(msg, sizeof msg, "unknown config action \"%s\" (want edit)", rest.items[0]);
        rc = usage_fail(msg);
    } else if (initialize) {
        if (!config_write_default(g.config_path, &created, &err)) rc = fail(err.msg);
        else if (created) printf("wrote %s\n", g.config_path);
        else printf("%s already exists\n", g.config_path);
    } else {
        Config cfg;
        if (!config_load(g.config_path, &cfg, &err)) {
            rc = fail(err.msg);
        } else {
            StrBuf text = {0};
            config_render(&cfg, &text);
            printf("# %s\n%s", g.config_path, sb_cstr(&text));
            sb_free(&text);
            config_free(&cfg);
        }
    }
    strlist_free(&rest);
    globals_free(&g);
    return rc;
}

static void append_all(StrList *dst, const StrList *src, size_t from) {
    for (size_t i = from; i < src->len; i++) strlist_push(dst, src->items[i]);
}

static int run(int argc, char **argv) {
    StrList positional = {0}, flags = {0}, args = {0};
    bool literal;
    split_args(argc, argv, &positional, &flags, &literal);
    const char *cmd = positional.len && !literal ? positional.items[0] : "";
    int rc;
    append_all(&args, &flags, 0);
    if (!strcmp(cmd, "index") || !strcmp(cmd, "watch") || !strcmp(cmd, "serve") || !strcmp(cmd, "config")) {
        append_all(&args, &positional, 1);
        rc = cmd[0] == 'i' ? cmd_index(&args) : cmd[0] == 'c' ? cmd_config(&args) : cmd_daemon(&args, cmd[1] == 'e');
    } else if (!strcmp(cmd, "status")) {
        rc = cmd_status(&args);
    } else if (!strcmp(cmd, "service")) {
        StrList rest = {0};
        append_all(&rest, &positional, 1);
        rc = cmd_service(&rest);
        strlist_free(&rest);
    } else if (!strcmp(cmd, "tui")) {
        StrList none = {0};
        rc = cmd_search(&args, &none, true);
    } else if (!strcmp(cmd, "search")) {
        StrList terms = {0};
        append_all(&terms, &positional, 1);
        rc = cmd_search(&args, &terms, false);
        strlist_free(&terms);
    } else if (!strcmp(cmd, "version")) {
        printf("eind %s\n", EIND_VERSION);
        rc = 0;
    } else if (!strcmp(cmd, "help")) {
        print_usage(stdout);
        rc = 0;
    } else {
        rc = cmd_search(&args, &positional, false);
    }
    strlist_free(&positional);
    strlist_free(&flags);
    strlist_free(&args);
    return rc;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    return run(argc - 1, argv + 1);
}

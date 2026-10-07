#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "app/build.h"
#include "app/config.h"
#include "app/server.h"
#include "core/arena.h"
#include "core/json.h"
#include "core/sort.h"
#include "core/threadpool.h"
#include "fs/fs.h"
#include "fs/updater.h"
#include "index/journal.h"
#include "index/query.h"
#include "index/search.h"

static int failures, checks; // the harness totals; modern-c: allow global-mutable

#define CHECK(cond) /* records the failing line and expression; modern-c: allow function-macro */ \
    do {                                                                              \
        checks++;                                                                     \
        if (!(cond)) {                                                                \
            failures++;                                                               \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__, __func__, #cond); \
        }                                                                             \
    } while (0)

#define CHECK_STR(got, want) /* modern-c: allow function-macro */ \
    do {                                                                                         \
        checks++;                                                                                \
        const char *g_ = (got), *w_ = (want);                                                    \
        if (strcmp(g_ ? g_ : "(null)", w_) != 0) {                                               \
            failures++;                                                                          \
            fprintf(stderr, "%s:%d: %s: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, __func__, g_, w_); \
        }                                                                                        \
    } while (0)

/* ---- helpers ---- */

/* describe renders a query tree compactly so tests can compare structure. */
static void describe(const QueryNode *n, StrBuf *sb) {
    static const char *modes[] = {"sub", "glob", "re", "ww", "wfn"};
    switch (n->kind) {
    case Q_AND:
    case Q_OR:
        sb_puts(sb, n->kind == Q_AND ? "and(" : "or(");
        for (uint32_t k = 0; k < n->kid_count; k++) {
            if (k) sb_putc(sb, ' ');
            describe(n->kids[k], sb);
        }
        sb_putc(sb, ')');
        break;
    case Q_NOT:
        sb_puts(sb, "not(");
        describe(n->kid, sb);
        sb_putc(sb, ')');
        break;
    case Q_TEXT:
        sb_printf(sb, "%s%s%s:%s", modes[n->mode], n->case_sensitive ? "+case" : "", n->match_path ? "+path" : "", n->text);
        break;
    case Q_EXT:
        sb_puts(sb, "ext");
        for (uint32_t k = 0; k < n->ext_count; k++) sb_printf(sb, "%c%s", k ? ',' : ':', n->exts[k]);
        break;
    case Q_SIZE: sb_printf(sb, "size[%lld,%lld]", (long long)n->range.lo, (long long)n->range.hi); break;
    case Q_NAMELEN: sb_printf(sb, "len[%lld,%lld]", (long long)n->range.lo, (long long)n->range.hi); break;
    case Q_DEPTH: sb_printf(sb, "depth[%lld,%lld]", (long long)n->range.lo, (long long)n->range.hi); break;
    case Q_MODIFIED: sb_puts(sb, "dm"); break;
    case Q_CREATED: sb_puts(sb, "dc"); break;
    case Q_ISDIR: sb_puts(sb, n->dir ? "dir" : "file"); break;
    case Q_PARENT: sb_printf(sb, "parent:%s", n->path); break;
    case Q_INFOLDER: sb_printf(sb, "infolder:%s", n->path); break;
    }
}

static const char *parsed(Arena *a, const char *query, QueryDefaults d) {
    Err err;
    QueryNode *n = query_parse(a, query, d, &err);
    if (!n) return arena_printf(a, "error: %s", err.msg);
    StrBuf sb = {0};
    describe(n, &sb);
    char *out = arena_strdup(a, sb_cstr(&sb));
    sb_free(&sb);
    return out;
}

static char scratch[1024]; // the fixture directory; modern-c: allow global-mutable

static void make_file(const char *rel, size_t size) {
    char *path = path_join(scratch, rel);
    char *dir = path_dir(path);
    mkdir_p(dir, 0755, NULL);
    FILE *f = fopen(path, "w");
    for (size_t i = 0; i < size; i++) fputc('x', f);
    fclose(f);
    free(dir);
    free(path);
}

static void make_dir(const char *rel) {
    char *path = path_join(scratch, rel);
    mkdir_p(path, 0755, NULL);
    free(path);
}

static void remove_tree(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", path);
}

/* search_names runs a query and returns the matching base names, sorted by path and joined with spaces. */
static const char *search_names(Arena *a, ThreadPool *cpu, const Snapshot *s, const char *query) {
    Err err;
    QueryNode *n = query_parse(a, query, (QueryDefaults){0}, &err);
    if (!n) return arena_printf(a, "error: %s", err.msg);
    U32Vec hits = {0};
    if (search_run(cpu, s, n, NULL, &hits, &err) != SEARCH_OK) return arena_printf(a, "error: %s", err.msg);
    search_top(s, hits.data, hits.len, SORT_PATH, false, -1);
    StrBuf sb = {0};
    for (size_t i = 0; i < hits.len; i++) sb_printf(&sb, "%s%s", i ? " " : "", path_base(snap_name(s, hits.data[i])));
    u32vec_free(&hits);
    char *out = arena_strdup(a, sb_cstr(&sb));
    sb_free(&sb);
    return out;
}

static Snapshot *build_fixture(const char *index_file) {
    remove_tree(scratch);
    make_file("root/src/main.go", 10);
    make_file("root/src/main_test.go", 2000);
    make_file("root/src/report_2024.PDF", 50000);
    make_file("root/docs/Readme.md", 5);
    make_file("root/docs/IMG_0001.JPG", 1);
    make_file("root/node_modules/dep/index.js", 1);
    make_dir("root/empty");
    Config cfg = {0};
    char *root = path_join(scratch, "root");
    strlist_push_owned(&cfg.roots, root);
    strlist_push(&cfg.excludes, "node_modules");
    Err err;
    Snapshot *s = build_index(&cfg, index_file, &err);
    if (!s) fprintf(stderr, "build_index: %s\n", err.msg);
    config_free(&cfg);
    return s;
}

/* ---- tests ---- */

static void test_query_parse(void) {
    Arena a;
    arena_init(&a, 4096);
    QueryDefaults d = {.now = 1710342000};
    CHECK_STR(parsed(&a, "", d), "and()");
    CHECK_STR(parsed(&a, "report", d), "sub:report");
    CHECK_STR(parsed(&a, "foo bar", d), "and(sub:foo sub:bar)");
    CHECK_STR(parsed(&a, "a b | c", d), "or(and(sub:a sub:b) sub:c)");
    CHECK_STR(parsed(&a, "!draft", d), "not(sub:draft)");
    CHECK_STR(parsed(&a, "<a|b> c", d), "and(or(sub:a sub:b) sub:c)");
    CHECK_STR(parsed(&a, "path:\"my dir\"", d), "sub+path:my dir");
    CHECK_STR(parsed(&a, "*.txt", d), "glob:*.txt");
    CHECK_STR(parsed(&a, "regex:^img_\\d+", d), "re:^img_\\d+");
    CHECK_STR(parsed(&a, "folder:case:src", d), "and(dir sub+case:src)");
    CHECK_STR(parsed(&a, "file:", d), "file");
    CHECK_STR(parsed(&a, "ext:pdf;.DOCX", d), "ext:pdf,docx");
    CHECK_STR(parsed(&a, "size:>1kb", d), "size[1025,9223372036854775807]");
    CHECK_STR(parsed(&a, "size:1kb..2kb", d), "size[1024,2048]");
    CHECK_STR(parsed(&a, "size:<1kb", d), "size[-9223372036854775808,1023]");
    CHECK_STR(parsed(&a, "len:>40", d), "len[41,9223372036854775807]");
    CHECK_STR(parsed(&a, "depth:3", d), "depth[3,3]");
    CHECK_STR(parsed(&a, "infolder:/tmp", d), "infolder:/tmp");
    CHECK_STR(parsed(&a, "c:notafunction", d), "sub:c:notafunction");
    CHECK_STR(parsed(&a, "size:big", d), "error: size: expected a size such as 10mb, got \"big\"");
    CHECK_STR(parsed(&a, "dm:someday", d), "error: dm: unknown date \"someday\"");
    CHECK_STR(parsed(&a, "len:x", d), "error: len: expected a number, got \"x\"");
    QueryDefaults all = {.regex = true, .case_sensitive = true, .match_path = true, .now = d.now};
    CHECK_STR(parsed(&a, "foo ext:txt", all), "and(re+case+path:foo ext:txt)");
    arena_free(&a);
}

static void test_values(void) {
    Range r;
    Err err;
    CHECK(parse_size_value("1.5kb", 0, &r, &err) && r.lo == 1536 && r.hi == 1536);
    CHECK(parse_size_value("2 MB", 0, &r, &err) && r.lo == 2 << 20);
    CHECK(parse_size_value("gigantic", 0, &r, &err) && r.lo == (128 << 20) + 1 && r.hi == INT64_MAX);
    CHECK(!parse_size_value("3 parsecs", 0, &r, &err));

    setenv("TZ", "UTC", 1);
    tzset();
    time_t now = 1710342000; /* Wednesday 2024-03-13 15:00 UTC */
    struct {
        const char *in;
        int64_t lo, hi;
    } cases[] = {
        {"today", 1710288000, 1710374399},     {"yesterday", 1710201600, 1710287999},
        {"thisweek", 1710115200, 1710719999},  {"lastweek", 1709510400, 1710115199},
        {"thismonth", 1709251200, 1711929599}, {"lastmonth", 1706745600, 1709251199},
        {"lastyear", 1672531200, 1704067199},  {"last7days", 1709737200, 1710342000},
        {"2023-11", 1698796800, 1701388799},   {"2023/11/05", 1699142400, 1699228799},
    };
    for (size_t i = 0; i < countof(cases); i++) {
        bool ok = parse_date_value(cases[i].in, now, &r, &err);
        if (!ok || r.lo != cases[i].lo || r.hi != cases[i].hi)
            fprintf(stderr, "date %s: got [%lld,%lld]\n", cases[i].in, (long long)r.lo, (long long)r.hi);
        CHECK(ok && r.lo == cases[i].lo && r.hi == cases[i].hi);
    }
    CHECK(parse_range(">2023", parse_date_value, now, &r, &err) && r.lo == 1704067200 && r.hi == INT64_MAX);
    unsetenv("TZ");
    tzset();
}

static void test_globs_and_excludes(void) {
    CHECK(glob_match("*.tmp", "a.tmp"));
    CHECK(!glob_match("*.tmp", "dir/a.tmp"));
    CHECK(glob_match("**/.git", "/home/me/x/.git"));
    CHECK(glob_match("/a/**/c", "/a/c"));
    CHECK(glob_match("/a/**/c", "/a/b/b/c"));
    CHECK(glob_match("IMG_[0-9]???.JPG", "IMG_0001.JPG"));
    CHECK(!glob_valid("[abc"));

    StrList patterns = {0};
    strlist_push(&patterns, "node_modules");
    strlist_push(&patterns, "/proc");
    strlist_push(&patterns, "/home/*/cache/");
    Excludes ex;
    Err err;
    CHECK(excludes_init(&ex, &patterns, &err));
    CHECK(excludes_match(&ex, "/x/node_modules", "node_modules"));
    CHECK(excludes_match(&ex, "/proc/1/status", "status"));
    CHECK(!excludes_match(&ex, "/processes", "processes"));
    CHECK(excludes_match(&ex, "/home/me/cache/a/b", "b"));
    CHECK(!excludes_match(&ex, "/home/me/caches", "caches"));
    excludes_free(&ex);
    strlist_free(&patterns);
}

static int compare_int(const void *ctx, const void *a, const void *b) {
    (void)ctx;
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void test_sort_top(void) {
    int v[1000];
    for (int i = 0; i < 1000; i++) v[i] = (i * 7919) % 1000;
    CHECK(sort_top(v, 1000, sizeof *v, 5, compare_int, NULL) == 5);
    CHECK(v[0] == 0 && v[1] == 1 && v[4] == 4);
    sort_stable(v, 1000, sizeof *v, compare_int, NULL);
    bool sorted = true;
    for (int i = 1; i < 1000; i++) sorted &= v[i - 1] <= v[i];
    CHECK(sorted);
}

static void test_json(void) {
    Arena a;
    arena_init(&a, 4096);
    Err err;
    const char *text = "{\"id\": [1, 2], \"query\": \"a\\\"b\\u00e9\", \"limit\": -1, \"case\": true}";
    JsonValue *v = json_parse(&a, text, strlen(text), &err);
    CHECK(v && v->type == JSON_OBJECT);
    CHECK_STR(json_string(v, "query", ""), "a\"b\xc3\xa9");
    CHECK(json_number(v, "limit", 0) == -1);
    CHECK(json_bool(v, "case"));
    const JsonValue *id = json_get(v, "id");
    CHECK(id && id->raw_len == 6 && memcmp(id->raw, "[1, 2]", 6) == 0);
    CHECK(json_parse(&a, "{\"a\":", 5, &err) == NULL);
    arena_free(&a);
}

static void test_index_search(void) {
    char *index_file = path_join(scratch, "../index.bin");
    Snapshot *s = build_fixture(index_file);
    CHECK(s != NULL);
    if (!s) return;
    Arena a;
    arena_init(&a, 4096);
    ThreadPool *cpu = threadpool_create(cpu_count());
    CHECK_STR(search_names(&a, cpu,s, "main"), "main.go main_test.go");
    CHECK_STR(search_names(&a, cpu,s, "ww:main"), "main.go main_test.go");
    CHECK_STR(search_names(&a, cpu,s, "file: ww:test"), "main_test.go");
    CHECK_STR(search_names(&a, cpu,s, "*.go"), "main.go main_test.go");
    CHECK_STR(search_names(&a, cpu,s, "*_????.JPG"), "IMG_0001.JPG");
    CHECK_STR(search_names(&a, cpu,s, "ext:pdf;md"), "Readme.md report_2024.PDF");
    CHECK_STR(search_names(&a, cpu,s, "case:readme"), "");
    CHECK_STR(search_names(&a, cpu,s, "wfn:readme.md"), "Readme.md");
    CHECK_STR(search_names(&a, cpu,s, "regex:_\\d+\\.pdf$"), "report_2024.PDF");
    CHECK_STR(search_names(&a, cpu,s, "size:>1kb"), "main_test.go report_2024.PDF");
    CHECK_STR(search_names(&a, cpu,s, "folder:"), "root docs empty src");
    CHECK_STR(search_names(&a, cpu,s, "main !test"), "main.go");
    CHECK_STR(search_names(&a, cpu,s, "<readme|img> ext:md"), "Readme.md");
    CHECK_STR(search_names(&a, cpu,s, "node_modules"), "");
    CHECK_STR(search_names(&a, cpu,s, "nothing-like-this"), "");
    CHECK_STR(search_names(&a, cpu,s, "path:src/main"), "main.go main_test.go");
    CHECK_STR(search_names(&a, cpu,s, "len:<8"), "docs empty src main.go");

    char *docs = path_join(scratch, "root/docs");
    CHECK_STR(search_names(&a, cpu,s, arena_printf(&a, "infolder:%s", docs)), "IMG_0001.JPG Readme.md");
    CHECK_STR(search_names(&a, cpu,s, arena_printf(&a, "parent:%s file:", docs)), "IMG_0001.JPG Readme.md");

    /* ranking: an exact stem beats a prefix, which beats a word start */
    Err err;
    QueryNode *n = query_parse(&a, "main", (QueryDefaults){0}, &err);
    U32Vec hits = {0};
    search_run(cpu, s, n, NULL, &hits, &err);
    size_t kept = search_rank(s, hits.data, hits.len, n, 1);
    CHECK(kept == 1);
    CHECK_STR(snap_name(s, hits.data[0]), "main.go");
    u32vec_free(&hits);

    /* keeping a few of many hits by path places the same ones a full sort would */
    n = query_parse(&a, "file:", (QueryDefaults){0}, &err);
    search_run(cpu, s, n, NULL, &hits, &err);
    CHECK(search_top(s, hits.data, hits.len, SORT_PATH, false, 1) == 1);
    CHECK_STR(snap_name(s, hits.data[0]), "IMG_0001.JPG");
    search_run(cpu, s, n, NULL, &hits, &err);
    search_top(s, hits.data, hits.len, SORT_PATH, true, 1);
    CHECK_STR(snap_name(s, hits.data[0]), "report_2024.PDF");
    u32vec_free(&hits);

    /* the file on disk reloads to the same answers */
    bool missing;
    Snapshot *loaded = index_load(index_file, &missing, &err);
    CHECK(loaded && loaded->total == s->total);
    if (loaded) CHECK_STR(search_names(&a, cpu,loaded, "report"), "report_2024.PDF");
    snapshot_release(loaded);
    snapshot_release(s);
    threadpool_destroy(cpu);
    free(docs);
    free(index_file);
    arena_free(&a);
}

static void test_updater_and_compaction(void) {
    char *index_file = path_join(scratch, "../index.bin");
    Snapshot *s = build_fixture(index_file);
    CHECK(s != NULL);
    if (!s) return;
    Index ix;
    index_init(&ix, s);
    StrList patterns = {0};
    strlist_push(&patterns, "node_modules");
    Excludes ex;
    Err err;
    excludes_init(&ex, &patterns, &err);
    ThreadPool *cpu = threadpool_create(cpu_count()), *io = threadpool_create(io_thread_count());
    Updater *u = updater_new(io, &ix, &ex);
    Arena a;
    arena_init(&a, 4096);

    /* a held snapshot keeps answering while newer ones are published */
    Snapshot *before = index_acquire(&ix);

    make_file("root/new/deep/fresh.txt", 3);
    make_file("root/src/main.go", 99);
    char *docs = path_join(scratch, "root/docs");
    remove_tree(docs);
    StrList changed = {0}, new_dirs = {0};
    char *p1 = path_join(scratch, "root/new/deep/fresh.txt"), *p2 = path_join(scratch, "root/src/main.go");
    strlist_push(&changed, p1);
    strlist_push(&changed, p2);
    strlist_push(&changed, docs);
    CHECK(updater_apply(u, &changed, &new_dirs));
    CHECK(new_dirs.len == 2);

    Snapshot *after = index_acquire(&ix);
    CHECK_STR(search_names(&a, cpu,after, "fresh"), "fresh.txt");
    CHECK_STR(search_names(&a, cpu,after, "ext:md;jpg"), "");
    CHECK_STR(search_names(&a, cpu,after, "size:99"), "main.go");
    CHECK_STR(search_names(&a, cpu,after, "folder:"), "root empty new deep src");
    CHECK_STR(search_names(&a, cpu,before, "ext:md;jpg"), "IMG_0001.JPG Readme.md");

    /* reconciling an unchanged path changes nothing */
    strlist_clear(&changed);
    strlist_push(&changed, p2);
    CHECK(!updater_apply(u, &changed, NULL));

    /* compaction drops tombstones and keeps every live answer */
    Snapshot *merged = index_compact(after, index_file, &err);
    CHECK(merged && merged->seg_count == 1 && merged->dead_count == 0 && merged->total == snap_live_count(after));
    if (merged) {
        CHECK_STR(search_names(&a, cpu,merged, "fresh"), "fresh.txt");
        CHECK_STR(search_names(&a, cpu,merged, "folder:"), "root empty new deep src");
        snapshot_retain(merged);
        index_publish(&ix, merged);
        updater_reset(u, merged);
        char *gone = path_join(scratch, "root/new");
        remove_tree(gone);
        strlist_clear(&changed);
        strlist_push(&changed, gone);
        CHECK(updater_apply(u, &changed, NULL));
        Snapshot *latest = index_acquire(&ix);
        CHECK_STR(search_names(&a, cpu,latest, "fresh"), "");
        snapshot_release(latest);
        free(gone);
    }

    snapshot_release(before);
    snapshot_release(after);
    strlist_free(&changed);
    strlist_free(&new_dirs);
    free(p1);
    free(p2);
    free(docs);
    updater_free(u);
    threadpool_destroy(cpu);
    threadpool_destroy(io);
    index_destroy(&ix);
    excludes_free(&ex);
    strlist_free(&patterns);
    free(index_file);
    arena_free(&a);
}

/* A test client: one connection to the daemon socket. */
typedef struct {
    int fd;
    StrBuf buf;
} Client;

static bool client_dial(Client *c, const char *socket_path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path);
    *c = (Client){.fd = socket(AF_UNIX, SOCK_STREAM, 0)};
    return c->fd >= 0 && connect(c->fd, (struct sockaddr *)&addr, sizeof addr) == 0;
}

static void client_close(Client *c) {
    close(c->fd);
    sb_free(&c->buf);
}

/* client_request sends one JSON line and parses the one-line reply into the arena. */
static JsonValue *client_request(Client *c, const char *line, Arena *arena, Err *err) {
    if (write(c->fd, line, strlen(line)) < 0 || write(c->fd, "\n", 1) < 0) return NULL;
    char *nl;
    while (!(c->buf.len && (nl = memchr(c->buf.data, '\n', c->buf.len)))) {
        char chunk[4096];
        ssize_t n = read(c->fd, chunk, sizeof chunk);
        if (n <= 0) return NULL;
        sb_append(&c->buf, chunk, (size_t)n);
    }
    size_t len = (size_t)(nl - c->buf.data);
    JsonValue *v = json_parse(arena, c->buf.data, len, err);
    memmove(c->buf.data, nl + 1, c->buf.len - len - 1);
    c->buf.len -= len + 1;
    return v;
}

/* Changes journaled by a daemon are seen by every later load of the index file. */
static void test_journal(void) {
    char *index_file = path_join(scratch, "../index.bin");
    Snapshot *s = build_fixture(index_file);
    CHECK(s != NULL);
    if (!s) return;
    Index ix;
    index_init(&ix, s);
    Err err;
    Journal *j = journal_open(index_file, s, &err);
    CHECK(j != NULL);
    Excludes ex = {0};
    ThreadPool *cpu = threadpool_create(cpu_count()), *io = threadpool_create(io_thread_count());
    Updater *u = updater_new(io, &ix, &ex);
    Arena a;
    arena_init(&a, 4096);

    make_file("root/journaled.txt", 7);
    make_file("root/src/main.go", 42);
    char *docs = path_join(scratch, "root/docs");
    remove_tree(docs);
    StrList changed = {0};
    char *p1 = path_join(scratch, "root/journaled.txt"), *p2 = path_join(scratch, "root/src/main.go");
    strlist_push(&changed, p1);
    strlist_push(&changed, p2);
    strlist_push(&changed, docs);
    Snapshot *before = updater_snapshot(u);
    snapshot_retain(before);
    CHECK(updater_apply(u, &changed, NULL));
    journal_record(j, before, updater_snapshot(u));
    snapshot_release(before);
    CHECK(journal_flush(j, &err) == JOURNAL_OK);

    bool missing;
    Snapshot *loaded = index_load(index_file, &missing, &err);
    CHECK(loaded != NULL);
    if (loaded) {
        CHECK_STR(search_names(&a, cpu,loaded, "journaled"), "journaled.txt");
        CHECK_STR(search_names(&a, cpu,loaded, "size:42"), "main.go");
        CHECK_STR(search_names(&a, cpu,loaded, "ext:md;jpg"), "");
        CHECK(snap_live_count(loaded) == snap_live_count(updater_snapshot(u)));
        snapshot_release(loaded);
    }

    /* a half-written last entry is ignored, and the next writer cuts it off */
    char *jpath = path_join(scratch, "../index.bin.journal");
    FILE *f = fopen(jpath, "ab");
    fputc('A', f);
    fputc(3, f);
    fclose(f);
    loaded = index_load(index_file, &missing, &err);
    CHECK(loaded != NULL);
    if (loaded) {
        CHECK_STR(search_names(&a, cpu,loaded, "journaled"), "journaled.txt");
        Journal *again = journal_open(index_file, loaded, &err);
        CHECK(again && journal_entries(again) == journal_entries(j));
        journal_free(again);
        snapshot_release(loaded);
    }

    /* a new index file written by another process is noticed on the next flush */
    loaded = index_load(index_file, &missing, &err);
    Snapshot *rewritten = loaded ? index_compact(loaded, index_file, &err) : NULL;
    CHECK(rewritten != NULL);
    make_file("root/late.txt", 1);
    char *p3 = path_join(scratch, "root/late.txt");
    strlist_clear(&changed);
    strlist_push(&changed, p3);
    before = updater_snapshot(u);
    snapshot_retain(before);
    CHECK(updater_apply(u, &changed, NULL));
    journal_record(j, before, updater_snapshot(u));
    snapshot_release(before);
    CHECK(journal_flush(j, &err) == JOURNAL_REPLACED);

    if (rewritten) snapshot_release(rewritten);
    if (loaded) snapshot_release(loaded);
    strlist_free(&changed);
    free(p1);
    free(p2);
    free(p3);
    free(docs);
    free(jpath);
    journal_free(j);
    updater_free(u);
    threadpool_destroy(cpu);
    threadpool_destroy(io);
    index_destroy(&ix);
    arena_free(&a);
    free(index_file);
}

static void test_server(void) {
    char *index_file = path_join(scratch, "../index.bin");
    Snapshot *s = build_fixture(index_file);
    CHECK(s != NULL);
    if (!s) return;
    Index ix;
    index_init(&ix, s);
    char socket_path[64];
    snprintf(socket_path, sizeof socket_path, "/tmp/eind-test-%d.sock", (int)getpid());
    Err err;
    ThreadPool *cpu = threadpool_create(cpu_count());
    Server *srv = server_start(cpu, &ix, socket_path, index_file, &err);
    CHECK(srv != NULL);
    CHECK(server_running(socket_path));
    Client c;
    CHECK(client_dial(&c, socket_path));
    Arena a;
    arena_init(&a, 4096);
    JsonValue *resp = client_request(&c, "{\"id\":\"q1\",\"query\":\"main\",\"limit\":1}", &a, &err);
    CHECK(resp && !strcmp(json_string(resp, "id", ""), "q1"));
    CHECK(json_number(resp, "total", 0) == 2);
    const JsonValue *results = json_get(resp, "results");
    CHECK(results && results->count == 1);
    if (results && results->count) {
        CHECK_STR(json_string(results->items[0], "name", ""), "main.go");
        CHECK_STR(json_string(results->items[0], "type", ""), "file");
    }
    resp = client_request(&c, "{\"op\":\"status\"}", &a, &err);
    CHECK(resp && json_number(resp, "files", 0) == 5 && json_number(resp, "folders", 0) == 4);
    resp = client_request(&c, "{\"id\":2,\"query\":\"size:huge!\"}", &a, &err);
    CHECK(resp && strstr(json_string(resp, "error", ""), "size:"));
    resp = client_request(&c, "{\"query\":\"\",\"limit\":0}", &a, &err);
    CHECK(resp && json_number(resp, "total", 0) == 9 && json_get(resp, "results")->count == 0);
    client_close(&c);
    server_stop(srv);
    CHECK(!server_running(socket_path));
    threadpool_destroy(cpu);
    index_destroy(&ix);
    free(index_file);
    arena_free(&a);
}

static void test_config(void) {
    char *path = path_join(scratch, "../config");
    const char *text = "# comment\nroot = ~/x\nexclude = *.tmp\n";
    Err err;
    CHECK(write_file_atomic(path, text, strlen(text), 0644, &err));
    Config cfg;
    CHECK(config_load(path, &cfg, &err));
    char *expected = path_join(home_dir(), "x");
    CHECK(cfg.roots.len == 1 && !strcmp(cfg.roots.items[0], expected));
    CHECK(cfg.excludes.len == 1 && !strcmp(cfg.excludes.items[0], "*.tmp"));
    config_free(&cfg);
    const char *bad = "colour = blue\n";
    write_file_atomic(path, bad, strlen(bad), 0644, &err);
    CHECK(!config_load(path, &cfg, &err) && strstr(err.msg, "unknown key"));
    free(expected);
    free(path);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(scratch, sizeof scratch, "%s/eind-test-%d/tree", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    test_query_parse();
    test_values();
    test_globs_and_excludes();
    test_sort_top();
    test_json();
    test_index_search();
    test_updater_and_compaction();
    test_journal();
    test_server();
    test_config();
    char *top = path_dir(scratch);
    remove_tree(top);
    free(top);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

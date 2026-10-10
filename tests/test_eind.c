#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mc/encoding/json.h"
#include "mc/platform/platform.h"
#include "mc/text/glob.h"
#include "mc/text/path.h"
#include "app/build.h"
#include "app/config.h"
#include "app/server.h"
#include "fs/excludes.h"
#include "fs/updater.h"
#include "index/journal.h"
#include "index/query.h"
#include "index/search.h"

/* Test is the harness's tally, passed to every test. */
typedef struct {
    int checks, failures;
    String scratch; /* the fixture directory */
    Arena *arena;   /* lives as long as the run */
} Test;

/* check records one assertion; what describes it when it fails. */
static void check(Test *t, bool ok, const char *where, int line, const char *what) {
    t->checks++;
    if (ok) return;
    t->failures++;
    fprintf(stderr, "%s:%d: check failed: %s\n", where, line, what);
}

static void check_str(Test *t, String got, const char *want, const char *where, int line) {
    t->checks++;
    if (str_equal(got, S(want))) return;
    t->failures++;
    fprintf(stderr, "%s:%d: got \"%.*s\", want \"%s\"\n", where, line, (int)got.len, got.data, want);
}

/* CHECK and CHECK_STR record the failing function, line and expression, which only a macro can see. */
#define CHECK(t, cond) check((t), (cond), __func__, __LINE__, #cond) // modern-c: allow function-macro
#define CHECK_STR(t, got, want) check_str((t), (got), (want), __func__, __LINE__) // modern-c: allow function-macro

/* ---- helpers ---- */

/* describe renders a query tree compactly so tests can compare structure. */
static void describe(const QueryNode *n, StringBuilder *out) {
    static const char *const modes[] = {"sub", "glob", "re", "ww", "wfn"};
    switch (n->kind) {
    case Q_AND:
    case Q_OR:
        str_builder_append(out, n->kind == Q_AND ? S("and(") : S("or("));
        for (uint32_t k = 0; k < n->kid_count; k++) {
            if (k) str_builder_append_char(out, ' ');
            describe(n->kids[k], out);
        }
        str_builder_append_char(out, ')');
        break;
    case Q_NOT:
        str_builder_append(out, S("not("));
        describe(n->kid, out);
        str_builder_append_char(out, ')');
        break;
    case Q_TEXT:
        str_builder_append_format(out, "%s%s%s:%.*s", modes[n->mode], n->case_sensitive ? "+case" : "",
                                  n->match_path ? "+path" : "", (int)n->text.len, n->text.data);
        break;
    case Q_EXT:
        str_builder_append(out, S("ext"));
        for (uint32_t k = 0; k < n->ext_count; k++)
            str_builder_append_format(out, "%c%.*s", k ? ',' : ':', (int)n->exts[k].len, n->exts[k].data);
        break;
    case Q_SIZE: str_builder_append_format(out, "size[%lld,%lld]", (long long)n->range.lo, (long long)n->range.hi); break;
    case Q_NAMELEN: str_builder_append_format(out, "len[%lld,%lld]", (long long)n->range.lo, (long long)n->range.hi); break;
    case Q_DEPTH: str_builder_append_format(out, "depth[%lld,%lld]", (long long)n->range.lo, (long long)n->range.hi); break;
    case Q_MODIFIED: str_builder_append(out, S("dm")); break;
    case Q_CREATED: str_builder_append(out, S("dc")); break;
    case Q_ISDIR: str_builder_append(out, n->dir ? S("dir") : S("file")); break;
    case Q_PARENT: str_builder_append_format(out, "parent:%.*s", (int)n->path.len, n->path.data); break;
    case Q_INFOLDER: str_builder_append_format(out, "infolder:%.*s", (int)n->path.len, n->path.data); break;
    }
}

static String parsed(Arena *a, const char *query, QueryDefaults d) {
    Err err;
    QueryNode *n;
    if (query_parse(a, S(query), d, &n, &err) != ERR_OK) return str_format(a, "error: %s", err.msg);
    StringBuilder out = str_builder_create(a, 64);
    describe(n, &out);
    return str_builder_finish(&out);
}

static String fixture_path(Test *t, const char *rel) { return path_join(t->arena, t->scratch, S(rel)); }

static void make_file(Test *t, const char *rel, size_t size) {
    String path = fixture_path(t, rel);
    Error e = file_write_atomic(path, str_repeat(t->arena, S("x"), size), 0644, nullptr);
    CHECK(t, e == ERR_OK);
}

static void make_dir(Test *t, const char *rel) { CHECK(t, dir_create_all(fixture_path(t, rel), 0755, nullptr) == ERR_OK); }

static void remove_tree(Test *t, String path) { CHECK(t, dir_remove_all(path, nullptr) == ERR_OK); }

static IdList run_query(Arena *a, ThreadPool *cpu, const Snapshot *s, const char *query, QueryNode **parsed_query) {
    Err err;
    QueryNode *n;
    IdList hits = {0};
    if (query_parse(a, S(query), (QueryDefaults){0}, &n, &err) != ERR_OK) return hits;
    if (search_run(cpu, a, s, n, nullptr, &hits, &err) != ERR_OK) hits = (IdList){0};
    if (parsed_query) *parsed_query = n;
    return hits;
}

/* search_names runs a query and returns the matching base names, sorted by path and joined with spaces. */
static String search_names(Arena *a, ThreadPool *cpu, const Snapshot *s, const char *query) {
    Err err;
    QueryNode *n;
    if (query_parse(a, S(query), (QueryDefaults){0}, &n, &err) != ERR_OK) return str_format(a, "error: %s", err.msg);
    IdList hits;
    if (search_run(cpu, a, s, n, nullptr, &hits, &err) != ERR_OK) return str_format(a, "error: %s", err.msg);
    search_top(a, s, hits.items, hits.count, SORT_PATH, false, -1);
    StringBuilder out = str_builder_create(a, 64);
    for (size_t i = 0; i < hits.count; i++) {
        if (i) str_builder_append_char(&out, ' ');
        str_builder_append(&out, path_base(snap_name_view(s, hits.items[i])));
    }
    return str_builder_finish(&out);
}

static String index_file(Test *t) { return fixture_path(t, "../index.bin"); }

static Snapshot *build_fixture(Test *t) {
    remove_tree(t, t->scratch);
    make_file(t, "root/src/main.go", 10);
    make_file(t, "root/src/main_test.go", 2000);
    make_file(t, "root/src/report_2024.PDF", 50000);
    make_file(t, "root/docs/Readme.md", 5);
    make_file(t, "root/docs/IMG_0001.JPG", 1);
    make_file(t, "root/node_modules/dep/index.js", 1);
    make_dir(t, "root/empty");
    Config cfg = {0};
    strlist_push(t->arena, &cfg.roots, fixture_path(t, "root"));
    strlist_push(t->arena, &cfg.excludes, S("node_modules"));
    Err err;
    Snapshot *s;
    if (build_index(&cfg, index_file(t), &s, &err) != ERR_OK) {
        fprintf(stderr, "build_index: %s\n", err.msg);
        return nullptr;
    }
    return s;
}

static ThreadPool *pool(Test *t, size_t threads) {
    ThreadPool *p;
    Error e = threadpool_create(threads, &p, nullptr);
    CHECK(t, e == ERR_OK);
    return p;
}

static Excludes node_modules_excluded(Test *t) {
    StringList patterns = {0};
    strlist_push(t->arena, &patterns, S("node_modules"));
    Excludes ex;
    Error e = excludes_init(t->arena, &ex, patterns, S("/home/me"), nullptr);
    CHECK(t, e == ERR_OK);
    return ex;
}

/* changed turns paths below the fixture into watch events. */
static WatchEventList changed(Test *t, size_t count, const char *const *rel, bool rescan) {
    WatchEventList events = {0};
    events.items = arena_push(t->arena, (count + 1) * sizeof *events.items);
    for (size_t i = 0; i < count; i++) events.items[events.count++] = (WatchEvent){fixture_path(t, rel[i]), rescan};
    return events;
}

/* ---- tests ---- */

static void test_query_parse(Test *t) {
    Arena *a = arena_create(0);
    QueryDefaults d = {.now = 1710342000};
    CHECK_STR(t, parsed(a, "", d), "and()");
    CHECK_STR(t, parsed(a, "report", d), "sub:report");
    CHECK_STR(t, parsed(a, "foo bar", d), "and(sub:foo sub:bar)");
    CHECK_STR(t, parsed(a, "a b | c", d), "or(and(sub:a sub:b) sub:c)");
    CHECK_STR(t, parsed(a, "!draft", d), "not(sub:draft)");
    CHECK_STR(t, parsed(a, "<a|b> c", d), "and(or(sub:a sub:b) sub:c)");
    CHECK_STR(t, parsed(a, "path:\"my dir\"", d), "sub+path:my dir");
    CHECK_STR(t, parsed(a, "*.txt", d), "glob:*.txt");
    CHECK_STR(t, parsed(a, "regex:^img_\\d+", d), "re:^img_\\d+");
    CHECK_STR(t, parsed(a, "folder:case:src", d), "and(dir sub+case:src)");
    CHECK_STR(t, parsed(a, "file:", d), "file");
    CHECK_STR(t, parsed(a, "ext:pdf;.DOCX", d), "ext:pdf,docx");
    CHECK_STR(t, parsed(a, "size:>1kb", d), "size[1025,9223372036854775807]");
    CHECK_STR(t, parsed(a, "size:1kb..2kb", d), "size[1024,2048]");
    CHECK_STR(t, parsed(a, "size:<1kb", d), "size[-9223372036854775808,1023]");
    CHECK_STR(t, parsed(a, "len:>40", d), "len[41,9223372036854775807]");
    CHECK_STR(t, parsed(a, "depth:3", d), "depth[3,3]");
    CHECK_STR(t, parsed(a, "infolder:/tmp", d), "infolder:/tmp");
    CHECK_STR(t, parsed(a, "c:notafunction", d), "sub:c:notafunction");
    CHECK_STR(t, parsed(a, "size:big", d), "error: size: expected a size such as 10mb, got \"big\"");
    CHECK_STR(t, parsed(a, "dm:someday", d), "error: dm: unknown date \"someday\"");
    CHECK_STR(t, parsed(a, "len:x", d), "error: len: expected a number, got \"x\"");
    QueryDefaults all = {.regex = true, .case_sensitive = true, .match_path = true, .now = d.now};
    CHECK_STR(t, parsed(a, "foo ext:txt", all), "and(re+case+path:foo ext:txt)");
    arena_destroy(a);
}

/* reload_time_zone makes localtime_r follow a changed TZ: mktime reads it again, as if it called tzset. */
static void reload_time_zone(void) {
    struct tm tm = {.tm_year = 124, .tm_mday = 1, .tm_isdst = -1};
    mktime(&tm);
}

static void test_values(Test *t) {
    Range r;
    Err err;
    CHECK(t, parse_size_value(S("1.5kb"), 0, &r, &err) == ERR_OK && r.lo == 1536 && r.hi == 1536);
    CHECK(t, parse_size_value(S("2 MB"), 0, &r, &err) == ERR_OK && r.lo == 2 << 20);
    CHECK(t, parse_size_value(S("gigantic"), 0, &r, &err) == ERR_OK && r.lo == (128 << 20) + 1 && r.hi == INT64_MAX);
    CHECK(t, parse_size_value(S("3 parsecs"), 0, &r, &err) == ERR_PARSE);

    Error set = env_set(S("TZ"), S("UTC"), nullptr);
    CHECK(t, set == ERR_OK);
    reload_time_zone();
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
        bool ok = parse_date_value(S(cases[i].in), now, &r, &err) == ERR_OK;
        if (!ok || r.lo != cases[i].lo || r.hi != cases[i].hi)
            fprintf(stderr, "date %s: got [%lld,%lld]\n", cases[i].in, (long long)r.lo, (long long)r.hi);
        CHECK(t, ok && r.lo == cases[i].lo && r.hi == cases[i].hi);
    }
    CHECK(t, parse_range(S(">2023"), parse_date_value, now, &r, &err) == ERR_OK && r.lo == 1704067200 && r.hi == INT64_MAX);
    env_unset(S("TZ"));
    reload_time_zone();
}

static void test_excludes(Test *t) {
    CHECK(t, glob_match(S("**/.git"), S("/home/me/x/.git")));
    CHECK(t, glob_match(S("/a/**/c"), S("/a/c")));
    CHECK(t, glob_match(S("IMG_[0-9]???.JPG"), S("IMG_0001.JPG")));

    StringList patterns = {0};
    strlist_push(t->arena, &patterns, S("node_modules"));
    strlist_push(t->arena, &patterns, S("/proc"));
    strlist_push(t->arena, &patterns, S("~/*/cache/"));
    strlist_push(t->arena, &patterns, S("**/.git"));
    Excludes ex;
    Err err;
    CHECK(t, excludes_init(t->arena, &ex, patterns, S("/home/me"), &err) == ERR_OK);
    CHECK(t, excludes_match(&ex, S("/x/node_modules"), S("node_modules")));
    CHECK(t, excludes_match(&ex, S("/proc/1/status"), S("status")));
    CHECK(t, !excludes_match(&ex, S("/processes"), S("processes")));
    CHECK(t, excludes_match(&ex, S("/home/me/x/cache/a/b"), S("b")));
    CHECK(t, !excludes_match(&ex, S("/home/me/x/caches"), S("caches")));
    CHECK(t, excludes_match(&ex, S("/src/repo/.git/objects/ab"), S("ab")));
    StringList invalid = {0};
    strlist_push(t->arena, &invalid, S("[abc"));
    CHECK(t, excludes_init(t->arena, &ex, invalid, S("/home/me"), &err) == ERR_INVALID_ARGUMENT);
}

static void test_index_search(Test *t) {
    Snapshot *s = build_fixture(t);
    CHECK(t, s != nullptr);
    if (!s) return;
    Arena *a = arena_create(0);
    ThreadPool *cpu = pool(t, 0);
    CHECK_STR(t, search_names(a, cpu, s, "main"), "main.go main_test.go");
    CHECK_STR(t, search_names(a, cpu, s, "ww:main"), "main.go main_test.go");
    CHECK_STR(t, search_names(a, cpu, s, "file: ww:test"), "main_test.go");
    CHECK_STR(t, search_names(a, cpu, s, "*.go"), "main.go main_test.go");
    CHECK_STR(t, search_names(a, cpu, s, "*_????.JPG"), "IMG_0001.JPG");
    CHECK_STR(t, search_names(a, cpu, s, "ext:pdf;md"), "Readme.md report_2024.PDF");
    CHECK_STR(t, search_names(a, cpu, s, "case:readme"), "");
    CHECK_STR(t, search_names(a, cpu, s, "wfn:readme.md"), "Readme.md");
    CHECK_STR(t, search_names(a, cpu, s, "regex:_\\d+\\.pdf$"), "report_2024.PDF");
    CHECK(t, str_starts_with(search_names(a, cpu, s, "regex:(unclosed"), S("error: invalid regex \"(unclosed\": ")));
    CHECK_STR(t, search_names(a, cpu, s, "size:>1kb"), "main_test.go report_2024.PDF");
    CHECK_STR(t, search_names(a, cpu, s, "folder:"), "root docs empty src");
    CHECK_STR(t, search_names(a, cpu, s, "main !test"), "main.go");
    CHECK_STR(t, search_names(a, cpu, s, "<readme|img> ext:md"), "Readme.md");
    CHECK_STR(t, search_names(a, cpu, s, "node_modules"), "");
    CHECK_STR(t, search_names(a, cpu, s, "nothing-like-this"), "");
    CHECK_STR(t, search_names(a, cpu, s, "path:src/main"), "main.go main_test.go");
    CHECK_STR(t, search_names(a, cpu, s, "len:<8"), "docs empty src main.go");

    String docs = fixture_path(t, "root/docs");
    CHECK_STR(t, search_names(a, cpu, s, str_cstr(a, str_format(a, "infolder:%s", docs.data))), "IMG_0001.JPG Readme.md");
    CHECK_STR(t, search_names(a, cpu, s, str_cstr(a, str_format(a, "parent:%s file:", docs.data))), "IMG_0001.JPG Readme.md");

    /* ranking: an exact stem beats a prefix, which beats a word start */
    QueryNode *n;
    IdList hits = run_query(a, cpu, s, "main", &n);
    CHECK(t, search_rank(a, s, hits.items, hits.count, n, 1) == 1);
    CHECK_STR(t, snap_name_view(s, hits.items[0]), "main.go");

    /* keeping a few of many hits by path places the same ones a full sort would */
    hits = run_query(a, cpu, s, "file:", nullptr);
    CHECK(t, search_top(a, s, hits.items, hits.count, SORT_PATH, false, 1) == 1);
    CHECK_STR(t, snap_name_view(s, hits.items[0]), "IMG_0001.JPG");
    hits = run_query(a, cpu, s, "file:", nullptr);
    search_top(a, s, hits.items, hits.count, SORT_PATH, true, 1);
    CHECK_STR(t, snap_name_view(s, hits.items[0]), "report_2024.PDF");

    /* the file on disk reloads to the same answers */
    Err err;
    Snapshot *loaded;
    CHECK(t, index_load(index_file(t), &loaded, &err) == ERR_OK && loaded->total == s->total);
    CHECK_STR(t, search_names(a, cpu, loaded, "report"), "report_2024.PDF");
    snapshot_release(loaded);
    CHECK(t, index_load(fixture_path(t, "../missing.bin"), &loaded, &err) == ERR_NOT_FOUND);
    snapshot_release(s);
    threadpool_destroy(cpu);
    arena_destroy(a);
}

static void test_updater_and_compaction(Test *t) {
    Snapshot *s = build_fixture(t);
    CHECK(t, s != nullptr);
    if (!s) return;
    Index ix;
    index_init(&ix, s);
    Excludes ex = node_modules_excluded(t);
    ThreadPool *cpu = pool(t, 0), *io = pool(t, io_thread_count());
    Updater *u = updater_create(io, &ix, &ex);
    Arena *a = arena_create(0);

    /* a held snapshot keeps answering while newer ones are published */
    Snapshot *before = index_acquire(&ix);

    make_file(t, "root/new/deep/fresh.txt", 3);
    make_file(t, "root/src/main.go", 99);
    remove_tree(t, fixture_path(t, "root/docs"));
    const char *const batch[] = {"root/new/deep/fresh.txt", "root/src/main.go", "root/docs"};
    WatchEventList events = changed(t, countof(batch), batch, false);
    CHECK(t, updater_apply(u, events.items, events.count));

    Snapshot *after = index_acquire(&ix);
    CHECK_STR(t, search_names(a, cpu, after, "fresh"), "fresh.txt");
    CHECK_STR(t, search_names(a, cpu, after, "ext:md;jpg"), "");
    CHECK_STR(t, search_names(a, cpu, after, "size:99"), "main.go");
    CHECK_STR(t, search_names(a, cpu, after, "folder:"), "root empty new deep src");
    CHECK_STR(t, search_names(a, cpu, before, "ext:md;jpg"), "IMG_0001.JPG Readme.md");

    /* reconciling an unchanged path changes nothing */
    const char *const unchanged[] = {"root/src/main.go"};
    events = changed(t, 1, unchanged, false);
    CHECK(t, !updater_apply(u, events.items, events.count));

    /* a rescan reads a directory again, finding what no event reported */
    make_file(t, "root/src/unreported.c", 4);
    const char *const src[] = {"root/src"};
    events = changed(t, 1, src, false);
    CHECK(t, !updater_apply(u, events.items, events.count));
    events = changed(t, 1, src, true);
    CHECK(t, updater_apply(u, events.items, events.count));
    Snapshot *rescanned = index_acquire(&ix);
    CHECK_STR(t, search_names(a, cpu, rescanned, "unreported"), "unreported.c");
    CHECK_STR(t, search_names(a, cpu, rescanned, "file: main"), "main.go main_test.go");
    CHECK_STR(t, search_names(a, cpu, rescanned, "folder:"), "root empty new deep src");
    snapshot_release(rescanned);

    /* compaction drops tombstones and keeps every live answer */
    Err err;
    Snapshot *latest = index_acquire(&ix);
    Snapshot *merged;
    CHECK(t, index_compact(latest, index_file(t), &merged, &err) == ERR_OK);
    CHECK(t, merged->seg_count == 1 && merged->dead_count == 0 && merged->total == snap_live_count(latest));
    snapshot_release(latest);
    CHECK_STR(t, search_names(a, cpu, merged, "fresh"), "fresh.txt");
    CHECK_STR(t, search_names(a, cpu, merged, "folder:"), "root empty new deep src");
    snapshot_retain(merged);
    index_publish(&ix, merged);
    updater_reset(u, merged);
    remove_tree(t, fixture_path(t, "root/new"));
    const char *const gone[] = {"root/new"};
    events = changed(t, 1, gone, false);
    CHECK(t, updater_apply(u, events.items, events.count));
    latest = index_acquire(&ix);
    CHECK_STR(t, search_names(a, cpu, latest, "fresh"), "");
    snapshot_release(latest);

    snapshot_release(before);
    snapshot_release(after);
    updater_destroy(u);
    threadpool_destroy(cpu);
    threadpool_destroy(io);
    index_destroy(&ix);
    arena_destroy(a);
}

/* append_entry writes raw journal bytes after what is there, as another implementation would. */
static void append_entry(Test *t, String path, String bytes) {
    int fd;
    Error e = file_open_append(path, &fd, nullptr);
    CHECK(t, e == ERR_OK);
    if (e != ERR_OK) return;
    e = file_write(fd, bytes, nullptr);
    CHECK(t, e == ERR_OK);
    file_close(fd);
}

static void apply_and_journal(Test *t, Updater *u, Journal *j, WatchEventList events) {
    Snapshot *before = updater_snapshot(u);
    snapshot_retain(before);
    CHECK(t, updater_apply(u, events.items, events.count));
    journal_record(j, before, updater_snapshot(u));
    snapshot_release(before);
}

/* Changes journaled by a daemon are seen by every later load of the index file. */
static void test_journal(Test *t) {
    Snapshot *s = build_fixture(t);
    CHECK(t, s != nullptr);
    if (!s) return;
    Index ix;
    index_init(&ix, s);
    Err err;
    Journal *j;
    CHECK(t, journal_open(index_file(t), s, &j, &err) == ERR_OK);
    Excludes ex = {0};
    ThreadPool *cpu = pool(t, 0), *io = pool(t, io_thread_count());
    Updater *u = updater_create(io, &ix, &ex);
    Arena *a = arena_create(0);

    make_file(t, "root/journaled.txt", 7);
    make_file(t, "root/src/main.go", 42);
    remove_tree(t, fixture_path(t, "root/docs"));
    const char *const batch[] = {"root/journaled.txt", "root/src/main.go", "root/docs"};
    apply_and_journal(t, u, j, changed(t, countof(batch), batch, false));
    bool replaced;
    CHECK(t, journal_flush(j, &replaced, &err) == ERR_OK && !replaced);

    Snapshot *loaded;
    CHECK(t, index_load(index_file(t), &loaded, &err) == ERR_OK);
    CHECK_STR(t, search_names(a, cpu, loaded, "journaled"), "journaled.txt");
    CHECK_STR(t, search_names(a, cpu, loaded, "size:42"), "main.go");
    CHECK_STR(t, search_names(a, cpu, loaded, "ext:md;jpg"), "");
    CHECK(t, snap_live_count(loaded) == snap_live_count(updater_snapshot(u)));
    snapshot_release(loaded);

    /* an update entry, which other implementations write, replaces a base record's size and times */
    String jpath = fixture_path(t, "../index.bin.journal");
    IdList hits = run_query(a, cpu, updater_snapshot(u), "wfn:main_test.go", nullptr);
    CHECK(t, hits.count == 1 && hits.items[0] < updater_snapshot(u)->segs[0]->count);
    char entry[29] = {'U'};
    int64_t value = 77;
    memcpy(entry + 1, &hits.items[0], 4);
    memcpy(entry + 5, &value, 8);
    memcpy(entry + 13, &value, 8);
    memcpy(entry + 21, &value, 8);
    append_entry(t, jpath, (String){entry, sizeof entry});

    /* a half-written last entry is ignored, and the next writer cuts it off */
    append_entry(t, jpath, S("A\3"));
    CHECK(t, index_load(index_file(t), &loaded, &err) == ERR_OK);
    CHECK_STR(t, search_names(a, cpu, loaded, "journaled"), "journaled.txt");
    CHECK_STR(t, search_names(a, cpu, loaded, "size:77"), "main_test.go");
    Journal *again;
    CHECK(t, journal_open(index_file(t), loaded, &again, &err) == ERR_OK && journal_entries(again) == journal_entries(j) + 1);
    journal_free(again);

    /* a new index file written by another process is noticed on the next flush */
    Snapshot *rewritten;
    CHECK(t, index_compact(loaded, index_file(t), &rewritten, &err) == ERR_OK);
    CHECK_STR(t, search_names(a, cpu, rewritten, "size:77"), "main_test.go");
    make_file(t, "root/late.txt", 1);
    const char *const late[] = {"root/late.txt"};
    apply_and_journal(t, u, j, changed(t, 1, late, false));
    CHECK(t, journal_flush(j, &replaced, &err) == ERR_OK && replaced);

    snapshot_release(rewritten);
    snapshot_release(loaded);
    journal_free(j);
    updater_destroy(u);
    threadpool_destroy(cpu);
    threadpool_destroy(io);
    index_destroy(&ix);
    arena_destroy(a);
}

/* A test client: one connection to the daemon socket. */
typedef struct {
    int fd;
    StringBuilder buf;
} Client;

/* client_request sends one JSON line and parses the one-line reply into the arena. */
static const Node *client_request(Client *c, const char *line, Arena *arena) {
    if (net_send(c->fd, str_concat(arena, S(line), S("\n")), nullptr) != ERR_OK) return nullptr;
    size_t nl;
    while (!str_find_char((String){c->buf.data, c->buf.len}, '\n', &nl)) {
        char chunk[4096];
        size_t got;
        if (net_receive(c->fd, chunk, sizeof chunk, &got, nullptr) != ERR_OK || got == 0) return nullptr;
        str_builder_append(&c->buf, (String){chunk, got});
    }
    Node *reply;
    Error e = json_parse(arena, (String){c->buf.data, nl}, &reply, nullptr);
    memmove(c->buf.data, c->buf.data + nl + 1, c->buf.len - nl - 1);
    c->buf.len -= nl + 1;
    return e == ERR_OK ? reply : nullptr;
}

static int64_t reply_int(const Node *reply, const char *key) { return reply ? node_get_int(reply, S(key), -1) : -1; }

static String reply_string(const Node *reply, const char *key) { return reply ? node_get_string(reply, S(key), S("")) : S(""); }

static void test_server(Test *t) {
    Snapshot *s = build_fixture(t);
    CHECK(t, s != nullptr);
    if (!s) return;
    Index ix;
    index_init(&ix, s);
    Arena *a = arena_create(0);
    String socket_path = str_format(a, "/tmp/eind-test-%d.sock", process_id());
    Err err;
    ThreadPool *cpu = pool(t, 0);
    Server *srv;
    CHECK(t, server_start(cpu, &ix, socket_path, index_file(t), &srv, &err) == ERR_OK);
    CHECK(t, server_running(socket_path));
    Server *second;
    CHECK(t, server_start(cpu, &ix, socket_path, index_file(t), &second, &err) == ERR_IO);
    Client c = {.buf = str_builder_create(a, 4096)};
    CHECK(t, net_connect_unix(socket_path, &c.fd, &err) == ERR_OK);
    const Node *reply = client_request(&c, "{\"id\":\"q1\",\"query\":\"main\",\"limit\":1}", a);
    CHECK_STR(t, reply_string(reply, "id"), "q1");
    CHECK(t, reply_int(reply, "total") == 2);
    const Node *results = reply ? node_get(reply, S("results")) : nullptr;
    CHECK(t, results && results->count == 1);
    if (results && results->count) {
        CHECK_STR(t, node_get_string(results->items[0], S("name"), S("")), "main.go");
        CHECK_STR(t, node_get_string(results->items[0], S("type"), S("")), "file");
    }
    reply = client_request(&c, "{\"id\":7.50,\"op\":\"status\"}", a);
    CHECK(t, reply_int(reply, "files") == 5 && reply_int(reply, "folders") == 4);
    const Node *id = reply ? node_get(reply, S("id")) : nullptr;
    CHECK(t, id && id->kind == NODE_FLOAT && str_equal(id->text, S("7.50")));
    reply = client_request(&c, "{\"id\":2,\"query\":\"size:huge!\"}", a);
    CHECK(t, str_contains(reply_string(reply, "error"), S("size:")));
    reply = client_request(&c, "{\"query\":\"\",\"limit\":0}", a);
    CHECK(t, reply_int(reply, "total") == 9 && reply && node_get(reply, S("results"))->count == 0);
    reply = client_request(&c, "[1]", a);
    CHECK_STR(t, reply_string(reply, "error"), "invalid request: expected a JSON object");
    net_close(c.fd);
    server_stop(srv);
    CHECK(t, !server_running(socket_path));
    threadpool_destroy(cpu);
    index_destroy(&ix);
    arena_destroy(a);
}

static void test_config(Test *t) {
    Arena *a = arena_create(0);
    String path = fixture_path(t, "../config");
    Err err;
    CHECK(t, file_write_atomic(path, S("# comment\nroot = ~/x\nexclude = *.tmp\n"), 0644, &err) == ERR_OK);
    Config cfg;
    CHECK(t, config_load(a, path, &cfg, &err) == ERR_OK);
    String expected = path_join(a, env_home(a), S("x"));
    CHECK(t, cfg.roots.count == 1 && str_equal(cfg.roots.items[0], expected));
    CHECK(t, cfg.excludes.count == 1 && str_equal(cfg.excludes.items[0], S("*.tmp")));
    CHECK(t, file_write_atomic(path, S("colour = blue\n"), 0644, &err) == ERR_OK);
    CHECK(t, config_load(a, path, &cfg, &err) == ERR_PARSE && strstr(err.msg, "unknown key"));
    CHECK(t, config_load(a, fixture_path(t, "../missing-config"), &cfg, &err) == ERR_OK &&
                 strlist_contains(cfg.excludes, S("node_modules")));
    arena_destroy(a);
}

int main(void) {
    Arena *arena = arena_create(0);
    String top;
    if (dir_create_temp(arena, S("eind-test-"), &top, nullptr) != ERR_OK) {
        fprintf(stderr, "cannot create a scratch directory\n");
        return 1;
    }
    Test t = {.arena = arena, .scratch = path_join(arena, top, S("tree"))};
    test_query_parse(&t);
    test_values(&t);
    test_excludes(&t);
    test_index_search(&t);
    test_updater_and_compaction(&t);
    test_journal(&t);
    test_server(&t);
    test_config(&t);
    remove_tree(&t, top);
    printf("%d checks, %d failures\n", t.checks, t.failures);
    arena_destroy(arena);
    return t.failures ? 1 : 0;
}

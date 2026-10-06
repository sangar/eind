#include "build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../fs/fs.h"
#include "../fs/scanner.h"

bool stderr_is_terminal(void) { return isatty(STDERR_FILENO); }

const char *format_duration(double ms, char buf[32]) {
    if (ms < 1000) {
        snprintf(buf, 32, "%.0fms", ms);
        return buf;
    }
    snprintf(buf, 32, "%.2f", ms / 1000);
    size_t n = strlen(buf);
    while (buf[n - 1] == '0') buf[--n] = '\0';
    if (buf[n - 1] == '.') buf[--n] = '\0';
    strcat(buf, "s");
    return buf;
}

static void show_progress(void *ctx, uint32_t added) {
    (void)added;
    const SegmentBuilder *b = ctx;
    char n[32];
    fprintf(stderr, "\r  %s entries...", commas(b->count, n));
}

Snapshot *build_index(const Config *cfg, const char *index_path, Err *err) {
    Excludes ex;
    if (!excludes_init(&ex, &cfg->excludes, err)) return NULL;
    int64_t start = monotonic_us();
    SegmentBuilder b;
    builder_init(&b, 0);
    StrList roots = {0};
    uint32_t unreadable = 0;
    bool tty = stderr_is_terminal();
    for (size_t i = 0; i < cfg->roots.len; i++) {
        char *abs = path_abs(cfg->roots.items[i]);
        ScanResult res;
        Err why;
        if (!scan_tree(&b, abs, NO_PARENT, &ex, tty ? show_progress : NULL, &b, &res, &why)) {
            fprintf(stderr, "skipping %s: %s\n", abs, why.msg);
            free(abs);
            continue;
        }
        strlist_push_owned(&roots, abs);
        unreadable += res.errors;
    }
    excludes_free(&ex);
    if (roots.len == 0) {
        err_set(err, "none of the configured roots could be read");
        builder_free(&b);
        strlist_free(&roots);
        return NULL;
    }
    bool ok = segment_write(index_path, &b, &roots, (int64_t)time(NULL), err);
    uint32_t dirs = 0;
    for (uint32_t i = 0; i < b.count; i++) dirs += (b.recs[i].flags & RECORD_DIR) != 0;
    uint32_t files = b.count - dirs;
    builder_free(&b);
    strlist_free(&roots);
    if (!ok) return NULL;

    char nfiles[32], ndirs[32], took[32], size[32];
    if (tty) fputs("\r\x1b[K", stderr);
    fprintf(stderr, "Indexed %s files and %s folders in %s", commas(files, nfiles), commas(dirs, ndirs),
            format_duration(elapsed_ms_since(start), took));
    if (unreadable) fprintf(stderr, " (%u folders unreadable)", unreadable);
    struct stat st;
    if (stat(index_path, &st) == 0) fprintf(stderr, "; index is %s at %s", human_size(st.st_size, size), index_path);
    fputc('\n', stderr);
    bool missing;
    return index_load(index_path, &missing, err);
}

Snapshot *load_or_build(const char *config_path, const char *index_path, Err *err) {
    bool missing;
    Snapshot *s = index_load(index_path, &missing, err);
    if (s || !missing) return s;
    fprintf(stderr, "No index at %s yet; building one (run `eind index` to rebuild later).\n", index_path);
    Config cfg;
    if (!config_load(config_path, &cfg, err)) return NULL;
    s = build_index(&cfg, index_path, err);
    config_free(&cfg);
    return s;
}

#include "output.h"

#include <string.h>

#include "../core/json.h"

void record_of(const Snapshot *s, uint32_t id, OutRecord *rec, StrBuf *path) {
    snap_path(s, id, path);
    *rec = (OutRecord){.path = path->data,
                       .name = snap_name(s, id),
                       .dir = snap_is_dir(s, id),
                       .size = snap_size(s, id),
                       .mtime = snap_mtime(s, id),
                       .ctime = snap_ctime(s, id)};
}

void record_json(StrBuf *sb, const OutRecord *r) {
    char when[40];
    sb_puts(sb, "{\"path\":");
    json_write_string(sb, r->path, strlen(r->path));
    sb_puts(sb, ",\"name\":");
    json_write_string(sb, r->name, strlen(r->name));
    sb_printf(sb, ",\"type\":\"%s\",\"size\":%lld,\"modified\":\"%s\"", r->dir ? "dir" : "file", (long long)r->size,
              format_rfc3339(r->mtime, when));
    if (r->ctime) sb_printf(sb, ",\"created\":\"%s\"", format_rfc3339(r->ctime, when));
    sb_putc(sb, '}');
}

static void csv_field(StrBuf *sb, const char *s) {
    bool quote = s[0] == ' ' || strpbrk(s, ",\"\r\n") != NULL;
    if (!quote) {
        sb_puts(sb, s);
        return;
    }
    sb_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"') sb_putc(sb, '"');
        sb_putc(sb, *p);
    }
    sb_putc(sb, '"');
}

static void plain_line(StrBuf *sb, const OutRecord *r, const OutputOptions *o) {
    char buf[40];
    if (o->show_size) sb_printf(sb, "%9s  ", r->dir ? "<DIR>" : human_size(r->size, buf));
    if (o->show_modified) sb_printf(sb, "%s  ", format_short_time(r->mtime, buf));
    if (o->show_created) sb_printf(sb, "%s  ", r->ctime ? format_short_time(r->ctime, buf) : "                ");
    const char *text = o->name_only ? r->name : r->path;
    if (o->color && r->dir) {
        sb_printf(sb, "\x1b[1;34m%s\x1b[0m", text);
    } else {
        sb_puts(sb, text);
    }
    sb_putc(sb, o->null_sep ? '\0' : '\n');
}

static void csv_line(StrBuf *sb, const OutRecord *r) {
    char when[40];
    csv_field(sb, r->path);
    sb_putc(sb, ',');
    csv_field(sb, r->name);
    sb_printf(sb, ",%s,%lld,%s,", r->dir ? "dir" : "file", (long long)r->size, format_rfc3339(r->mtime, when));
    if (r->ctime) sb_puts(sb, format_rfc3339(r->ctime, when));
    sb_putc(sb, '\n');
}

/* A RecordSource yields record k; path storage may be reused between calls. */
typedef void (*RecordSource)(void *ctx, size_t k, OutRecord *rec, StrBuf *path);

static bool output_write(FILE *out, size_t count, RecordSource source, void *ctx, const OutputOptions *o) {
    StrBuf sb = {0}, path = {0};
    bool ok = true;
    if (o->format == FORMAT_JSON) sb_putc(&sb, '[');
    if (o->format == FORMAT_CSV) sb_puts(&sb, "path,name,type,size,modified,created\n");
    for (size_t k = 0; k < count && ok; k++) {
        OutRecord r;
        source(ctx, k, &r, &path);
        switch (o->format) {
        case FORMAT_JSON:
            sb_puts(&sb, k ? ",\n  " : "\n  ");
            record_json(&sb, &r);
            break;
        case FORMAT_CSV: csv_line(&sb, &r); break;
        case FORMAT_PLAIN: plain_line(&sb, &r, o); break;
        }
        if (sb.len >= 64 * 1024) {
            ok = fwrite(sb.data, 1, sb.len, out) == sb.len;
            sb_clear(&sb);
        }
    }
    if (o->format == FORMAT_JSON) sb_puts(&sb, count ? "\n]\n" : "]\n");
    if (ok && sb.len) ok = fwrite(sb.data, 1, sb.len, out) == sb.len;
    ok = ok && fflush(out) == 0;
    sb_free(&sb);
    sb_free(&path);
    return ok;
}

typedef struct {
    const Snapshot *s;
    const uint32_t *hits;
} HitSource;

static void hit_record(void *ctx, size_t k, OutRecord *rec, StrBuf *path) {
    const HitSource *h = ctx;
    record_of(h->s, h->hits[k], rec, path);
}

bool output_write_hits(FILE *out, const Snapshot *s, const uint32_t *hits, size_t count, const OutputOptions *o) {
    HitSource src = {s, hits};
    return output_write(out, count, hit_record, &src, o);
}

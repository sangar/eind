#include "output.h"

#include <time.h>

#include "mc/encoding/json.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"

enum { FLUSH_AT = 64 * 1024 };

OutRecord record_of(const Snapshot *s, uint32_t id, StringBuilder *path) {
    return (OutRecord){.path = snap_path(s, id, path),
                       .name = snap_name_view(s, id),
                       .dir = snap_is_dir(s, id),
                       .size = snap_size(s, id),
                       .mtime = snap_mtime(s, id),
                       .ctime = snap_ctime(s, id)};
}

/* local_rfc3339 formats Unix seconds in the local zone, such as 2024-03-13T10:00:00+01:00. */
static String local_rfc3339(Arena *arena, int64_t seconds) {
    return fmt_rfc3339(arena, seconds * NS_PER_SECOND, clock_local_offset(seconds), false);
}

void record_json(StringBuilder *out, Arena *scratch, const OutRecord *r) {
    ArenaMark mark = arena_mark(scratch);
    str_builder_append(out, S("{\"path\":"));
    json_append_quoted(out, r->path);
    str_builder_append(out, S(",\"name\":"));
    json_append_quoted(out, r->name);
    String modified = local_rfc3339(scratch, r->mtime);
    str_builder_append_format(out, ",\"type\":\"%s\",\"size\":%lld,\"modified\":\"%.*s\"", r->dir ? "dir" : "file",
                              (long long)r->size, (int)modified.len, modified.data);
    if (r->ctime) {
        String created = local_rfc3339(scratch, r->ctime);
        str_builder_append_format(out, ",\"created\":\"%.*s\"", (int)created.len, created.data);
    }
    str_builder_append_char(out, '}');
    arena_release(mark);
}

static void csv_field(StringBuilder *out, String s) {
    bool quote = (s.len && s.data[0] == ' ') || str_contains_any(s, S(",\"\r\n"));
    if (!quote) {
        str_builder_append(out, s);
        return;
    }
    str_builder_append_char(out, '"');
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] == '"') str_builder_append_char(out, '"');
        str_builder_append_char(out, s.data[i]);
    }
    str_builder_append_char(out, '"');
}

/* short_time writes local time such as 2024-03-13 10:00. */
static const char *short_time(int64_t seconds, char buf[32]) {
    time_t t = (time_t)seconds;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, 32, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

static void plain_line(StringBuilder *out, Arena *scratch, const OutRecord *r, const OutputOptions *o) {
    char when[32];
    if (o->show_size) {
        ArenaMark mark = arena_mark(scratch);
        String size = r->dir ? S("<DIR>") : fmt_bytes(scratch, r->size);
        str_builder_append_format(out, "%9.*s  ", (int)size.len, size.data);
        arena_release(mark);
    }
    if (o->show_modified) str_builder_append_format(out, "%s  ", short_time(r->mtime, when));
    if (o->show_created) str_builder_append_format(out, "%s  ", r->ctime ? short_time(r->ctime, when) : "                ");
    String text = o->name_only ? r->name : r->path;
    if (o->color && r->dir) {
        str_builder_append(out, S("\x1b[1;34m"));
        str_builder_append(out, text);
        str_builder_append(out, S("\x1b[0m"));
    } else {
        str_builder_append(out, text);
    }
    str_builder_append_char(out, o->null_sep ? '\0' : '\n');
}

static void csv_line(StringBuilder *out, Arena *scratch, const OutRecord *r) {
    ArenaMark mark = arena_mark(scratch);
    csv_field(out, r->path);
    str_builder_append_char(out, ',');
    csv_field(out, r->name);
    String modified = local_rfc3339(scratch, r->mtime);
    str_builder_append_format(out, ",%s,%lld,%.*s,", r->dir ? "dir" : "file", (long long)r->size, (int)modified.len,
                              modified.data);
    if (r->ctime) str_builder_append(out, local_rfc3339(scratch, r->ctime));
    str_builder_append_char(out, '\n');
    arena_release(mark);
}

Error output_write_hits(int fd, Arena *scratch, const Snapshot *s, const uint32_t *hits, size_t count,
                        const OutputOptions *o, Err *err) {
    StringBuilder out = str_builder_create(scratch, 2 * FLUSH_AT);
    StringBuilder path = str_builder_create(scratch, 256);
    Arena *temporary = arena_create(4096);
    Error e = ERR_OK;
    if (o->format == FORMAT_JSON) str_builder_append_char(&out, '[');
    if (o->format == FORMAT_CSV) str_builder_append(&out, S("path,name,type,size,modified,created\n"));
    for (size_t k = 0; k < count && e == ERR_OK; k++) {
        OutRecord r = record_of(s, hits[k], &path);
        switch (o->format) {
        case FORMAT_JSON:
            str_builder_append(&out, k ? S(",\n  ") : S("\n  "));
            record_json(&out, temporary, &r);
            break;
        case FORMAT_CSV: csv_line(&out, temporary, &r); break;
        case FORMAT_PLAIN: plain_line(&out, temporary, &r, o); break;
        }
        if (out.len >= FLUSH_AT) {
            e = file_write(fd, (String){out.data, out.len}, err);
            out.len = 0;
        }
    }
    if (o->format == FORMAT_JSON) str_builder_append(&out, count ? S("\n]\n") : S("]\n"));
    if (e == ERR_OK && out.len) e = file_write(fd, (String){out.data, out.len}, err);
    arena_destroy(temporary);
    return e;
}

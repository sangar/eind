#ifndef EIND_OUTPUT_H
#define EIND_OUTPUT_H

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"
#include "../index/index.h"

typedef enum { FORMAT_PLAIN, FORMAT_JSON, FORMAT_CSV } OutputFormat;

typedef struct {
    OutputFormat format;
    bool null_sep; /* terminate plain lines with NUL for xargs -0 */
    bool name_only;
    bool show_size, show_modified, show_created;
    bool color;
} OutputOptions;

/* OutRecord is one search hit in the JSON, CSV and socket formats. */
typedef struct {
    String path;
    String name;
    bool dir;
    int64_t size;
    int64_t mtime;
    int64_t ctime; /* 0 when unknown */
} OutRecord;

/* output_write_hits writes count hits to fd in the chosen format; its buffers live in scratch until it returns. */
[[nodiscard]] Error output_write_hits(int fd, Arena *scratch, const Snapshot *s, const uint32_t *hits, size_t count,
                                      const OutputOptions *o, Err *err);

/* record_of describes id, building its path in path. */
OutRecord record_of(const Snapshot *s, uint32_t id, StringBuilder *path);
/* record_json appends r as a JSON object; its timestamps are formatted in scratch, which is not out's arena, and released again. */
void record_json(StringBuilder *out, Arena *scratch, const OutRecord *r);

#endif

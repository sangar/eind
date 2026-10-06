#ifndef EIND_OUTPUT_H
#define EIND_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../core/util.h"
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
    const char *path;
    const char *name;
    bool dir;
    int64_t size;
    int64_t mtime;
    int64_t ctime; /* 0 when unknown */
} OutRecord;


/* output_write_hits returns false when writing fails, with errno set. */
bool output_write_hits(FILE *out, const Snapshot *s, const uint32_t *hits, size_t count, const OutputOptions *o);

void record_of(const Snapshot *s, uint32_t id, OutRecord *rec, StrBuf *path);
void record_json(StrBuf *sb, const OutRecord *r);

#endif

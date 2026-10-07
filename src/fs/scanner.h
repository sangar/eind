#ifndef EIND_SCANNER_H
#define EIND_SCANNER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include "../core/threadpool.h"
#include "../index/segment.h"
#include "fs.h"

typedef struct {
    uint32_t first_id, end_id; /* records added, as a half-open id range */
    uint32_t errors;           /* directories that could not be read */
} ScanResult;

typedef void (*ScanProgress)(void *ctx, uint32_t added);

/*
 * scan_tree appends root and everything below it to the builder. With
 * parent == NO_PARENT the root record is named by its full path; otherwise
 * it is attached under parent with its base name. Directories are read in
 * parallel on the I/O pool; records are appended by this thread alone, so
 * parents always precede their children.
 */
bool scan_tree(ThreadPool *pool, SegmentBuilder *b, const char *root, uint32_t parent, const Excludes *ex,
               ScanProgress progress, void *progress_ctx, ScanResult *result, Err *err);

/* scan_add_record appends one record for a stat result. */
uint32_t scan_add_record(SegmentBuilder *b, const char *name, size_t len, uint32_t parent, const struct stat *st);

#endif

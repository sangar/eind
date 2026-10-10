#ifndef EIND_SCANNER_H
#define EIND_SCANNER_H

#include "mc/concurrency/threadpool.h"
#include "mc/core/error.h"
#include "mc/platform/platform.h"
#include "mc/text/str.h"
#include "../index/segment.h"
#include "excludes.h"

typedef struct {
    uint32_t first_id, end_id; /* records added, as a half-open id range */
    uint32_t errors;           /* directories that could not be read */
} ScanResult;

typedef void (*ScanProgress)(void *context, uint32_t added);

/*
 * scan_tree appends root and everything below it to the builder. With
 * parent == NO_PARENT the root record is named by its full path; otherwise
 * it is attached under parent with its base name. Directories are read in
 * parallel on the I/O pool; records are appended by this thread alone, so
 * parents always precede their children. A root that cannot be read is an
 * error; a directory below it that cannot be read is counted in errors.
 */
[[nodiscard]] Error scan_tree(ThreadPool *pool, SegmentBuilder *b, String root, uint32_t parent, const Excludes *ex,
                              ScanProgress progress, void *progress_context, ScanResult *result, Err *err);

/* scan_add_record appends one record for what file_info or dir_read reported. */
uint32_t scan_add_record(SegmentBuilder *b, String name, uint32_t parent, const FileInfo *info);

/* seconds_of turns nanoseconds since the epoch into whole seconds, rounding down as stat does. */
int64_t seconds_of(int64_t nanoseconds);

#endif

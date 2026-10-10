#ifndef EIND_BUILD_H
#define EIND_BUILD_H

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"
#include "../index/index.h"
#include "config.h"

/* build_index scans the configured roots, writes the index file and stores a snapshot of it. */
[[nodiscard]] Error build_index(const Config *cfg, String index_path, Snapshot **snapshot, Err *err);
/* load_or_build loads the index, building it first when there is none yet. */
[[nodiscard]] Error load_or_build(String config_path, String index_path, Snapshot **snapshot, Err *err);

/* io_thread_count sizes a pool that reads directories: one per CPU, but at most a few. */
size_t io_thread_count(void);

/* format_elapsed writes a duration the way people read it: 750ms, 1.25s. */
String format_elapsed(Arena *arena, int64_t nanoseconds);

#endif

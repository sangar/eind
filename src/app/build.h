#ifndef EIND_BUILD_H
#define EIND_BUILD_H

#include <stdbool.h>

#include "../core/util.h"
#include "../index/index.h"
#include "config.h"

/* build_index scans the configured roots, writes the index file and returns a snapshot of it. */
Snapshot *build_index(const Config *cfg, const char *index_path, Err *err);
/* load_or_build loads the index, building it first when there is none yet. */
Snapshot *load_or_build(const char *config_path, const char *index_path, Err *err);

const char *format_duration(double ms, char buf[32]);

#endif

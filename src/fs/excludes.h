#ifndef EIND_EXCLUDES_H
#define EIND_EXCLUDES_H

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

/*
 * Excludes decide which paths stay out of the index. A pattern without a
 * slash is matched against the file name ("node_modules", "*.tmp"). A pattern
 * with a slash is matched against the full path and also excludes everything
 * below it ("/proc", "~/Library/Caches", "**\/.git").
 */
typedef struct {
    StringList names;
    StringList paths;
} Excludes;

/* excludes_init compiles patterns into arena, expanding a leading ~ to home; an invalid glob is ERR_INVALID_ARGUMENT. */
[[nodiscard]] Error excludes_init(Arena *arena, Excludes *ex, StringList patterns, String home, Err *err);
bool excludes_empty(const Excludes *ex);
/* excludes_match says whether the entry called name at path stays out of the index. */
bool excludes_match(const Excludes *ex, String path, String name);

#endif

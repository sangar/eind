#ifndef EIND_FS_H
#define EIND_FS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include "../core/util.h"

/*
 * Excludes decide which paths stay out of the index. A pattern without a
 * slash is matched against the file name ("node_modules", "*.tmp"). A pattern
 * with a slash is matched against the full path and also excludes everything
 * below it ("/proc", "~/Library/Caches", "**\/.git").
 */
typedef struct {
    StrList names;
    StrList paths;
} Excludes;

bool excludes_init(Excludes *ex, const StrList *patterns, Err *err);
void excludes_free(Excludes *ex);
bool excludes_empty(const Excludes *ex);
bool excludes_match(const Excludes *ex, const char *path, const char *name);

/* glob_match supports *, ?, [...] and ** across slashes, like doublestar. */
bool glob_match(const char *pattern, const char *s);
bool glob_valid(const char *pattern);

/* Platform details of stat that the index records. */
int64_t stat_birthtime(const struct stat *st);

/*
 * fs_read_dir lists the directory open at dirfd with lstat metadata for
 * every entry that skip does not reject. It returns false when the listing
 * could not be read completely. macOS reads names and metadata in bulk;
 * elsewhere it is readdir plus fstatat.
 */
typedef bool (*DirSkipFn)(void *ctx, const char *name);
typedef void (*DirEntryFn)(void *ctx, const char *name, const struct stat *st);
bool fs_read_dir(int dirfd, DirSkipFn skip, DirEntryFn add, void *ctx);
bool fs_read_dir_portable(int dirfd, DirSkipFn skip, DirEntryFn add, void *ctx);

#endif

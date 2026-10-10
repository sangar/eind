#ifdef __APPLE__

#include "defaults.h"

/*
 * Sandboxed app data stays out too, because reading ~/Library/Containers
 * makes macOS ask for permission to "access data from other apps" on every
 * rebuild.
 */
void platform_default_excludes(Arena *arena, StringList *excludes) {
    static const char *const paths[] = {"~/Library/Caches",   "~/.Trash", "~/Library/Containers",
                                        "~/Library/Group Containers", "/dev",    "/System/Volumes",
                                        "/Volumes",           "/private/var/vm"};
    for (size_t i = 0; i < countof(paths); i++) strlist_push(arena, excludes, S(paths[i]));
}

#endif

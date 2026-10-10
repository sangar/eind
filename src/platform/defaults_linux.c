#ifdef __linux__

#include "defaults.h"

#include "mc/platform/platform.h"
#include "mc/text/path.h"

void platform_default_excludes(Arena *arena, StringList *excludes) {
    String cache;
    if (env_get(arena, S("XDG_CACHE_HOME"), &cache) && cache.len && !str_equal(path_base(cache), S(".cache")))
        strlist_push(arena, excludes, cache);
    static const char *const paths[] = {"~/.local/share/Trash", "/proc", "/sys", "/dev", "/run"};
    for (size_t i = 0; i < countof(paths); i++) strlist_push(arena, excludes, S(paths[i]));
}

#endif

#ifndef EIND_DEFAULTS_H
#define EIND_DEFAULTS_H

#include "mc/core/arena.h"
#include "mc/text/str.h"

/*
 * platform_default_excludes appends what a launcher never wants to offer on
 * this system: its cache directory and trash, and its virtual or foreign
 * filesystems. Paths below home may be absolute or start with "~/".
 */
void platform_default_excludes(Arena *arena, StringList *excludes);

#endif

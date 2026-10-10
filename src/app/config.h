#ifndef EIND_CONFIG_H
#define EIND_CONFIG_H

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

/*
 * The config file is a plain list of "key = value" lines:
 *
 *   root = /Users/me
 *   exclude = node_modules
 *   exclude = ~/Library/Caches
 */
typedef struct {
    StringList roots;
    StringList excludes;
} Config;

/* config_load reads path into arena, falling back to the defaults when the file does not exist. */
[[nodiscard]] Error config_load(Arena *arena, String path, Config *cfg, Err *err);
/* config_render writes a config in the file format, with a short syntax primer. */
void config_render(const Config *cfg, StringBuilder *out);
/* config_write_default creates the file unless it exists; *created says which. */
[[nodiscard]] Error config_write_default(Arena *scratch, String path, bool *created, Err *err);

/* Paths default to XDG locations; EIND_CONFIG and EIND_INDEX override them. */
String config_path(Arena *arena);
String index_path(Arena *arena);

#endif

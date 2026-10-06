#ifndef EIND_CONFIG_H
#define EIND_CONFIG_H

#include <stdbool.h>

#include "../core/util.h"

/*
 * The config file is a plain list of "key = value" lines:
 *
 *   root = /Users/me
 *   exclude = node_modules
 *   exclude = ~/Library/Caches
 */
typedef struct {
    StrList roots;
    StrList excludes;
} Config;

void config_default(Config *cfg);
void config_default_excludes(StrList *out);
void config_free(Config *cfg);
/* config_load falls back to the defaults when the file does not exist. */
bool config_load(const char *path, Config *cfg, Err *err);
/* config_render writes a config in the file format, with a short syntax primer. */
void config_render(const Config *cfg, StrBuf *out);
/* config_write_default creates the file unless it exists; *created says which. */
bool config_write_default(const char *path, bool *created, Err *err);

/* Paths default to XDG locations; EIND_CONFIG and EIND_INDEX override them. */
char *config_path(void);
char *index_path(void);

#endif

#ifndef EIND_TUI_H
#define EIND_TUI_H

#include <stdbool.h>

#include "../core/util.h"
#include "../index/index.h"
#include "../index/query.h"

/*
 * tui_run opens the interactive search-as-you-type view on the terminal and
 * sets *chosen to the path accepted with Enter, or NULL if the user quit.
 */
bool tui_run(Snapshot *s, QueryDefaults defaults, char **chosen, Err *err);

#endif

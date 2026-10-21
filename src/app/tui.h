#ifndef EIND_TUI_H
#define EIND_TUI_H

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"
#include "../index/index.h"
#include "../index/query.h"

/*
 * tui_run opens the interactive search-as-you-type view on the terminal and
 * sets *chosen, in arena, to the path accepted with Enter, or to the empty
 * string if the user quit. Open it before starting other threads: the
 * terminal reports size changes through a signal they must not take.
 */
[[nodiscard]] Error tui_run(Arena *arena, Snapshot *s, QueryDefaults defaults, String *chosen, Err *err);

/*
 * tui_draw_row appends the list row of record id to frame: exactly width
 * columns, whatever the name holds, with control characters drawn as '?' so a
 * file name cannot send the terminal a sequence. What it formats lives in
 * scratch. The view draws every row with it; the tests check it.
 */
void tui_draw_row(StringBuilder *frame, Arena *scratch, const Snapshot *s, uint32_t id, bool selected, int width);

#endif

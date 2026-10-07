# C style review

A review of the code against the performance-oriented C style the project
aims for (arenas and pools over scattered `malloc`, explicit lifetimes and
ownership, pointer plus length strings, contiguous data, pooled threads,
assertions for invariants, measurement before optimisation). Done in
October 2026 against the baseline in [benchmarks.md](benchmarks.md); all
line numbers are from that date and may have drifted.

State at review time: warning-free build, `make test` 134 checks and 0
failures, `make sanitize` clean.

## Verdict

The architecture follows the style: lifetimes are grouped in arenas,
records are contiguous arrays addressed by id, ownership is stated in the
headers, threads are pooled and their count was measured, platform code
stays in `src/fs/fs_*.c`. The gaps below are at the edges. Two of them
affect performance; the rest are consistency.

## Findings, by value

Each item says what to change and how to check it. Measure the first two
with `tools/bench.py` before and after (see benchmarks.md); treat the
others as cleanups that need only `make test` and `make sanitize`.

### 1. Hot search loop rescans strings whose length is known

Done on October 7, though measurement showed the loop was not where the
time went: the gap to the Zig version was the index load, which decoded
every column into a record array and copied a lowercase name table on
every command, 1.3 ms of page faults and 10 MB. The base segment now reads
the file's columns in place through `seg_*` accessors (`segment.h`), the
matchers fold case while comparing instead of copying (`find_in` and
`fold_equal_at` in `search.c`), `snap_ext` scans backwards from the known
length, and hits from the base segment skip the path sort because the file
already stores them in path order. Searches went from 6.8 to 7.4 ms to 5.5
to 6.3 ms and from 20 MB to 10 MB.

### 2. Scanning allocates per directory instead of from a pool

Done on October 7. `Task` now lives inside the work item, so a submission
allocates nothing; the collector keeps and reuses `DirScan` items; and the
read buffers (arena, children, scratch path) circulate through a small
pool that a worker takes when it starts listing and the collector returns
after copying, so only the directories being listed hold one. Allocations
for an index of `~/Developer` went from 412k to 77k, the build from 382 ms
to 363 ms and its peak memory from 54 MB to 51 MB. A first version that
kept the buffers on the spare items instead peaked at 686 MB, because a
breadth-first scan queues tens of thousands of directories at once.

### 3. No assertions

`src` contains no `assert`. Runtime errors from the outside (a damaged
file, a missing path) are handled through `Err`, which is right, but
internal invariants are never checked in debug builds:

- `snap_record`, `snap_name`, `snap_lower` (`src/index/index.h:44`) take
  any id; `id < s->total` is assumed.
- `builder_add` (`src/index/segment.c:57`) assumes `parent < base_id + count`
  or `NO_PARENT` (invariant 1 in AGENTS.md).
- `arena_alloc` (`src/core/arena.c:25`) assumes `a->head` is valid after
  `arena_init`; `arena_calloc` does not check `count * size` for overflow.
- `idtable_insert` assumes the table was initialised (`capacity` is a power
  of two).

Change: add `assert` for these; `make` builds with `-g` and no `-DNDEBUG`,
so they run in tests and the sanitizer build.

### 4. Temporary work in the query parser goes through the heap

Small, but it is the pattern the style says scratch memory should remove:

- `lex` (`src/index/query.c:53`) grows a token array with `xrealloc`, then
  copies it into the arena and frees it.
- `lex_word` (`query.c:23`) builds every word in a heap `StrBuf`, then
  copies it into the arena.
- `NodeList` (`query.c:358`) grows with `xrealloc`, is copied into the arena
  in `nodes_finish` and freed.
- `plain_terms` (`src/index/search.c:569`) grows the term array with
  `xrealloc` next to an arena that holds the terms themselves.

Change: allocate from the arena the parse already owns. Tokens can be a
linked list or a two-pass count, words can be written into the arena
directly since the output is never longer than the input.

### 5. Strings outside the record are NUL-terminated pointers

`FileRecord` carries `name_off` and `name_len`, but every API above it
(`snap_name`, `snap_lower`, `path_base`, `has_prefix`, `excludes_match`,
the matchers) takes `const char *` and there are 53 `strlen` calls. The
project notes say names are NUL-terminated on purpose so libc calls work,
so this is a deliberate choice rather than an oversight. If it is ever
revisited, a `Str { const char *p; size_t len; }` type in `core/util.h`
used by the matchers and the path helpers is the smallest step, and it
would resolve finding 1 as a side effect.

### 6. One-off threads on request paths

The style prefers pooled workers to creating and joining a thread per
operation:

- `handle_line` (`src/app/server.c:291`) creates a thread per request.
- `accept_loop` (`server.c:342`) creates a thread per connection.
- `start_search` in `src/app/tui.c:128` creates a thread per search.

The search itself already fans out on the CPU pool, so these threads mostly
wait. The cost is one thread create and join per keystroke, which is cheap
on a local socket. Change only if measured; the simplest form is to run
the request on the CPU pool and keep the per-request `cancel` flag.

### 7. Small consistency items

- `int` is used for a few counts: `ThreadPool.size`
  (`src/core/threadpool.c:18`), `utf8_count` (`src/core/util.c:395`),
  `Ranked.score` and `depth` (`src/index/search.c:533`), `View.outstanding`
  (`src/app/tui.c:93`).
- `parse_range` (`src/index/query.c:99`) and `parse_size_value`
  (`query.c:180`) copy the value into a fixed 256 or 128 byte buffer and
  silently truncate longer input; an error would be clearer.
- `snap_segment` (`src/index/index.h:37`) scans the segment list backwards
  on every record access. One segment is the common case and compaction
  bounds the list, so leave it unless a profile shows it.
- `format_duration` (`src/app/build.c:24`) uses `strcat` on a 32 byte
  buffer; safe today because the number is short, but `snprintf` with the
  known length would not depend on that.

## What not to change

These looked like deviations but are deliberate or correct:

- `journal_replay` edits records in a snapshot nobody has seen yet; this is
  the documented exception to immutable snapshots.
- `struct stat` crosses the platform boundary from `fs_macos.c`; it is the
  portable representation here, filled from `getattrlistbulk` fields.
- `Err` is a 512 byte struct passed by pointer and left uninitialised on
  the stack until a failure fills it; callers only read it after `false`.

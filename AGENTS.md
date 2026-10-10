# eind (C) — notes for agents

eind is an instant file search for the terminal and for launchers. It indexes
the names and metadata of every file under its roots, answers queries such as
`report ext:pdf size:>1mb dm:thisweek` in a few milliseconds, keeps the index
fresh from filesystem events, and serves a JSON-lines socket for GUIs.

This directory is the C implementation, built on immutable segments,
snapshots and arenas. It follows the Modern C profile at Level 2 and is
built on [libmc](https://github.com/sangar/libmc), vendored in `deps/libmc`;
beyond that it needs libc, pthreads and, on macOS, CoreServices.

Read in this order:

1. this file, for the rules and the workflow;
2. [docs/architecture.md](docs/architecture.md), for how the pieces fit together;
3. [docs/file-format.md](docs/file-format.md) before touching `segment.c` or `journal.c`;
4. [docs/protocol.md](docs/protocol.md) before touching `server.c`;
5. [README.md](README.md) for the user-facing behaviour and query syntax.

`docs/original-proposal.md` is the design proposal the code started from. Parts
of it (the trigram inverted index, the private file format) were later
removed; trust `docs/architecture.md` over it.

## Build and test

```sh
cc tools/build.c -o nob   # once; ./nob rebuilds itself when tools/build.c changes
./nob             # ./eind, -O2 -g
./nob test        # out/debug/test_eind under AddressSanitizer + UndefinedBehaviorSanitizer
./nob check       # the modern-c checker (ruby ~/.agents/tools/modern-c/modern-c check), then the tests
./nob cross       # out/<target>/eind with the zig pinned in mise.toml (mise exec -- ./nob cross)
./nob clean
VERSION=1.2.3 ./nob   # what `eind version` prints (default: git describe or "dev")
```

Run `./nob test` after every change and `./nob check` before finishing one;
both must pass with `CC=clang` and `CC=gcc`. The tests always run under the
sanitizers, and a sanitizer finding fails them. A full rebuild, libmc
included, takes a few seconds and an incremental one well under a second.

The code is C23 and builds with `-Werror` under `-Wall -Wextra -Wshadow
-Wconversion -Wvla -Wstrict-prototypes -Wimplicit-fallthrough`, with clang
as the primary compiler and gcc 14 or newer also supported. The README's
Profile section states the rules the project follows and the departures it
has chosen; keep both true.

Never run a manual test against the user's real index. Isolate every run with
environment variables:

```sh
export EIND_CONFIG=/tmp/x/config EIND_INDEX=/tmp/x/index.bin EIND_SOCKET=/tmp/x/s.sock
printf 'root = /some/tree\n' > /tmp/x/config
./eind index && ./eind report
```

The user runs one eind daemon from login, which writes the shared default index
and journal (`~/.local/share/eind/index.bin` and `index.bin.journal`). Without
`EIND_INDEX`, a test would rewrite the user's index. Without `EIND_SOCKET`,
`eind serve` refuses to start ("another eind daemon is already serving").

## Layout

```
deps/libmc/  the foundation library, vendored and pinned in deps.lock: arenas, String,
             Error, containers, JSON, thread pool, platform layer, file watching,
             login services, Unix sockets, the terminal
src/index/   records and segments, the index file (segment.c), its journal (journal.c),
             snapshots (index.c), compaction (merge.c), query parser (query.c), search and ranking (search.c)
src/fs/      directory scanner, excludes, updater
src/app/     config, output formats, socket server, TUI, index build, daemon loop
src/platform/  the default excludes of each OS (defaults_darwin.c, defaults_linux.c)
src/main.c   command line: subcommands and flag parsing
tests/test_eind.c   one test binary: unit tests and end-to-end checks of the library
tools/build.c       the build program (nob)
macos/       a SwiftUI client of the socket protocol for trying the daemon by hand; not built by ./nob
packaging/   systemd user unit, sysctl drop-in and install scripts for the Linux packages
```

Dependencies point down: `app` uses `fs` and `index`; `fs` uses `index`; all
use libmc. **Nothing in `src/index` may know about file events or sockets.**
OS headers and `#if` on the platform appear only in `src/platform/` and in
libmc.

**Shared code lives in libmc.** Anything another C project could use (a
container, a text helper, a platform call) goes into libmc, not `src/`: open
a pull request on `github.com/sangar/libmc`, vendor the branch commit into
`deps/libmc` with `git archive`, and pin it in `deps.lock` with the hash from
`ruby ~/.agents/tools/modern-c/modern-c hash deps/libmc`. Never edit
`deps/libmc` in place.

## Invariants

Breaking one of these corrupts results or the shared file; check them in every
change.

1. **Parents precede children.** A record's parent id is always smaller than its
   own id. Paths are rebuilt by walking up parents (`snap_path`), compaction
   relies on deciding a parent before its children, and `segment_open` rejects
   files that break it.
2. **A record's id is its position**: base segment ids are `0..count-1`, delta
   segments continue from there. Ids are not stable across compaction.
3. **Published snapshots never change.** Searches hold a snapshot with a
   reference count while the watcher publishes newer ones. To change the index,
   build a new delta segment and tombstone bitmap and publish a new snapshot
   (`snapshot_derive`, `index_publish`). The one exception is
   `journal_replay`, which edits a snapshot that nobody else has seen yet.
4. **A change to a record is a tombstone plus a new record**, never an in-place
   edit. The journal therefore only needs `A` (add) and `R` (remove) entries.
5. **The index file and journal are shared with other eind implementations**
   that may run the daemon instead (they all default to the same paths). Their
   layout in [docs/file-format.md](docs/file-format.md) is a contract: never
   change it, only read what it allows, and keep writing what other readers
   expect (entries in path order, names in name order, the journal header with
   the file's generation).
6. **Only one daemon writes.** The daemon appends to the journal; every command
   only reads. Commands never talk to the daemon; they load the file and
   replay the journal.
7. **The base segment reads the mapping in place.** Its columns and names
   point into the read-only mapping of the file and are read through the
   `snap_*` accessors; nothing is decoded on load, so a command pays only
   for the pages its query touches. Never write to the mapping. The one
   mutable part is the overlay of sizes and times that `journal_replay`
   sets through `segment_set_times` before the segment is published.

## Conventions

The code follows a performance-oriented, explicit-ownership style. Match it.

- **Lifetimes first.** Before writing a subsystem, decide what data exists, how
  long it lives and who owns it. Data that dies together goes in an `Arena`
  (`mc/core/arena.h`): parsed queries, matchers, one scanned directory, one
  socket request, a segment, a snapshot. Free the arena, not the objects.
  Arena memory comes back zeroed and running out of memory aborts; there is
  no `malloc` in `src/`. Releasing to an `arena_mark` keeps the blocks, so a
  pooled arena that once grew stays grown (see `return_buf` in `scanner.c`).
- **Ownership is explicit.** A function that takes ownership says so in its
  comment ("takes the caller's reference", "takes the builder's buffers").
  Reference-counted types are `Segment` and `Snapshot` (`*_retain`,
  `*_release`).
- **Growable containers** live in arenas: `StringList`, `StringBuilder`,
  `IdList` (`index.h`) and anything grown with `arena_grow`. The one
  exception is `SegmentBuilder`, whose two large arrays each sit in an arena
  of their own that is replaced as they grow, so millions of records do not
  leave every outgrown copy behind. Prefer contiguous arrays and ids over
  pointers.
- **Strings** are libmc's `String`, a pointer and a length. Names from the
  index are also NUL-terminated so `regexec` works on them in place. Do not
  assume NUL-termination elsewhere; functions that take an `Arena` return
  terminated strings.
- **Errors** are libmc's `Error` codes. A fallible function returns `Error`,
  is `[[nodiscard]]`, hands results back through out-parameters and takes an
  `Err *err` last, filled with `err_set` (a message for the user). Missing
  files are `ERR_NOT_FOUND` (`index_load` uses it for "no index yet"),
  malformed input `ERR_PARSE`, a cancelled search `ERR_CANCELLED`. Parsers
  and lookups where "no" is ordinary return `bool`.
- **Threads.** libmc's `ThreadPool`; whoever owns the work creates the pool
  and passes it down, there are no globals. Searches run on a pool of one
  thread per CPU and scans on one of `io_thread_count()` (`build.c`), at most
  6, because directory reads contend on kernel locks (64 threads were 3x
  slower than 6 on APFS). `build_index` makes an I/O pool for the build,
  `daemon_run` makes both for its lifetime, and `tui_run` and the search
  command make a CPU pool. Cancellation is a libmc `Cancel` checked every
  few thousand records. Signals are taken by a thread in `signals_wait`,
  never by a handler.
- **Hot loops stay simple.** The search scans every record in parallel chunks;
  matchers are compiled once per query. Measure before optimising
  (see [docs/benchmarks.md](docs/benchmarks.md)).
- **Comments** explain why, not what; each header documents its contract in a
  short block comment. Names say what things mean in the domain.
- **No dependencies.** Use libc and the platform; do not add libraries.

## Common changes

**A new query function** (like `size:` or `dm:`): add a `Q_*` kind to
`QueryKind` in `src/index/query.h`, parse its key in `parse_word` in `query.c`
(ranges go through `range_node`, `parse_range` and a `ValueParser` such as
`parse_size_value`), add an `M_*` matcher kind and its cases in `compile` and
`matches` in `search.c`, add parse and search cases to
`test_query_parse` / `test_index_search`, and document it in the README's query
syntax table and `usage_text` in `main.c`.

**A new command line flag**: add it to the `Flag` table of its command in
`main.c`; if it takes a value, also add its name to `takes_value` there, so it
may appear anywhere on the line.

**A new socket field**: read it in `handle_search` / `handle_status` in
`server.c` with the `request_*` helpers over libmc's `Node` tree, and
document it in [docs/protocol.md](docs/protocol.md).

**A new record field**: impossible without changing the shared file format;
see invariant 5. Discuss with the user first.

**Watcher behaviour**: the daemon loop is `daemon_run` in `app/daemon.c`;
events come from libmc's `watch_read` (`mc/platform/watch.h`), already
settled and without duplicates; reconciling a changed path is
`updater_apply` in `fs/updater.c`, where an event's `rescan` flag replaces a
directory's whole subtree.

## Testing

All tests are in `tests/test_eind.c`, one function per area, called from
`main` at the bottom with the run's `Test`; `CHECK` and `CHECK_STR` record
failures and the binary prints `N checks, M failures`. Tests build a small
tree under a scratch directory (`build_fixture`, `make_file`, `remove_tree`),
index it, and query it with `search_names`, which returns the matching names
in a stable order. Watch events are built with `changed`. What libmc does
itself (sorting, JSON, globs, the platform layer) is tested in libmc.

Add a test with every behaviour change. For the daemon and journal, drive the
`Updater` and `Journal` directly as `test_journal` does rather than starting
processes. For an end-to-end check of the binary, use an isolated environment
(see above) and a temporary tree.

## Pitfalls

- `./nob` compares timestamps to the second; when a script edits a source
  file right after a build, wait a second or the change is not rebuilt.
- Directory listings come from libmc's `dir_read`, which on macOS uses
  `getattrlistbulk`; its packed attributes are parsed in libmc's `posix.c`.
- Case-insensitive matching folds ASCII only (`fold_byte` in `search.c`,
  `str_compare_ignore_case`); `é` and `É` are different. Regular expressions
  use the C library's POSIX engine (`regcomp`), with `\d`, `\w`, `\s`
  rewritten to POSIX classes in `posix_regex`; non-greedy and lookaround are
  not supported.
- On Linux, libmc's watcher adds an inotify watch per directory when the
  daemon starts and fails when the system runs out of them, so `eind serve`
  stops with an error rather than watching part of the tree.
- The TUI draws on `/dev/tty` through libmc's `Terminal`, not on stdout, so
  `vim "$(eind)"` works. Open it before starting threads: resizes arrive as
  a signal that every thread must block. It has no automated tests; drive it
  through a pseudo-terminal (`script -qfec "./eind tui" /dev/null`) or by
  hand after changing `app/tui.c`.
- The repository is `git@github.com:sangar/eind.git`. It held a Go
  implementation until October 2026, kept at the `go-final` tag; this C
  implementation replaced it. Ask the user before large rewrites.

## Open work

Known gaps, roughly by value:

1. **A folder's own modified time is not refreshed** by the watcher
   (`upsert` in `updater.c` skips directories); changes below it are seen.
2. **Unicode case folding** (`É` matching `é`) in name matching and sorting.
3. **The TUI has no tests**; a scripted input source like the server tests use
   would allow them.
4. **Regex speed**: POSIX `regexec` makes regex queries about 3x slower than
   RE2-style engines (see the benchmarks).
5. **FreeBSD** would need a kqueue backend in libmc's watcher; Windows needs
   a libmc platform layer first.
6. **No assertions on internal invariants**, the open finding of
   [docs/style-review.md](docs/style-review.md).
7. **Index build memory** rose with the move to libmc: indexing `/usr`
   (316k entries) peaks at about 65 MB against 55 MB before, and takes about
   8% longer. The rest is malloc fragmentation in the scanner: read buffers
   are allocated on the I/O workers and freed on the collector, so they
   spread over the workers' malloc arenas (with `MALLOC_ARENA_MAX=1` the
   peak matches the old build). Searches are as fast as before or faster,
   except sorting by size and JSON output of thousands of results, about
   10% slower.

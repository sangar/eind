# eind (C) — notes for agents

eind is an instant file search for the terminal and for launchers. It indexes
the names and metadata of every file under its roots, answers queries such as
`report ext:pdf size:>1mb dm:thisweek` in a few milliseconds, keeps the index
fresh from filesystem events, and serves a JSON-lines socket for GUIs.

This directory is the C implementation, built on immutable segments,
snapshots and arenas. It has no dependencies beyond libc, pthreads and, on
macOS, CoreServices.

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
make            # ./eind, -O2 -g
make test       # build/test_eind: unit and integration tests, ~2 s
make sanitize   # the tests under AddressSanitizer + UndefinedBehaviorSanitizer, then cleans
make clean
make VERSION=1.2.3   # what `eind version` prints (default: git describe or "dev")
```

Run `make test` after every change and `make sanitize` before finishing any
change that touches memory, threads or the file formats. A full rebuild takes
about 2 s and an incremental one well under a second, so build often.

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
src/core/    arena, StrBuf/StrList/U32Vec, hash table, queue, thread pools, sort, JSON, util
src/index/   records and segments, the index file (segment.c), its journal (journal.c),
             snapshots (index.c), compaction (merge.c), query parser (query.c), search and ranking (search.c)
src/fs/      directory scanner, excludes and globs, watcher, updater
             fs_macos.c (FSEvents, getattrlistbulk), fs_linux.c (inotify), fs_common.c (portable parts)
src/app/     config, output formats, socket server, TUI, login service, index build, daemon loop
src/main.c   command line: subcommands and flag parsing
tests/test_eind.c   one test binary: unit tests and end-to-end checks of the library
macos/       a SwiftUI client of the socket protocol for trying the daemon by hand; not built by make
packaging/   systemd user unit, sysctl drop-in and install scripts for the Linux packages
```

Dependencies point down: `app` uses `fs` and `index`; `fs` uses `index`; all
use `core`. **Nothing in `src/index` may know about FSEvents, inotify or
sockets**, and platform code stays in `src/fs/fs_*.c`.

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
  (`core/arena.h`): parsed queries, matchers, one scanned directory, one socket
  request. Free the arena, not the objects.
- **Ownership is explicit.** A function that takes ownership says so in its
  comment ("takes the caller's reference", "takes the builder's buffers").
  Reference-counted types are `Segment` and `Snapshot` (`*_retain`,
  `*_release`).
- **Growable containers** are `StrBuf`, `StrList`, `U32Vec` (`core/util.h`)
  and `SegmentBuilder`; prefer contiguous arrays and ids over pointers.
- **Strings** are pointer plus length where they come from the index (`name_off`,
  `name_len` in `FileRecord`); names are also NUL-terminated so libc calls work.
  Do not assume NUL-termination for new data; pass lengths.
- **Errors** travel as `bool` or `NULL` plus an `Err *err` filled with
  `err_set` (a message for the user). Allocation failure aborts
  (`xmalloc`, `xcalloc`, `xrealloc`, `xstrdup`).
- **Threads.** `core/threadpool.c` provides pools; whoever owns the work
  creates them and passes them down, there are no globals. Searches run on
  a pool of `cpu_count()` threads and scans on one of `io_thread_count()`,
  at most 6, because directory reads contend on kernel locks (64 threads
  were 3x slower than 6 on APFS). `build_index` makes an I/O pool for the
  build, `daemon_run` makes both for its lifetime, and `tui_run` and the
  search command make a CPU pool. Cancellation is an `atomic_int` checked
  every few thousand records.
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
`server.c` with the `json_*` helpers, and document it in
[docs/protocol.md](docs/protocol.md).

**A new record field**: impossible without changing the shared file format;
see invariant 5. Discuss with the user first.

**Watcher behaviour**: the daemon loop is `daemon_run` in `app/daemon.c`;
reconciling a changed path is `updater_apply` in `fs/updater.c`; platform
event sources implement `fs/watcher.h`.

## Testing

All tests are in `tests/test_eind.c`, one function per area, registered in
`main` at the bottom; `CHECK` and `CHECK_STR` record failures and the binary
prints `N checks, M failures`. Tests build a small tree under a scratch
directory (`build_fixture`, `make_file`, `remove_tree`), index it, and query it
with `search_names`, which returns the matching names in a stable order.

Add a test with every behaviour change. For the daemon and journal, drive the
`Updater` and `Journal` directly as `test_journal` does rather than starting
processes. For an end-to-end check of the binary, use an isolated environment
(see above) and a temporary tree.

## Pitfalls

- `make` compares timestamps to the second; when a script edits a source file
  right after a build, wait a second or the change is not rebuilt.
- `getattrlistbulk` (macOS) returns attributes packed in a fixed order, with
  the error field right after the returned-attribute set; see `fs_read_dir` in
  `fs_macos.c`. Other filesystems fall back to `readdir` plus `fstatat`.
- Case-insensitive matching folds ASCII only (`ascii_lower`); `é` and `É` are
  different. Regular expressions use the C library's POSIX engine
  (`regcomp`), with `\d`, `\w`, `\s` rewritten to POSIX classes in
  `posix_regex`; non-greedy and lookaround are not supported.
- The JSON parser (`core/json.c`) is minimal, for the socket protocol only.
- The TUI draws on `/dev/tty`, not stdout, so `vim "$(eind)"` works; it has
  no automated tests, so try it by hand after changing `app/tui.c`.
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
5. **FreeBSD** would need a kqueue watcher backend; Windows is not supported.
6. **Style review findings** in [docs/style-review.md](docs/style-review.md):
   `strlen` on known-length strings in the search hot loop, per-directory
   heap allocation in the scanner, no assertions on internal invariants.

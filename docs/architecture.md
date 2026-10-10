# Architecture

How eind (C) works, from the index file to an answer. Code locations are
given as `file: function`.

## Data

**Records.** Every file and folder is a fixed-size `FileRecord`
(`index/segment.h`): parent id, name offset and length, flags (`RECORD_DIR`),
size, modified and created times. A record's id is its position. Parents
always have smaller ids than their children, so a full path is rebuilt by
walking up the parents (`index.c: snap_path`) and is never stored.

**Segments.** A `Segment` is an immutable set of records plus their names,
read one field at a time through the `snap_*` accessors in `index.h`. Two
kinds exist:

- the **base segment**, loaded from the index file (`segment.c: segment_open`):
  its columns and names are read in place from the read-only mapping of the
  file, so a command faults in only the pages its query touches; sizes and
  times the journal has replaced live in a small overlay beside them;
- **delta segments**, built in memory from a `SegmentBuilder` by the daemon,
  one per batch of filesystem changes (`segment_from_builder`).

Segments are reference-counted (`segment_retain`, `segment_release`).

**Snapshots.** A `Snapshot` (`index/index.h`) is a consistent read-only view:
the base segment, any delta segments after it, and a tombstone bitmap over
all ids. Ids run contiguously across segments; `snap_record`, `snap_name` and
`snap_lower` find the segment for an id. Snapshots are reference-counted and
never change once published.

**The index.** An `Index` holds the current snapshot behind a mutex.
Readers take a reference (`index_acquire`) and search it without locks;
the daemon replaces it (`index_publish`). A search that started on an old
snapshot finishes on it, mapping included.

## Building the index

`eind index` (`app/build.c: build_index`):

1. For each configured root, `fs/scanner.c: scan_tree` lists directories on
   the I/O thread pool (at most 6 threads). Each worker reads one directory,
   skipping excluded names, and returns a batch; the collecting thread appends
   the batch to one `SegmentBuilder`, so parents always precede children. A
   directory's names and metadata come from libmc's `dir_read`: on macOS from
   `getattrlistbulk` in bulk, elsewhere from `readdir` and `fstatat`.
2. `segment.c: segment_write` orders the records by lowercased path, stores
   every distinct name once in name order, and writes the columns of the index
   file (see [file-format.md](file-format.md)), together with an empty journal.
3. The file is loaded back with `index_load` and the snapshot returned.

Excludes (`fs/excludes.c`, on libmc's `glob_match`) are globs: a pattern without a slash matches names,
one with a slash matches the full path and everything below it; `**` spans
directories.

## Loading

`index.c: index_load` maps the file (`segment_open`), wraps it in a snapshot
and replays its journal (`journal.c: journal_replay`): the journal's added
entries become one delta segment, its removals tombstones. A command therefore
sees a running daemon's changes without talking to it.

## Searching

A query string is parsed into a `QueryNode` tree (`index/query.c`, allocated
in an arena): words are AND, `|` is OR, `!` is NOT, `<...>` groups, and a
word may carry modifiers (`case:`, `regex:`, `path:`, ...) and functions
(`ext:`, `size:`, `dm:`, ...). Relative dates are resolved against the time
of parsing.

`search.c: search_run` compiles the tree into `Matcher`s once (regexes are
compiled, needles lowercased, `parent:` and `infolder:` resolved to directory
ids), then scans every id of the snapshot in parallel chunks on the CPU thread
pool. Each worker skips tombstoned ids and tests the matcher tree against a
`MatchCtx`, which builds the record's full path only when a path matcher asks
for it. Workers check the cancellation flag every few thousand records.
The hits come back in id order.

Ordering:

- `search_top` sorts by path, name, size, dates or extension, selecting only
  the best `keep` hits when that is cheaper than sorting all;
- `search_rank` orders by relevance: names equal to a search term, then names
  starting with it, then containing it at a word boundary, then anywhere;
  ties go to shallower paths, then to name order.

Output (`app/output.c`) prints plain lines, NUL-separated lines, JSON or CSV.

## The daemon

`eind watch` and `eind serve` run `app/daemon.c: daemon_run`:

1. Load the index (or build it) and publish it.
2. Open libmc's watcher (`mc/platform/watch.h`) on the roots, skipping
   excluded directories: FSEvents on macOS watches whole trees; inotify on
   Linux needs one watch per directory, so libmc adds one for every directory
   below the roots and for each one that appears later.
3. Open the journal for the loaded file (`journal_open`), which cuts off a
   half-written last entry or starts a fresh journal.
4. Loop: `watch_read` waits for a change and keeps collecting until events
   have been quiet for 250 ms (at most 2 s), so a path touched many times is
   examined once. `updater_apply` re-examines each changed path on disk: what
   exists is added or refreshed, what is gone is tombstoned, a new directory
   is scanned in full. An event marked `rescan` (a directory moved in or out,
   a root that moved, events the system dropped) replaces the directory's
   whole subtree with a fresh scan. The batch becomes one delta segment and a new
   tombstone bitmap, published as a new snapshot. The differences between the
   old and new snapshot are appended to the journal at once (`journal_record`,
   `journal_flush`).
5. Every save interval (default 10 s), if the journal holds more than 10,000
   changes or 1% of the index, compact (`merge.c: index_compact`): merge all
   segments, drop tombstoned records and everything below them, write a new
   index file with an empty journal, load it, publish it and reset the updater
   and journal. Compact once more on exit.
6. If a flush finds that another process wrote a new index file (its journal
   header names another generation), load that file and continue from it.

SIGINT, SIGTERM and SIGHUP are blocked in every thread and taken by one
thread in `signals_wait`, which cancels the loop; the daemon then compacts
and exits.

The updater (`fs/updater.c`) finds records by (parent id, name) in a hash table
of ids, so a path is looked up by walking down from its root.

## The socket server

`eind serve` also starts `app/server.c`: an accept thread, one thread per
connection, and one thread per request. A request acquires the current
snapshot, so it never blocks the daemon. A new request on a connection
cancels the one still running there, which answers `{"cancelled":true}`.
The protocol is in [protocol.md](protocol.md).

## The interactive view

`app/tui.c` draws on `/dev/tty` in raw mode with ANSI escapes, through
libmc's `Terminal`. Each change to the input cancels the running search and
starts a new one on a thread of its own after a short typing pause; a
finished search queues itself and wakes `terminal_wait`, which also reports
window resizes. Enter prints the selected path to stdout.

## The login service

`eind service` (`main.c: cmd_service`) uses libmc's login services
(`mc/platform/service.h`) to write a launchd agent
(`~/Library/LaunchAgents/eind.plist`) or a systemd user unit
(`~/.config/systemd/user/eind.service`) that runs `eind serve`, and loads it.

## Memory

Everything lives in libmc arenas; nothing in `src/` calls `malloc`.

| Data | Lives in | Freed |
|---|---|---|
| parsed query, hits | the caller's `Arena` | with it |
| matchers, compiled regexes | the search's own arena | after the search |
| one chunk's hits and paths | an arena per chunk | merged into the hits, then freed |
| socket request, its JSON and response | per-request `Arena` | when the request finishes |
| scanned directory batch | a pooled read buffer's arena | released after the collector appends it; freed if it grew large |
| records being built | `SegmentBuilder`, an arena per array, replaced as it grows | handed to a `Segment` |
| segments, snapshots | an arena each, reference counted | when the last holder releases |
| base names and columns | the file mapping | with the base segment |
| a daemon batch's events, tombstones | per-batch arenas | when the next batch starts |

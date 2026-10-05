# eind compared with find and fd

`find` walks the filesystem and stats every entry it needs to test on each
run, so it costs the same whether the query is selective or not. `fd` walks
too, but in parallel and without needless stat calls, and by default it skips
hidden and gitignored paths. `eind` answers from its index. Measured on a
MacBook with 1.08 million files and 259,000 folders under two roots, warm
cache, best of several runs. `fd -HI` includes hidden and ignored files, so
that it sees the same tree as `find`.

| Query | find | fd -HI | fd default | eind |
|---|---|---|---|---|
| `eind server.go` | 167 s | 5.4 s | 2.8 s, 91 of 97 hits | 15 ms |
| `eind ext:pdf size:>1mb` | 437 s | 7.8 s | 2.8 s, 125 of 127 hits | 13 ms |
| `eind ext:go dm:last7days` | 259 s | 8.7 s | 6.0 s | 16 ms |
| `eind folder:wfn:src` | 92 s | 6.0 s | 5.7 s, 547 of 2,338 hits | 15 ms |
| `eind --count a` (817,000 hits) | 64 s | 6.1 s | 5.9 s | 22 ms |

The `eind` column is a whole command line invocation without a daemon:
process start, mapping the 48 MB index, searching and printing. Process start
is about 6 ms of it, and the search itself 2 to 5 ms. With `eind serve`
running the times are much the same, since loading the index costs next to
nothing. Listing all 817,000 hits for `a` takes 0.2 s. Building the index
from scratch took 8.6 s and at most 320 MB of memory.

`find` is slowest where it has to stat every file, for sizes and dates, and
its times vary by minutes between runs. `fd` is 10 to 60 times faster, and
`eind` some 300 times faster again.

The price is freshness: `find` and `fd` are always exact, while `eind` is as
current as its watcher, which is within a fraction of a second while `eind
serve` runs.

The hit counts depend on what each tool skips. `fd`'s defaults hide hidden
and gitignored paths, which is right inside a project and wrong for a
whole-disk "where did that file go" search: they hid 6 of the 97 `server.go`
files and 3 of every 4 `src` folders. `eind` skips what its config excludes,
by default `.git`, `node_modules`, caches and the trash, so it finds 91
`server.go` files and 1,113 `src` folders. Inside one repository `fd` is the
better tool: it finishes in tens of milliseconds, respects the ignore rules,
and needs no index.

`find` and `fd` need no setup, search any path, and can act on what they find
with `-exec`, `-delete` and `-x`. `eind` needs an index and a list of roots,
and only finds; pipe `eind -0` into `xargs -0` to act on results. `find`
filters by permissions and owner, `fd` by owner, symlink and executable type,
empty files and ignore rules; `eind` does none of that. `eind` combines AND,
OR, NOT and grouping in one query, takes dates and sizes as words
(`dm:2024-03`, `size:1mb..5mb`), filters by name length and depth, ranks by
relevance, and matches case-insensitive substrings by default, where `find`
needs `-iname '*server.go*'` and `fd` a regex.

## The daemon

Every `eind` command maps the index file and reads it straight from the page
cache, so searches are fast with or without a daemon. What `eind serve` adds
is freshness: it watches the roots and appends each change to a journal next
to the index, which every command reads along with it, and folds the journal
into a new index file once it grows long. While idle the daemon uses about
21 MB of memory.

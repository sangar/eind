# eind compared with find and fd

`find` walks the filesystem and stats every entry on each run, so it costs the
same whether the query is selective or not. `fd` walks too, but in parallel
and without needless stat calls, and by default it skips hidden and
gitignored paths. `eind` answers from its index. Measured on a MacBook with
2.07 million files and 378,000 folders under two roots, warm cache, best of
several runs, with `eind serve` running as an installation would have it.
`fd -HI` includes hidden and ignored files so that all three see the same
tree.

| Query | find | fd -HI | fd default | eind |
|---|---|---|---|---|
| `eind server.go` | 52 s | 7.5 s | 2.3 s, 2 of 8 hits | 23 ms |
| `eind ext:pdf size:>1mb` | 52 s | 6.6 s | 5.5 s | 20 ms |
| `eind ext:go dm:last7days` | 52 s | 7.0 s | 5.9 s | 21 ms |
| `eind folder:wfn:node_modules` | 51 s | 4.7 s | 2.3 s, 416 of 1,553 hits | 17 ms |
| `eind --count a` (1.4 million hits) | 52 s | 6.6 s | 5.9 s, 627 k hits | 16 ms |

The `eind` column is a whole command line invocation: process start, a
round trip to the daemon and printing. The search itself takes 2 to 5 ms; the
daemon spends most of the last query ranking 1.4 million hits when asked for
a listing rather than a count. Without a daemon each command spends about
110 ms loading the 82 MB index first. Building the index from scratch took
15 s.

The price is freshness: `find` and `fd` are always exact, while `eind` is as
current as its watcher. In the last query `eind` counted 86 stale entries out
of 1.4 million, files deleted since the index was last updated.

`fd`'s defaults change the answer, not just the time: they hid 6 of the 8
`server.go` files and 3 of every 4 `node_modules` folders because those sit
under hidden or gitignored paths. That is right inside a project and wrong
for a whole-disk "where did that file go" search. Inside one repository `fd`
is the better tool: it finishes in tens of milliseconds, respects the ignore
rules, and needs no index.

`find` and `fd` need no setup, search any path, and can act on what they find
with `-exec`, `-delete` and `-x`. `eind` needs an index and a list of roots,
and only finds; pipe `eind -0` into `xargs -0` to act on results. `find`
filters by permissions and owner, `fd` by owner, symlink and executable type,
empty files and ignore rules; `eind` does none of that. `eind` combines AND,
OR, NOT and grouping in one query, takes dates and sizes as words
(`dm:2024-03`, `size:1mb..5mb`), filters by name length and depth, ranks by
relevance, and matches case-insensitive substrings by default, where `find`
needs `-iname '*server.go*'` and `fd` a regex.

## Without the daemon

With `eind serve` running, the index stays current and every search, from
the command line, the interactive view or a GUI, is answered from memory in a
few milliseconds. Without it each `eind` command loads the index from disk
first, about 110 ms for two million files.

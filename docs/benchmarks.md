# Benchmarks

Measure before and after any change meant to make eind faster or smaller, on
the same tree and machine, and compare medians.

```sh
make && tools/bench.py ~/some/tree            # wall time, peak RSS, instructions (macOS)
tools/bench.py ~/some/tree --runs 50          # more runs for small differences
```

`tools/bench.py` indexes the tree into a temporary directory with
`EIND_CONFIG`, `EIND_INDEX` and `EIND_SOCKET` pointing there, so it never
touches the user's index or a running daemon. Always isolate manual
measurements the same way: a command that finds a daemon on the default
socket, or the user's journal on the default index, measures something else.

Indexing time varies by 30% or more between runs with filesystem and kernel
state; compare medians of several runs, interleaved when comparing two
binaries. Differences below about 1 ms in a search are noise.

## Baseline

October 2026, Apple M-series, macOS, warm cache, `~/Developer` with 155,816
entries, `make` defaults (`-O2 -g`). Times include process start and about
1.5 ms of `/usr/bin/time` overhead.

| Workload | Median | Peak RSS | Instructions |
|---|---|---|---|
| `eind index` | 372 ms | 51 MB | 12.9 G |
| `eind --version` (startup) | 4.8 ms | 5.6 MB | 27 M |
| `-n 50 -s relevance main` | 6.6 ms | 19.6 MB | 80 M |
| `-n 50 ext:json size:>10kb` | 6.7 ms | 19.6 MB | 83 M |
| `-n 50 *.go` | 6.5 ms | 19.6 MB | 69 M |
| `-n 50 regex:^[a-z]+_test\.go$` | 17.1 ms | 19.8 MB | 1,054 M |
| `-n 50 path:src/main` | 11.1 ms | 19.7 MB | 512 M |
| `--count e` | 7.6 ms | 20.7 MB | 77 M |

Other measurements at the same time:

| | |
|---|---|
| Lines of code (non-blank, non-comment) | 6,336 in `src`, 441 in `tests` |
| Clean build (`make clean && make`) | 1.9 s, 88 MB compiler peak RAM |
| Incremental build (one function added to `search.c`) | 0.23 s |
| Binary | 192 KB, 161 KB stripped |
| Heap allocations (`malloc` family) | 412k for `eind index`, 958 for one search |
| Indexing throughput | about 420k entries/s |

Allocations were counted with a small `DYLD_INSERT_LIBRARIES` library that
interposes `malloc`, `calloc`, `realloc`, `posix_memalign` and
`aligned_alloc` and prints the count at exit.

## The four implementations

eind was written four times: in Go (this repository's `go-final` tag), Rust,
Zig and C. All four share the index format, the socket protocol and the
command line, so `tools/bench.py --binary` runs the same measurement against
each. October 2026, the same Apple M-series machine and warm cache,
`~/Developer` with 160,647 entries, medians of 30 runs (index: 5). Go 1.26.5,
rustc 1.99.0, Zig 0.17.0 in ReleaseFast, Apple clang 21 at `-O2`.

| Workload | C | Go | Rust | Zig |
|---|---|---|---|---|
| `eind index` | 420 ms | 477 ms | 403 ms | 346 ms |
| startup (`--version`) | 4.7 ms | 5.7 ms | 5.3 ms | 4.6 ms |
| `-n 50 -s relevance main` | 7.3 ms | 7.4 ms | 6.4 ms | 5.7 ms |
| `-n 50 ext:json size:>10kb` | 7.0 ms | 6.9 ms | 6.3 ms | 5.6 ms |
| `-n 50 *.go` | 6.7 ms | 7.2 ms | 6.0 ms | 5.5 ms |
| `-n 50 regex:^[a-z]+_test\.go$` | 17.3 ms | 8.5 ms | 7.3 ms | 11.9 ms |
| `-n 50 path:src/main` | 11.3 ms | 22.3 ms | 14.5 ms | 10.8 ms |
| `--count e` | 7.2 ms | 8.7 ms | 6.4 ms | 6.4 ms |
| Peak RSS, a search | 20 MB | 16 MB | 11 MB | 14 MB |
| Peak RSS, `eind index` | 61 MB | 55 MB | 53 MB | 42 MB |
| Binary | 165 KB | 5.9 MB | 3.4 MB | 536 KB |

Instructions retired tell the same story more precisely: a plain search is
60 to 85 million in every version, so the differences of a millisecond or
two are process start and page-cache mapping, not the scan. The outliers are
regexes, where the C version's POSIX `regexec` costs 1,100 million
instructions against 120 million for Rust's engine, and `path:` queries,
which rebuild every path, where Go takes twice the C time. The C version's
search RSS is the highest of the four; about 7 MB of it is the record array
and lowercase name table it decodes from the mapping on load.

## Where the time goes

- **Indexing** is dominated by the kernel listing directories; 6 I/O threads
  is the measured optimum on APFS (more threads contend on filesystem locks).
- **Searches** scan every record on all cores; the base cost of a search is
  process start, mapping the file and decoding the records (about 2 ms here).
- **Regex** queries spend most of their time in `regexec`, about 3x the
  instructions of RE2-style engines.
- **`path:`** queries build each record's full path, which costs more than
  name matching.

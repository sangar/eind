# Benchmarks

Measure before and after any change meant to make eind faster or smaller, on
the same tree and machine, and compare medians.

```sh
make && tools/bench.py ~/some/tree            # wall time, peak RSS, instructions (macOS)
tools/bench.py ~/some/tree --runs 50          # more runs for small differences
tools/bench.py ~/some/tree --binary ../../rust/eind/target/release/eind   # another implementation
```

`tools/bench.py` indexes the tree into a temporary directory with
`EIND_CONFIG`, `EIND_INDEX` and `EIND_SOCKET` pointing there, so it never
touches the user's index or a running daemon. Always isolate manual
measurements the same way: a command that finds a daemon on the default
socket, or the user's journal on the default index, measures something else.

Indexing time varies by 30% or more between runs with filesystem and kernel
state; compare medians of several runs, interleaved when comparing two
binaries. Differences below about 1 ms in a search are noise. Only measure
release builds: the debug builds below are up to eight times slower on some
queries, and the ratio differs by query, so a debug measurement says nothing
about a release one.

## Baseline

October 7, 2026, Apple M3 Pro (6 performance and 6 efficiency cores, 18 GB),
macOS 26.6, Apple clang 21, warm cache, `~/Developer` with 160,656 entries,
`make` defaults (`-O2 -g`). Times include process start and about 1.5 ms of
`/usr/bin/time` overhead.

| Workload | Median | Peak RSS | Instructions |
|---|---|---|---|
| `eind index` | 376 ms | 51 MB | 13.4 G |
| `eind --version` (startup) | 4.7 ms | 5.6 MB | 27 M |
| `-n 50 -s relevance main` | 5.9 ms | 10.4 MB | 87 M |
| `-n 50 ext:json size:>10kb` | 5.7 ms | 10.3 MB | 78 M |
| `-n 50 *.go` | 5.4 ms | 9.6 MB | 54 M |
| `-n 50 regex:^[a-z]+_test\.go$` | 15.8 ms | 9.9 MB | 1,093 M |
| `-n 50 path:src/main` | 11.1 ms | 9.7 MB | 535 M |
| `--count e` | 6.2 ms | 10.7 MB | 79 M |

Other measurements at the same time:

| | |
|---|---|
| Lines of code (non-blank, non-comment) | 6,653 in `src`, 583 in `tests` |
| Clean build (`make clean && make`) | 3.2 s, 84 MB compiler peak RAM |
| Incremental build (one function added to `search.c`) | 0.28 s |
| Binary | 166 KB, 140 KB stripped |
| Index file | 5.9 MB |
| Heap allocations (`malloc` family) | 77k for `eind index` |
| Indexing throughput | about 430k entries/s |

Allocations were counted with a small `DYLD_INSERT_LIBRARIES` library that
interposes `malloc`, `calloc`, `realloc`, `posix_memalign` and
`aligned_alloc` and prints the count at exit.

Before the October 7 changes (reading the base segment in place instead of
decoding it on load, folding case while comparing instead of copying names,
reusing the scanner's buffers; see [style-review.md](style-review.md)) a
search took 6.8 to 7.4 ms and 20 MB, `eind index` 382 ms, 54 MB and 412k
allocations, and the idle daemon 22 MB.

## The four implementations

eind was written four times: in Go (this repository's `go-final` tag), Rust,
Zig and C. All four share the index format, the socket protocol and the
command line, so `tools/bench.py --binary` runs the same measurement against
each, and every query and sort order tried gives identical output. The
index file they write of `~/Developer` is the same 5.9 MB.

October 7, 2026, the machine above, warm cache, `~/Developer` with 160,656
entries, medians of 30 runs (index: 5). Release builds: Apple clang 21 at
`-O2`, Go 1.26.5 with `-ldflags '-s -w'`, rustc 1.99.0 with `--release`,
Zig 0.17.0 with `-Doptimize=ReleaseFast`. The C column was measured after
the other three, with a Zig rerun as a control that matched its earlier
numbers within 0.5 ms.

| Workload | C | Go | Rust | Zig |
|---|---|---|---|---|
| `eind index` | 376 ms | 487 ms | 411 ms | 354 ms |
| startup (`--version`) | 4.7 ms | 5.8 ms | 4.9 ms | 4.8 ms |
| `-n 50 -s relevance main` | 5.9 ms | 6.8 ms | 6.0 ms | 5.8 ms |
| `-n 50 ext:json size:>10kb` | 5.7 ms | 6.8 ms | 6.2 ms | 5.8 ms |
| `-n 50 *.go` | 5.4 ms | 6.7 ms | 6.0 ms | 5.7 ms |
| `-n 50 regex:^[a-z]+_test\.go$` | 15.8 ms | 8.4 ms | 7.0 ms | 12.2 ms |
| `-n 50 path:src/main` | 11.1 ms | 21.8 ms | 14.7 ms | 10.7 ms |
| `--count e` | 6.2 ms | 8.4 ms | 7.4 ms | 6.3 ms |
| Peak RSS, a search | 10 MB | 16 MB | 11 MB | 14 MB |
| Peak RSS, `eind index` | 51 MB | 56 MB | 53 MB | 42 MB |
| Binary | 166 KB | 3.9 MB | 3.4 MB | 536 KB |

Instructions retired tell the same story more precisely: a plain search is
54 to 87 million in every version, so the differences of a millisecond or
two are process start and page-cache mapping, not the scan. Indexing is
13 to 14 billion in C and Zig, 16 in Rust and 22 in Go. The outliers are
regexes, where the C version's POSIX `regexec` costs 1,100 million
instructions against 740 million for Zig's (the same POSIX engine),
220 million for Go's and 120 million for Rust's; and `path:` queries, which
rebuild every path, where Go spends 1,100 million instructions against
about 500 million in the others. The C version now has the lowest search
memory of the four, 10 MB against 20 MB before it read the base segment
in place.

### Size, dependencies, build and memory

Same day and machine. Lines are non-blank, non-comment, with test files
counted separately. A cold build starts with an empty compiler cache (`make
clean`, a fresh `GOCACHE`, a fresh Cargo target directory, fresh Zig cache
directories), so it includes compiling every dependency and, for Zig, the
standard library; downloaded sources are already present. An incremental
build follows a one-line change to the search module. Compiler peak is the
largest resident set of the build process and its children. Daemon memory
is `eind serve` idle three seconds after start with the same index of
`~/Developer`.

| | C | Go | Rust | Zig |
|---|---|---|---|---|
| Source files (and test files) | 50 (1) | 29 (11) | 25 (3) | 16 (3) |
| Lines of code | 6,653 | 4,657 | 5,839 | 5,786 |
| Lines of tests | 583 | 1,456 | 810 | 779 |
| Dependencies | none | 4 direct, 9 modules | 11 direct, 59 crates built (105 in the lock file) | none |
| Cold build | 3.2 s | 4.2 s | 8.0 s | 47.9 s |
| Compiler peak, cold | 84 MB | 315 MB | 370 MB | 420 MB |
| Incremental build | 0.28 s | 0.47 s | 2.4 s | 6.6 s |
| Compiler peak, incremental | 66 MB | 101 MB | 351 MB | 360 MB |
| Binary | 166 KB | 3.9 MB | 3.4 MB | 536 KB |
| Binary, stripped | 140 KB | 3.9 MB | 2.7 MB | 473 KB |
| `eind serve` idle | 14 MB | 16 MB | 14 MB | 10 MB |
| Peak RSS, a search | 10 MB | 16 MB | 11 MB | 14 MB |
| Peak RSS, `eind index` | 51 MB | 56 MB | 53 MB | 42 MB |

The Go binary is built with `-s -w`, which is what `make build` does; a
plain `go build` keeps the symbol table and DWARF and is 5.8 MB. The Go line
count leaves out the Swift client, which has no counterpart elsewhere. Zig's
cold build is dominated by compiling the standard library, and a one-line
change costs a whole-program recompile.

An earlier four-way timing table, measured on October 6 before the C
version's last changes, is in the Zig implementation's README.

### Debug and release builds

Each toolchain has a build for development and one for shipping:

| | Debug | Release |
|---|---|---|
| C | `CFLAGS="-O0 -g" make` | `make` (`-O2 -g`) |
| Go | `go build -gcflags='all=-N -l'` (no inlining, no register allocation, DWARF kept) | `make build` (`-ldflags '-s -w'`) |
| Rust | `cargo build` (opt-level 0, debug assertions, overflow checks) | `cargo build --release` |
| Zig | `zig build` (Debug: no optimisation, every safety check on) | `zig build -Doptimize=ReleaseFast` |

Go's normal build is already optimised, so its debug build only turns
optimisations off for the debugger. Same day, machine and tree; each cell
is debug / release.

| | C | Go | Rust | Zig |
|---|---|---|---|---|
| Cold build | 2.0 / 3.2 s | 4.1 / 4.2 s | 5.8 / 8.0 s | 43.1 / 47.9 s |
| Incremental build | 0.19 / 0.28 s | 0.74 / 0.47 s | 0.68 / 2.4 s | 2.1 / 6.6 s |
| Compiler peak, cold | 73 / 84 MB | 218 / 315 MB | 295 / 370 MB | 421 / 420 MB |
| Binary | 259 KB / 166 KB | 6.3 / 3.9 MB | 11.2 / 3.4 MB | 3.4 MB / 536 KB |
| `eind index` | 441 / 376 ms | 770 / 487 ms | 1,045 / 411 ms | 696 / 354 ms |
| `-n 50 -s relevance main` | 8.1 / 5.9 ms | 8.2 / 6.8 ms | 10.6 / 6.0 ms | 9.1 / 5.8 ms |
| `-n 50 regex:^[a-z]+_test\.go$` | 16.4 / 15.8 ms | 12.1 / 8.4 ms | 17.5 / 7.0 ms | 17.3 / 12.2 ms |
| `-n 50 path:src/main` | 21.5 / 11.1 ms | 38.8 / 21.8 ms | 42.3 / 14.7 ms | 81.7 / 10.7 ms |
| `eind serve` idle | 14 / 14 MB | 17 / 16 MB | 16 / 14 MB | 11 / 10 MB |

Debug searches are 1.2x (Go) to 1.8x (Rust) slower on a plain query and up
to 7.6x (Zig `path:`) on the ones that do the most work per record; the
unoptimised C scan runs about twice the instructions. Memory is the same in
both builds: it is the index, not the code. The debug build is the one to
iterate on in Rust and Zig, where the release incremental build is three
times slower; in C and Go the two cost the same and the release build is
the default.

### Testing

Same day and machine. Run time is for a test suite whose binaries are
already built; the first run after a cold build also compiles them. Each
suite builds a small tree in a temporary directory, indexes it and queries
it, and the Go, Rust and Zig suites also run the built binary end to end.

| | C | Go | Rust | Zig |
|---|---|---|---|---|
| Command | `make test` | `make test` | `cargo test` | `zig build test` |
| Tests | 137 checks in one binary | 53 tests (59 with subtests) | 62 (50 unit, 3 in the binary, 9 end to end) | 66 (57 unit, 9 end to end) |
| Run time | 0.3 s (1.3 s with the relink) | 3.8 s | 1.1 s | 1.3 s |
| First run after a cold build | 1.3 s | 3.8 s | 5.2 s | 5.4 s |
| What `make test` adds | nothing | `go vet`, staticcheck, `-race`: 4.0 s in all | | |
| Extra checks | `make sanitize`: ASan and UBSan, 4.5 s including the rebuild | race detector (0.4 s extra) | debug assertions and overflow checks | bounds, overflow and undefined-behaviour checks |
| Lines of tests | 583 | 1,456 | 810 | 779 |

All four suites passed with no failures. `zig build test` caches test
results: a second run with no change takes 0.2 s and runs nothing, so the
1.3 s is the two test binaries run by hand. Rust and Zig tests run in the
debug build, where the checks above are on; the C sanitizer run is a
separate `-O1` build, and Go's race detector is a separate build too. The
C suite does not start the binary; the end-to-end checks drive the library
from the test binary.

## Where the time goes

- **Indexing** is dominated by the kernel listing directories; 6 I/O threads
  is the measured optimum on APFS (more threads contend on filesystem locks).
- **Searches** scan every record on all cores; the base cost of a search is
  process start and mapping the file (about 1 ms over `--version` here); the base segment
  is read in place, so a query only faults in the pages it touches.
- **Regex** queries spend most of their time in `regexec`, five to nine times
  the instructions of the RE2-style engines in Go and Rust.
- **`path:`** queries build each record's full path, which costs more than
  name matching.

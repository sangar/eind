# eind

Instant file search for the terminal and for launchers, written in C.

`eind` indexes the names of every file and folder under your roots once, then
answers searches from that index in milliseconds, with a small query
language: `report ext:pdf size:>1mb dm:thisweek !draft`. A daemon keeps the
index current from filesystem events and answers launchers over a Unix
socket.

Runs on macOS and Linux. Building needs only a C23 compiler: its one
dependency, [libmc](https://github.com/sangar/libmc), is vendored in
`deps/libmc`, and the build program is C too.

## Profile

| | |
|---|---|
| Profile | Modern C Level 2 |
| Language | C23 |
| Compilers | clang 18 or newer (primary), gcc 14 or newer |
| Targets | Linux x86_64 and aarch64, macOS arm64 and x86_64 |
| Build | `tools/build.c`, a build program in C (see below) |
| Dependencies | [libmc](https://github.com/sangar/libmc), vendored in `deps/libmc` and pinned in `deps.lock` |

Everything shared comes from libmc: arenas, `String`, the `Error` codes,
containers, JSON, the thread pool and the platform layer, including file
watching, the login service, Unix sockets and the terminal. eind's own
platform code is the default excludes in `src/platform/defaults_*.c`.
Memory lives in arenas, fallible functions return `Error` with an `Err`
message, and there is no global mutable state.

The build runs with `-Werror` and `-Wall -Wextra -Wshadow -Wconversion -Wvla
-Wstrict-prototypes -Wimplicit-fallthrough`; `./nob test` runs the tests
under AddressSanitizer and UndefinedBehaviorSanitizer, and CI runs them with
clang 18 and gcc 14 on Linux x86_64 and aarch64 and with clang on macOS. The
only approved extension is `[[gnu::format]]`.

`./nob cross` builds the Linux binaries for both architectures with the zig
pinned in `mise.toml`, and compiles the macOS one; linking it needs the
CoreServices framework from the macOS SDK, so macOS binaries are built on a
Mac. Windows is not a target: libmc has no Windows platform layer.

Deliberate departures from the profile: searches use the C library's POSIX
regular expressions (`<regex.h>`), which macOS and Linux both provide, since
libmc has no regex engine; dates use `localtime_r` and `mktime` from
`<time.h>`; and the tests' `CHECK` macros are function-like so a failure can
name its line.

## Build

The build program is C and rebuilds itself when it changes. Bootstrap it
once:

```sh
cc tools/build.c -o nob
```

```sh
./nob             # ./eind, -O2 -g
./nob test        # the tests under AddressSanitizer and UndefinedBehaviorSanitizer
./nob check       # the modern-c contract check, then the tests
./nob cross       # out/<target>/eind for every target, with zig
./nob clean
VERSION=1.2.3 ./nob   # what `eind version` prints (default: git describe or "dev")
```

## Install

```sh
./nob install      # ~/.local/bin/eind, and eind serve at login
./nob uninstall    # stop the service and remove the binary
```

`./nob install` builds and copies the binary to `~/.local/bin`, which must be
on your `PATH`, and enables the login service (`eind service enable`). Run it
again after pulling to upgrade; the service restarts on the new binary.
Install elsewhere with `PREFIX=/usr/local ./nob install`, and uninstall with
the same `PREFIX`.

`./nob uninstall` keeps the index and config; `eind status` shows where they
are, so you can delete them too. On macOS the service logs to
`~/Library/Logs/eind.log`.

On Linux, raise the inotify limit once, since the watcher registers every
indexed directory and the default of 8192 watches is too low for a home
directory. Without enough watches, `eind watch` and `eind serve` stop with an
error that names the limit:

```sh
sudo cp packaging/50-eind.conf /etc/sysctl.d/ && sudo sysctl --system
```

A small SwiftUI client of the daemon for macOS lives in [macos/](macos/README.md).
Coming from the Go version? See [docs/migration.md](docs/migration.md).

## Quick start

```sh
eind index                    # index your home directory
eind invoice 2024             # names containing "invoice" and "2024"
eind ext:go 'size:>100kb'     # large Go files
eind "*.psd" dm:lastmonth     # wildcards match the whole name
eind                          # interactive view; Enter prints the chosen path
cd "$(eind folder: project)"  # use it from the shell
```

Searching with no index builds one first.

## Query syntax

| Syntax | Meaning |
|---|---|
| `a b` | both terms match (AND); terms match anywhere in the name, ignoring case |
| `a\|b` | either term (OR) |
| `!a` | not |
| `<a b\|c>` | grouping |
| `"two words"` | protect spaces |
| `*.txt`, `IMG_????.JPG` | wildcards, matched against the whole name |
| `ext:pdf;docx` | extension is one of the list |
| `size:>10mb`, `size:1mb..5mb`, `size:large` | size; units kb mb gb tb, names empty tiny small medium large huge gigantic |
| `dm:today`, `dm:lastweek`, `dm:2024-03`, `dm:>2023`, `dm:last30days` | date modified |
| `dc:...` | date created |
| `len:>40`, `depth:3` | name length, path depth |
| `file:`, `folder:` | only files, only folders (`folder:src` = folders named src) |
| `path:src/main` | match against the full path |
| `parent:~/Documents` | direct children of a folder |
| `infolder:~/Projects` | anything below a folder |
| `case:Readme` | match case |
| `regex:^draft_\d+` | regular expression (POSIX extended, plus `\d` `\w` `\s`) |
| `ww:log` | whole word (underscores separate words) |
| `wfn:Makefile` | whole file name |

Modifiers can be chained: `folder:case:regex:^Src$`. Ranges accept `>`, `>=`,
`<`, `<=`, `=` and `a..b`. Dates may be `today`, `yesterday`, `thisweek`,
`lastweek`, `thismonth`, `lastmonth`, `thisyear`, `lastyear`,
`last<N>days`/`weeks`/`months`/`years`, `YYYY`, `YYYY-MM` or `YYYY-MM-DD`.

## Commands

```
eind [options] [query...]     search; with no query, open the interactive view
eind index [--root DIR]...    build the index from the configured roots
eind watch                    keep the index up to date from filesystem events
eind serve                    watch, and answer queries over a Unix socket
eind service enable|disable   run eind serve at login (launchd agent or systemd user unit)
eind status                   show where the index and config live, and their size
eind config [--init]          show the effective config, or write a default file
eind config edit              open the config in $VISUAL or $EDITOR, then check it
eind tui                      open the interactive view
eind version                  show the version
```

Search options: `-r` regex, `-i` match case, `-w` whole words, `-p` match the
full path, `-n N` at most N results, `-o N` skip N, `-s KEY` sort by `path`
(default), `name`, `size`, `dm`, `dc`, `ext` or `relevance`, `-d` descending,
`--path DIR`, `--files`, `--dirs`.

Output options: `--json`, `--csv`, `-0` (NUL-separated), `--name-only`,
`--size`, `--dm`, `--dc`, `--count`, `--color auto|always|never`.

Options may appear anywhere on the line; everything after `--` is query text.
`eind --help` lists them all.

## Files

| | Default | Override |
|---|---|---|
| config | `~/.config/eind/config` | `--config`, `$EIND_CONFIG` |
| index | `~/.local/share/eind/index.bin` (and `index.bin.journal`) | `--index`, `$EIND_INDEX` |
| socket | `$XDG_RUNTIME_DIR/eind.sock`, else `$TMPDIR/eind-<uid>.sock` | `--socket`, `$EIND_SOCKET` |

`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME` move the config and index
directories.

The config file is a list of `key = value` lines:

```
root = ~
exclude = node_modules
exclude = ~/Library/Caches
exclude = **/build
```

`root` may repeat. An `exclude` without a slash matches names; one with a
slash matches the full path and everything below it; `**` spans folders. A
file without any `exclude` lines uses the defaults: `node_modules`, `.git`,
`.cache`, the platform's cache directory and trash, and its virtual
filesystems (`/proc`, `/sys`, `/dev`, `/run` on Linux; `/dev`, `/Volumes`,
`/System/Volumes`, `/private/var/vm` and the sandboxed app data in
`~/Library/Containers` on macOS). `eind config --init` writes them out for
editing. Run `eind index` after changing the config.

## The daemon

`eind serve` (or `eind watch`, without the socket) loads the index, watches
every root (FSEvents on macOS, inotify on Linux), appends each change to the
journal next to the index file, and folds the journal into a new index file
when it grows large. Every command reads the journal, so searches see changes
at once. `eind service enable` installs it as a login service.

Launchers talk to it over the socket in JSON lines; see
[docs/protocol.md](docs/protocol.md).

## Limitations

- Case-insensitive matching folds ASCII only; `É` and `é` are different letters.
- Regular expressions are POSIX extended with `\d`, `\w` and `\s` added;
  non-greedy matching and lookaround are not supported.
- A folder's own modified time is not refreshed by the watcher; changes below
  it are.
- Windows and FreeBSD are not supported.

## Documentation

- [AGENTS.md](AGENTS.md): how to work on the code (start here)
- [docs/architecture.md](docs/architecture.md): how it works
- [docs/file-format.md](docs/file-format.md): the index file and journal
- [docs/protocol.md](docs/protocol.md): the socket protocol
- [docs/benchmarks.md](docs/benchmarks.md): how to measure, and the baseline

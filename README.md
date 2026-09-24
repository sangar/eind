# eind

Instant file search for the terminal.

`eind` indexes the names of every file and folder under your chosen roots once,
then answers searches from that index in a few milliseconds, with a simple query
language, example: `report ext:pdf size:>1mb dm:thisweek !draft`.
A watcher keeps the index current from filesystem events, and `eind` on its own
opens an interactive search-as-you-type view.

Linux and macOS are the primary targets. It also builds for FreeBSD and
Windows.

## Install

macOS, with Homebrew:

```sh
brew install OWNER/tap/eind
eind service enable           # run eind serve from login
```

Debian, Ubuntu, Fedora, Alpine and Arch: install the `.deb`, `.rpm`, `.apk`
or `.pkg.tar.zst` from the releases page with your package manager. The
package ships a systemd user unit that is enabled for every user and starts
at their next login; `systemctl --user start eind` starts it right away. It
also raises the inotify watch limit that a home directory needs.

From source, with Go 1.26 or newer (no C dependencies, `CGO_ENABLED=0` works):

```sh
git clone <this repository> eind && cd eind
go install .                  # puts eind in $(go env GOPATH)/bin
eind service enable
```

With `eind serve` running, the index stays current and every search, from
the command line, the interactive view or a GUI, is answered from memory in a
few milliseconds. Without it each `eind` command loads the index from disk
first, about 110 ms for two million files.

## Quick start

```sh
eind index                    # index your home directory (default root)
eind invoice 2024             # every name containing "invoice" AND "2024"
eind ext:go size:>100kb       # large Go files
eind "*.psd" dm:lastmonth     # wildcards match the whole name
eind                          # interactive view; Enter prints the chosen path
cd "$(eind folder: project)"  # use it from the shell
```

Searching with no index builds one first. Rebuild any time with `eind index`.

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
| `dc:...` | date created (macOS, BSD, Windows) |
| `len:>40`, `depth:3` | name length, path depth |
| `file:`, `folder:` | only files, only folders (`folder:src` = folders named src) |
| `path:src/main` | match against the full path |
| `parent:~/Documents` | direct children of a folder |
| `infolder:~/Projects` | anything below a folder |
| `case:Readme` | match case |
| `regex:^draft_\d+` | regular expression |
| `ww:log` | whole word (underscores separate words) |
| `wfn:Makefile` | whole file name |

Modifiers can be chained: `folder:case:regex:^Src$`.

## Command line

```
eind [options] [query...]     search; with no query, open the interactive view
eind index [--root DIR]...    build the index from the configured roots
eind watch                    keep the index up to date from filesystem events
eind serve                    watch, and answer queries over a Unix socket (for GUIs)
eind status                   show where the index and config live, and their size
eind config [--init]          show the effective config, or write a default file
eind tui                      open the interactive view
```

Search options: `-r` regex, `-i` match case,
`-w` whole word, `-p` match path, `-n N` max results, `-o N` offset,
`-s KEY` sort by `path` (default), `name`, `size`, `dm`, `dc`, `ext` or `relevance`,
`-d` descending, `--path DIR`, `--files`, `--dirs`.

Output options: `--json`, `--csv`, `-0` (NUL separated, for `xargs -0`),
`--name-only`, `--size`, `--dm`, `--dc`, `--count`, `--color always|never`.

Options may appear anywhere on the line. Use `--` when a search word collides
with a subcommand name: `eind -- index`.

When `eind serve` is running and serves the same index file, searches are
answered by it instead of loading the index from disk. Listings of more than
100,000 results still come from the local index, because they are cheaper to
produce there than to transfer as JSON.

Run `eind --help` for the full list.

## Daemon and socket API

`eind serve` is what a GUI or launcher should talk to. It loads the index once,
keeps it fresh from filesystem events exactly like `eind watch`, and answers
queries from memory over a Unix socket in a few milliseconds. On a 2.2 million
entry index a typical query round-trips in about 10 ms.

```sh
eind serve                          # socket at $XDG_RUNTIME_DIR/eind.sock
eind serve --socket /run/user/1000/eind.sock
eind status                         # shows whether a daemon is running
```

The default socket is `$XDG_RUNTIME_DIR/eind.sock`, falling back to
`eind-<uid>.sock` in the temp directory; `EIND_SOCKET` or `--socket` override
it. The socket is created with mode 0600. Unix socket paths are limited to
about 100 bytes.

The protocol is JSON lines: send one JSON object per line, receive one JSON
object per request. Any `id` you send is echoed back unchanged.

Search request, all fields except `query` optional:

```json
{"id": 1, "query": "report ext:pdf", "limit": 50, "offset": 0,
 "sort": "relevance", "descending": false,
 "regex": false, "case": false, "whole_word": false, "match_path": false,
 "path": "/home/me/Documents", "files": false, "dirs": false}
```

`limit` defaults to 100; `-1` returns everything and `0` returns only the
`total`, without sorting. `path` restricts results to one folder and its
descendants; `files` and `dirs` keep only files or only folders. `sort` is `relevance`
(default), `path`, `name`, `size`, `dm`, `dc` or `ext`. Relevance puts names
equal to a search term first, then names starting with it, then names
containing it at a word boundary, then plain substring matches, with shallower
paths winning ties; it is also available on the command line as
`-s relevance`.

Search response:

```json
{"id": 1, "total": 132, "elapsed_ms": 2.1, "results": [
  {"path": "/home/me/x/report.pdf", "name": "report.pdf", "type": "file",
   "size": 1024, "modified": "2024-03-13T10:00:00+01:00",
   "created": "2024-03-01T08:00:00+01:00"}
]}
```

`total` is the number of matches before `offset` and `limit`; `type` is
`file` or `dir`; `created` is omitted where the platform does not report it.

Sending a new request on a connection cancels the one still running, which
answers `{"id": 1, "cancelled": true}`. Send a request per keystroke and
render whichever response arrives with the latest id.

Status request and response:

```json
{"op": "status"}
{"files": 1847170, "folders": 330914, "roots": ["/home/me"],
 "built": "2024-03-13T09:00:00+01:00", "index": "/home/me/.local/share/eind/index.bin"}
```

Errors come back as `{"id": 1, "error": "size: expected a size such as 10mb, got \"huge!\""}`.
Connections are independent; open as many as you like.

### macOS app

`macos/` holds a small SwiftUI client for the daemon: a search field, a table
of results with Finder icons, size and modified date, and the index summary in
the footer. Double-click or Enter opens a result, the down arrow moves into
the list and space shows a Quick Look preview like Finder; the context menu
reveals a result in Finder or copies its path. It connects to the same default
socket as the daemon (`EIND_SOCKET` overrides it). When no daemon answers, the
app starts `eind serve` itself and stops it again when it quits; it looks for
the binary at `EIND_BINARY`, on `PATH`, and in `~/go/bin`, `/opt/homebrew/bin`
and `/usr/local/bin`. A daemon that is already running, for example from a
launchd agent, is used as is.

```sh
go install .
cd macos && swift run
```

Requires Xcode 16 or newer and macOS 14.

Try it from a shell:

```sh
printf '{"query":"readme","limit":3}\n' | nc -U "$XDG_RUNTIME_DIR/eind.sock"
```

## Interactive view

`eind` with no query, or `eind tui`, opens a full-screen list that filters as
you type, using the same query syntax. Searches run in the background and are
cancelled the moment you type again, so the input never blocks. A blank
query lists the whole index; it is shown in index order,
while any other query is sorted by name. Arrow keys, Page Up/Down, Home/End
and the mouse wheel move; Enter prints the selected path to stdout and exits
(if a search is still running, it waits for those results first); Ctrl-O opens
the selection with the system opener; Esc quits. Because the result goes to
stdout, it composes with the shell:

```sh
vim "$(eind)"
```

## Configuration

The config file lives at `~/.config/eind/config` (`$XDG_CONFIG_HOME` is
honoured, `%AppData%` on Windows) and the index at
`~/.local/share/eind/index.bin`. Both can be overridden with `--config` /
`--index` or the `EIND_CONFIG` / `EIND_INDEX` environment variables.

```sh
eind config --init            # write the defaults, then edit the file
```

```
root = ~
root = /Volumes/Data

exclude = /proc
exclude = **/node_modules
exclude = *.tmp
```

`root` may be repeated. An `exclude` without a slash is matched against file
names; with a slash it is matched against the full path and excludes
everything below it. Run `eind index` after editing.

To index a whole machine, set `root = /`. The defaults already exclude
`/proc`, `/sys`, `/dev`, `/run`, `/Volumes` and `/System/Volumes`.

## Keeping the index fresh

`eind serve` loads the index, subscribes to change notifications for every
root (FSEvents on macOS, inotify on Linux, ReadDirectoryChangesW on Windows),
saves the updated index every ten seconds while changes accumulate, and
answers queries over the socket. Renames, moves and newly created folders are
picked up in full. `eind watch` does the same without the socket.

Run it as a login service so it is always on:

```sh
eind service enable           # start now and at every login
eind service disable          # stop and remove it
eind status                   # shows whether the daemon and the service are set up
```

On macOS this writes `~/Library/LaunchAgents/eind.plist`, a launchd agent
that logs to `~/Library/Logs/eind.log`. On Linux it writes
`~/.config/systemd/user/eind.service` and enables it with `systemctl --user`.
The service starts the `eind` found on your `PATH`, so upgrading the binary in
place is enough. The Linux packages instead ship
`/usr/lib/systemd/user/eind.service`, enabled for all users; use one or the
other, not both.

On Linux, inotify needs one watch per directory. The packages install a
sysctl snippet for this; otherwise raise the limit yourself:
`sudo sysctl fs.inotify.max_user_watches=1048576`.

## Compared with find and fd

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

## How it works

The index is a flat array of entries. Each entry stores its name, its parent's
index, size, modified and created times. Parents always precede children, so
a path is rebuilt by walking up the chain, and the whole tree for two million
files fits in a 70 MB file that loads in well under a hundred milliseconds.
Scanning reads directories in parallel; searching splits the array across all
CPU cores. Entries removed by the watcher become tombstones and are compacted
away on save.

## Development

```sh
make test          # go vet + go test ./...
make snapshot      # build archives, deb/rpm/apk/Arch packages and the Homebrew cask into dist/
make release       # the same for a tagged commit, published as a GitHub release
```

Releases are described in `.goreleaser.yaml`; the Linux packages take their
systemd unit, sysctl snippet and install scripts from `packaging/`.

## License

MIT, see [LICENSE](LICENSE).

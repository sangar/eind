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

```sh
git clone <this repository> eind && cd eind
go install .                  # puts eind in $(go env GOPATH)/bin
```

Or `make build` and copy `eind` somewhere on your `PATH`. Go 1.26 or newer is
required; there are no C dependencies, so `CGO_ENABLED=0` builds work too.

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
 "regex": false, "case": false, "whole_word": false, "match_path": false}
```

`limit` defaults to 100; `-1` returns everything. `sort` is `relevance`
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
the footer. Double-click or Enter opens a result; the context menu reveals it
in Finder or copies its path. It connects to the same default socket as the
daemon (`EIND_SOCKET` overrides it) and reconnects when `eind serve` starts.

```sh
eind serve &
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

`eind watch` loads the index, subscribes to change notifications for every
root (FSEvents on macOS, inotify on Linux, ReadDirectoryChangesW on Windows)
and saves the updated index every ten seconds while changes accumulate.
Renames, moves and newly created folders are picked up in full. `eind serve`
does the same and additionally answers queries over the socket, so run that
one if anything else on the machine will query eind.

Run it as a user service so it is always on.

macOS, `~/Library/LaunchAgents/eind.watch.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>eind.watch</string>
  <key>ProgramArguments</key><array><string>/usr/local/bin/eind</string><string>serve</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key><string>/tmp/eind-watch.log</string>
</dict></plist>
```

```sh
launchctl load ~/Library/LaunchAgents/eind.watch.plist
```

Linux, `~/.config/systemd/user/eind-watch.service`:

```ini
[Unit]
Description=eind file index watcher

[Service]
ExecStart=%h/go/bin/eind serve
Restart=on-failure

[Install]
WantedBy=default.target
```

```sh
systemctl --user enable --now eind-watch
```

On Linux, inotify needs one watch per directory. For large trees raise the
limit: `sudo sysctl fs.inotify.max_user_watches=1048576`.

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
make release       # cross-compile into dist/ for macOS, Linux, FreeBSD and Windows
```

## License

MIT, see [LICENSE](LICENSE).

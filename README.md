# eind

Instant file search for the terminal and for launchers.

`eind` indexes the names of every file and folder under your roots once, then
answers searches from that index in milliseconds, with a small query language:
`report ext:pdf size:>1mb dm:thisweek !draft`. A daemon keeps the index
current from filesystem events and answers launchers over a Unix socket.

Linux is the primary target; macOS is fully supported, and FreeBSD and Windows
build too.

## Install

Linux: install the `.deb`, `.rpm`, `.apk` or `.pkg.tar.zst` from the releases
page. The package enables the daemon for every user from their next login;
`systemctl --user start eind` starts it now.

macOS:

```sh
brew install OWNER/tap/eind
eind service enable
```

From source, with Go 1.26 or newer:

```sh
go install .
eind service enable
```

## Quick start

```sh
eind index                    # index your home directory
eind invoice 2024             # names containing "invoice" and "2024"
eind ext:go size:>100kb       # large Go files
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

## Usage

```
eind [options] [query...]     search; with no query, open the interactive view
eind index [--root DIR]...    build the index from the configured roots
eind watch                    keep the index up to date from filesystem events
eind serve                    watch, and answer queries over a Unix socket
eind service enable|disable   run eind serve at login
eind status                   show where the index and config live, and their size
eind config [--init]          show the effective config, or write a default file
eind config edit              open the config in $VISUAL or $EDITOR, then check it
eind tui                      open the interactive view
eind version                  show the version
```

Common options: `-r` regex, `-i` match case, `-n N` max results,
`-s KEY` sort (`path`, `name`, `size`, `dm`, `dc`, `ext`, `relevance`),
`--files`, `--dirs`, `--json`, `--csv`, `-0` for `xargs -0`, `--count`.
Options may appear anywhere; use `--` when a search word is also a command
name (`eind -- index`). `eind --help` lists everything.

The interactive view filters as you type. Enter prints the selection, Ctrl-O
opens it, Esc quits, so `vim "$(eind)"` works.

## Configuration

`~/.config/eind/config` (`$XDG_CONFIG_HOME` is respected, `%AppData%` on
Windows); the index lives at `~/.local/share/eind/index.bin`. `--config`,
`--index`, `EIND_CONFIG` and `EIND_INDEX` override them.

```sh
eind config edit              # write the defaults if needed and open them in $EDITOR
```

```
root = ~
root = /Volumes/Data

exclude = node_modules
exclude = ~/Library/Caches
exclude = *.tmp
```

`root` may be repeated. An `exclude` without a slash matches file names; with
a slash it matches the full path and everything below it. Run `eind index`
after editing.

The defaults leave out dependency trees, `.git`, caches, the trash and
virtual filesystems for the platform eind runs on. Exclude lines in the file
replace the defaults, so `eind config --init` writes them out for editing.

## Running in the background

```sh
eind service enable           # start now and at every login
eind service disable          # stop and remove it
eind status                   # shows whether the daemon and the service are set up
```

On macOS this installs a launchd agent logging to `~/Library/Logs/eind.log`;
on Linux a systemd user unit. It starts the `eind` on your `PATH`, so
upgrading the binary is enough. The Linux packages ship their own unit; use
one or the other, not both.

On Linux, inotify needs one watch per directory. The packages raise the limit;
otherwise run `sudo sysctl fs.inotify.max_user_watches=1048576`.

## More

- [docs/benchmarks.md](docs/benchmarks.md): eind against `find` and `fd`;
  typical searches take about 20 ms where `fd` takes seconds
- [docs/protocol.md](docs/protocol.md): the JSON socket protocol for launchers and GUIs
- [docs/design.md](docs/design.md): the index format and how it stays fresh
- [macos/README.md](macos/README.md): the SwiftUI reference client

## Development

```sh
make test          # go vet + go test -race ./...
make snapshot      # archives, Linux packages and the Homebrew cask in dist/
make release       # the same for a tagged commit, published as a GitHub release
```

## License

MIT, see [LICENSE](LICENSE).

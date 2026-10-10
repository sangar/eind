# eind for macOS

This folder holds a small SwiftUI client for the daemon: a search field, a table
of results with Finder icons, size and modified date, and the index summary in
the footer. Double-click or Enter opens a result, the down arrow moves into
the list and space shows a Quick Look preview like Finder; the context menu
reveals a result in Finder or copies its path. It connects to the same default
socket as the daemon (`EIND_SOCKET` overrides it). When the login service from
`eind service enable` is installed, the app waits for it. Otherwise, when no
daemon answers, the app starts `eind serve` itself and stops it again when it
quits; it looks for the binary at `EIND_BINARY`, on `PATH`, and in `~/.local/bin`,
`/opt/homebrew/bin` and `/usr/local/bin`.

```sh
cc tools/build.c -o nob && ./nob install
cd macos && swift run
```

Requires Xcode 16 or newer and macOS 14.

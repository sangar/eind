# Migrating from the Go version

Until October 2026 this repository held a Go implementation of eind, kept at
the `go-final` tag. The C implementation replaced it. This guide moves a
machine from one to the other.

## What carries over

The two implementations share the index file, its journal and the socket
protocol, so nothing needs rebuilding or reconfiguring:

- **The index and journal.** Both versions write the format in
  [file-format.md](file-format.md). Indexing the same tree with each
  produces byte-identical sections, except that the C writer marks every
  non-ASCII name in the `unicode` bitset where the Go writer marks only
  names whose lowercasing differs, which the format allows. Each version
  reads the other's file and returns the same results in the same order.
- **A daemon handoff.** Changes journaled by one version's daemon are seen
  by the other's queries, and the index one daemon folds its journal into
  on shutdown is read by the other.
- **The config file** at `~/.config/eind/config`, the `EIND_CONFIG`,
  `EIND_INDEX` and `EIND_SOCKET` variables, the query syntax and the
  command set.
- **The macOS client** in `macos/`, which only speaks the socket protocol.

What does not carry over: FreeBSD and Windows. The C version runs on macOS
and Linux.

## Steps

Do them in this order, so that only one daemon ever writes to the journal.

1. **Stop the old daemon with the old binary.** This also removes its
   launchd agent or systemd user unit.

   ```sh
   eind service disable
   ```

2. **Delete the Go binary.** `go install` has no uninstall; it only copied
   the binary into Go's bin directory. With mise, that is
   `~/.local/share/mise/installs/go/<version>/bin/eind`; otherwise it is
   `~/go/bin/eind`.

   ```sh
   rm ~/go/bin/eind
   mise reshim        # only with mise, to drop its shim
   ```

3. **Build and install the C version.** It needs a C23 compiler and `make`:
   Apple clang with the Xcode command line tools on macOS, clang 18 or
   gcc 14 or newer on Linux.

   ```sh
   git clone git@github.com:sangar/eind.git
   cd eind
   make && make test
   cp eind ~/.local/bin/
   ```

   `~/.local/bin` must be on your `PATH`.

4. **On Linux, raise the inotify limit.** The watcher registers every
   indexed directory, and the default limit of 8192 watches is far too low
   for a home directory.

   ```sh
   sudo cp packaging/50-eind.conf /etc/sysctl.d/
   sudo sysctl --system
   ```

5. **Check that the existing index is picked up.** It is the same file, so
   there is nothing to rebuild.

   ```sh
   eind status
   ```

6. **Start the new daemon at login.**

   ```sh
   eind service enable
   ```

## If something looks wrong

A rebuild takes well under a second per 150,000 entries and replaces the
index and journal from scratch:

```sh
eind index
```

The Go version remains available from the `go-final` tag should you need to
go back; its index is still readable by both.

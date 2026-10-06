# How eind works

## The index

The index is a flat array of entries. Each entry stores its name, its parent's
index, size, modified and created times. Parents always precede children, so
a path is rebuilt by walking up the chain, and the whole tree for two million
files fits in a 70 MB file that loads in well under a hundred milliseconds.
Scanning reads directories in parallel; searching splits the array across all
CPU cores. Entries removed by the watcher become tombstones and are compacted
away on save.

## Keeping it fresh

`eind serve` loads the index, subscribes to change notifications for every
root (FSEvents on macOS, inotify on Linux, ReadDirectoryChangesW on Windows),
and answers queries over the socket. Renames, moves and newly created folders
are picked up in full. `eind watch` does the same without the socket.

Every change is appended at once to a journal next to the index file
(`index.bin.journal`). Command line searches load the index file and replay
its journal, so they see the daemon's changes immediately without talking to
it. Every ten seconds the daemon checks whether the journal has grown past
10,000 changes or 1% of the index, and if so folds it into a new index file
and starts an empty journal; it does so once more when it stops.


## Default excludes

By default `eind` excludes what a launcher should never offer: `node_modules`,
`.git` and `.cache` folders everywhere, and, derived from the platform it runs
on, the user's cache directory, the trash, and the virtual or foreign
filesystems. On Linux that is `~/.local/share/Trash`, `/proc`, `/sys`, `/dev`
and `/run`; on macOS `~/Library/Caches`, `~/.Trash`, `/dev`, `/Volumes`,
`/System/Volumes` and `/private/var/vm`, plus the sandboxed app data in
`~/Library/Containers` and `~/Library/Group Containers`, which macOS guards
with an "access data from other apps" prompt. On a developer's machine these
hold roughly half of all files. Tool-specific trees such as `~/.local/share/mise`
or `~/.cargo/registry` are worth adding yourself. The exclude lines in the
config file replace the default list, so `eind config --init` writes it out
for editing: delete a line to index that location again. To index a whole
machine, set `root = /`.

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

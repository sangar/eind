# Index file and journal format

This format is shared by every eind implementation: they all default to the
same paths, and whichever one runs the daemon writes the journal that the
others' commands read. Treat it as a contract. Readers must accept everything
described here; writers must produce exactly this.

All integers are little-endian.

## Index file (`index.bin`)

Code: `src/index/segment.c` (`segment_write`, `decode`).

### Header (192 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | 4 bytes | magic `EIND` |
| 4 | u32 | format version, `2` |
| 8 | u64 | generation: nanoseconds since the epoch when written; names this file in its journal |
| 16 | i64 | built at: unix seconds when the index was first built |
| 24 | u32 | n: number of entries |
| 28 | u32 | d: number of distinct names |
| 32 + 16·k | u64, u64 | offset and length in bytes of section k, for k = 0..9 |

Every section starts at an offset that is a multiple of 8; the gaps are zero
bytes.

### Sections

| k | Section | Content | Length |
|---|---|---|---|
| 0 | roots | root paths, each followed by NUL | any |
| 1 | names | the distinct names in name order, each followed by NUL | any |
| 2 | name offsets | u32 per distinct name: offset of its first byte in names | 4·d |
| 3 | name ids | u32 per entry: index of its name among the distinct names | 4·n |
| 4 | parents | u32 per entry: parent entry id, or `0xFFFFFFFF` for a root | 4·n |
| 5 | sizes | i64 per entry; 0 for directories | 8·n |
| 6 | modified | u32 per entry, unix seconds, clamped to 0..2³²−1 | 4·n |
| 7 | created | u32 per entry, unix seconds, 0 when unknown | 4·n |
| 8 | dirs | bitset over entries: bit i set when entry i is a directory | 8·⌈n/64⌉ |
| 9 | unicode | bitset over distinct names whose Unicode lowercasing may differ from ASCII lowercasing | 8·⌈d/64⌉ |

Bitsets are arrays of u64 words; bit i is bit `i % 64` of word `i / 64`.

A root entry's name is its absolute path (`/Users/me`); every other name is a
single path component. A path is the root's name followed by each component,
joined with `/` unless the previous part already ends in `/`.

### Order rules

Readers in other implementations rely on these orders for sorting without
comparing strings, so a writer must keep them:

- **Entries are in path order**: sorted by full path compared byte by byte
  after ASCII lowercasing, ties broken by the name as written. This puts
  `a`, `a.txt`, `a/b` in that order, because `.` sorts before `/`. Parents
  therefore precede children. `path_order` produces it by walking the tree
  with each directory's children sorted, where a directory appears twice:
  once as itself (key: its name) and once as its subtree (key: its name
  followed by `/`).
- **Names are in name order**: sorted by ASCII-lowercased bytes, then by the
  bytes as written. The name ids of entries therefore sort like their names.
- **The unicode bitset** marks names that a reader matching case-insensitively
  must test in full rather than with ASCII case folding. This writer marks
  every name containing a byte ≥ 0x80, which is always safe.

### Validation

A reader rejects the file as corrupt unless: the magic matches; each section
lies inside the file, starts at a multiple of 8 and has the length given
above; the names section ends with NUL; name offsets increase and lie inside
names; every name id is below d; and every parent is `0xFFFFFFFF` or below the
entry's own id. A file with another version is rejected with a request to run
`eind index`.

## Journal (`index.bin.journal`)

Code: `src/index/journal.c`.

The journal lists the changes made since the index file was written.

```
header   'EINJ' (4 bytes), generation u64 — the generation of the index file it belongs to
entries  until end of file:
  'A' parent u32, size i64, modified i64, created i64, dir u8, name length u16, name bytes
  'R' id u32
  'U' id u32, size i64, modified i64, created i64
```

- **Ids** are as the index file numbers them; each `A` entry gets the next id
  after the file's entries and the entries added before it, in journal order.
- `R` removes one entry (callers also remove its descendants). `U` replaces
  an entry's size and times. This implementation writes only `A` and `R`
  (a changed file is removed and added again) but reads all three.
- **Generation**: a reader ignores a journal whose header names another
  generation; it predates the file or a new file is being written.
- **Torn writes**: writers append whole batches; a reader stops at an
  incomplete or unknown entry and ignores the rest. A writer reopening the
  journal truncates such a tail first.
- **Replacing the index file**: write the new journal (header only) first and
  the new index file second, each through a temporary file renamed into place.
  A daemon that finds the journal header changed before appending knows
  another process replaced the file, and loads it again.

> **Historical.** This is the design proposal the C implementation started
> from. The code has since diverged: the trigram inverted index and the
> private file format were removed, the index file is decoded on load rather
> than used in place, and a journal keeps commands up to date. See
> [architecture.md](architecture.md) for the current design.

Here are the proposed architecture and design decisions for a high-performance file indexer and search engine targeting macOS and Linux.

The core recommendation is to use C with arena allocation, memory-mapped immutable index segments, batched filesystem scanning, and a worker-thread pool.

ARCHITECTURE

Filesystem
↓
Directory Scanner
↓
Work Queue
↓
Worker Thread Pool
↓
Batch Builder
↓
Immutable Index Segments
↓
Segment Manager / Compaction
↓
Search Engine

The search engine operates on snapshots of immutable segments, allowing indexing and searching to happen concurrently with minimal locking.

1. MEMORY MANAGEMENT

Use arenas rather than individual malloc/free operations wherever object lifetimes are similar.

Suggested arenas:

- Worker/scan arena — temporary directory and parsing data
- Segment arena — data being assembled into an index segment
- Query arena — temporary search results and query structures
- String storage — persistent string data

For example:

typedef struct {
uint8_t *base;
size_t capacity;
size_t offset;
} Arena;

An entire batch can then be discarded with:

arena_reset(&arena);

This avoids allocator overhead and fragmentation.

2. FILE RECORDS

Don't store every path as an independent heap allocation.

Use compact records containing offsets:

typedef struct {
uint64_t file_id;
uint64_t parent_id;
uint64_t size;
uint64_t mtime;

uint32_t name_offset;
uint32_t name_length;

uint32_t path_offset;
uint32_t path_length;

uint32_t flags;
} FileRecord;

Use numeric file IDs throughout the search index rather than storing complete paths in posting lists.

3. DIRECTORY SCANNING

Use a fixed-size worker pool rather than creating threads per directory.

A directory becomes a work item. Workers scan directories and push newly discovered directories back onto the queue.

Where practical, use openat()/fstatat() style APIs rather than repeatedly constructing full absolute paths.

This reduces path manipulation and can improve filesystem traversal.

4. BATCHING

Avoid:

file → lock → update → unlock

Instead:

1,000 files
↓
build batch
↓
update/write once

Batch filesystem metadata, tokenization, index updates, and disk writes.

5. IMMUTABLE INDEX SEGMENTS

Do not have every worker modify one giant global index.

Workers produce immutable segments:

Segment 001
Segment 002
Segment 003

The search engine searches these segments.

Periodically, background compaction merges segments:

Segment 1 ─┐
Segment 2 ─┼──→ Merge ──→ Segment 5
Segment 3 ─┘

This is similar to an LSM-style architecture and greatly reduces synchronization.

6. INVERTED INDEX

For filename/content search:

term → posting list

Example:

"rust" → [7, 19, 33, 48, 90]
"project" → [3, 7, 48, 92]

A query such as:

rust AND project

becomes an intersection of posting lists:

[7, 19, 33, 48, 90]
[3, 7, 48, 92]

→ [7, 48]

Store sorted file IDs and use delta encoding plus variable-length integers to reduce index size.

7. MEMORY-MAPPED INDEX

Once a segment becomes immutable, store it in a compact binary format and mmap() it.

A segment could contain:

Header
File records
String table
Term dictionary
Posting lists

This allows the search engine to access index data directly through the OS page cache without loading the entire index into ordinary heap memory.

8. SNAPSHOT SEARCH

The search engine should acquire a consistent index snapshot.

Conceptually:

IndexSnapshot *snapshot = index_acquire_snapshot();

search(snapshot, query);

index_release_snapshot(snapshot);

This allows searches to continue while new segments are being created.

9. FILESYSTEM WATCHING

Do a full scan initially, then switch to incremental updates.

macOS:
FSEvents

Linux:
inotify

Events become indexing tasks:

CREATE → index file
MODIFY → re-index
DELETE → tombstone
RENAME → update metadata

10. TOMBSTONES

For immutable indexes, don't physically remove records immediately.

A deleted file can produce:

DELETE file_id=101

During background compaction, deleted records are removed permanently.

This makes incremental updates very cheap.

11. CONTENT INDEXING

Keep filename/path indexing separate from full-content indexing.

Pipeline:

filesystem
↓
file filter
↓
content reader
↓
tokenizer
↓
content index

Skip or configure limits for binary files, huge files, and files that don't need content indexing.

12. PLATFORM ABSTRACTION

Keep platform-specific filesystem code isolated:

src/
core/
arena.c
hash.c
queue.c
threadpool.c

index/
segment.c
dictionary.c
postings.c
search.c
merge.c

fs/
scanner.c
watcher.c
fs_common.c
fs_linux.c
fs_macos.c

main.c

The core indexing/search code should not depend on FSEvents or inotify.

13. DEVELOPMENT PHASES

Phase 1:
- Arena allocator
- Directory scanner
- File records
- Hash table
- Basic search

Phase 2:
- Thread pool
- Batched scanning
- String storage/interning
- Binary index

Phase 3:
- Immutable segments
- mmap
- Inverted index
- Posting compression

Phase 4:
- FSEvents
- inotify
- Incremental updates
- Tombstones
- Segment compaction

Phase 5:
- Profile and optimize actual bottlenecks

RECOMMENDED FINAL DESIGN

C
+ arena allocation
+ fixed-size records
+ numeric file IDs
+ batched filesystem scanning
+ worker thread pool
+ immutable index segments
+ mmap
+ compressed inverted indexes
+ FSEvents on macOS
+ inotify on Linux
+ background segment compaction
+ snapshot-based searches

The key performance principle is not simply "C is faster." The major gains will come from minimizing allocation, copying, locking, and shared mutable state.

For this particular workload, C is a very good fit because the indexer has predictable object lifetimes, benefits from compact memory layouts, and can take advantage of direct filesystem and mmap APIs.


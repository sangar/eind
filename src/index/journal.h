#ifndef EIND_JOURNAL_H
#define EIND_JOURNAL_H

#include "mc/core/error.h"
#include "mc/text/str.h"
#include "index.h"

/*
 * The journal lists the changes made since the index file was written, so
 * that any process loading the index sees a running daemon's changes without
 * waiting for a new index file. It lives next to the index file
 * ("<index>.journal") and is a header naming the index file's generation,
 * then entries:
 *
 *   'A' parent u32, size i64, modified i64, created i64, dir u8, name length u16, name
 *   'R' id u32
 *   'U' id u32, size i64, modified i64, created i64
 *
 * Ids are as the index file numbers them, with added entries numbered after
 * it in journal order. The writer appends whole batches; a reader that finds
 * a half-written last entry ignores it, and a journal of another generation
 * is ignored altogether.
 */

/*
 * journal_replay applies the journal of the index file at path to s, which
 * must be freshly loaded from it, and stores the snapshot to use. It takes
 * the caller's reference to s, also on failure.
 */
[[nodiscard]] Error journal_replay(Snapshot *s, String path, Snapshot **replayed, Err *err);

typedef struct Journal Journal;

/*
 * journal_open starts recording changes to s, the snapshot loaded from the
 * index file at path. A journal that is missing, belongs to another index
 * file, or ends in a half-written entry is rewritten to hold exactly the
 * changes s has replayed.
 */
[[nodiscard]] Error journal_open(String path, const Snapshot *s, Journal **journal, Err *err);
void journal_free(Journal *j);

/* journal_record queues the records added and removed between two snapshots. */
void journal_record(Journal *j, const Snapshot *before, const Snapshot *after);

/*
 * journal_flush appends the queued changes. *replaced says that another
 * process wrote a new index file instead, which must be loaded again.
 */
[[nodiscard]] Error journal_flush(Journal *j, bool *replaced, Err *err);

/* journal_entries is the number of changes since the index file was written. */
size_t journal_entries(const Journal *j);

#endif

#ifndef EIND_UPDATER_H
#define EIND_UPDATER_H

#include <stdbool.h>

#include "../core/util.h"
#include "../index/index.h"
#include "fs.h"

/*
 * Updater applies batches of changed paths to an index. Every path is simply
 * examined again on disk: whatever exists is added or refreshed, whatever is
 * gone is tombstoned, and a directory that appears is scanned in full. Each
 * batch becomes one delta segment plus a new tombstone bitmap, published as
 * a new snapshot.
 */
typedef struct Updater Updater;

/* updater_new starts from the index's current snapshot. */
Updater *updater_new(Index *ix, const Excludes *ex);
void updater_free(Updater *u);
/* updater_reset starts over from a snapshot, as needed after a compaction renumbers ids. */
void updater_reset(Updater *u, Snapshot *s);
/* updater_snapshot is the newest snapshot this updater published, without a new reference. */
Snapshot *updater_snapshot(const Updater *u);

/* updater_apply reports whether the index changed; directories it added are appended to new_dirs. */
bool updater_apply(Updater *u, const StrList *paths, StrList *new_dirs);

#endif

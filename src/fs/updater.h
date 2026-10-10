#ifndef EIND_UPDATER_H
#define EIND_UPDATER_H

#include "mc/concurrency/threadpool.h"
#include "mc/platform/watch.h"
#include "../index/index.h"
#include "excludes.h"

/*
 * Updater applies batches of changed paths to an index. Every path is simply
 * examined again on disk: whatever exists is added or refreshed, whatever is
 * gone is tombstoned, and a directory that appears is scanned in full. Each
 * batch becomes one delta segment plus a new tombstone bitmap, published as
 * a new snapshot.
 */
typedef struct Updater Updater;

/* updater_create starts from the index's current snapshot and scans directories that appear on io. */
Updater *updater_create(ThreadPool *io, Index *ix, const Excludes *ex);
void updater_destroy(Updater *u);
/* updater_reset starts over from s, taking the caller's reference, as needed after a compaction renumbers ids. */
void updater_reset(Updater *u, Snapshot *s);
/* updater_snapshot is the newest snapshot this updater published, without a new reference. */
Snapshot *updater_snapshot(const Updater *u);

/*
 * updater_apply reconciles each event's path with the disk and reports
 * whether the index changed. An event marked rescan replaces a directory and
 * everything below it with a fresh scan, for changes the events left out.
 */
bool updater_apply(Updater *u, const WatchEvent *events, size_t count);

#endif

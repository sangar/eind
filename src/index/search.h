#ifndef EIND_SEARCH_H
#define EIND_SEARCH_H

#include "mc/concurrency/cancel.h"
#include "mc/concurrency/threadpool.h"
#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"
#include "index.h"
#include "query.h"

typedef enum {
    SORT_PATH,
    SORT_NAME,
    SORT_SIZE,
    SORT_MODIFIED,
    SORT_CREATED,
    SORT_EXT,
    SORT_RELEVANCE, /* needs the query, see search_rank */
} SortKey;

/* sort_key_parse reads a sort key name; an unknown one is ERR_INVALID_ARGUMENT. */
[[nodiscard]] Error sort_key_parse(String s, SortKey *out, Err *err);

/*
 * search_run scans every record of s on pool and collects the ids of the
 * live records matching the query into hits, in id order, allocated in
 * arena. Cancelling cancel, which may be nullptr, from another thread stops
 * it early with ERR_CANCELLED; an invalid regular expression is ERR_PARSE.
 */
[[nodiscard]] Error search_run(ThreadPool *pool, Arena *arena, const Snapshot *s, const QueryNode *query, Cancel *cancel,
                               IdList *hits, Err *err);

/*
 * search_top orders hits by key and returns how many of the best keep it
 * placed at the front; keep < 0 means all. Its temporaries live in scratch
 * until it returns.
 */
size_t search_top(Arena *scratch, const Snapshot *s, uint32_t *hits, size_t n, SortKey key, bool descending, int64_t keep);

/*
 * search_rank orders hits the way a launcher wants them: names equal to a
 * search term first, then names starting with it, then names containing it
 * at a word boundary, then plain substring matches. Ties go to shallower
 * paths, then to name order.
 */
size_t search_rank(Arena *scratch, const Snapshot *s, uint32_t *hits, size_t n, const QueryNode *query, int64_t keep);


#endif

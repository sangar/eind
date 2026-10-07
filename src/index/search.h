#ifndef EIND_SEARCH_H
#define EIND_SEARCH_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "../core/threadpool.h"
#include "../core/util.h"
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

bool sort_key_parse(const char *s, SortKey *out, Err *err);

typedef enum { SEARCH_OK, SEARCH_ERROR, SEARCH_CANCELLED } SearchStatus;

/*
 * search_run collects the ids of all live records matching the query, in id
 * order. Setting *cancel to non-zero from another thread stops it early.
 */
/* search_run scans every record of s on pool and collects the matching ids. */
SearchStatus search_run(ThreadPool *pool, const Snapshot *s, const QueryNode *query, const atomic_int *cancel, U32Vec *hits,
                        Err *err);

/* search_top orders hits by key and returns how many of the best keep it placed at the front; keep < 0 means all. */
size_t search_top(const Snapshot *s, uint32_t *hits, size_t n, SortKey key, bool descending, long keep);

/*
 * search_rank orders hits the way a launcher wants them: names equal to a
 * search term first, then names starting with it, then names containing it
 * at a word boundary, then plain substring matches. Ties go to shallower
 * paths, then to name order.
 */
size_t search_rank(const Snapshot *s, uint32_t *hits, size_t n, const QueryNode *query, long keep);


#endif

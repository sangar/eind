#ifndef EIND_SORT_H
#define EIND_SORT_H

#include <stddef.h>

/* Comparators receive a context, so sorting stays reentrant across threads. */
typedef int (*CompareFn)(const void *ctx, const void *a, const void *b);

/* sort_stable is a merge sort over elements of the given size. */
void sort_stable(void *base, size_t n, size_t size, CompareFn cmp, const void *ctx);

/*
 * sort_top moves the k smallest elements, in order, to the front of base and
 * returns how many that is. Selecting a few hundred out of a million is far
 * cheaper than sorting the million.
 */
size_t sort_top(void *base, size_t n, size_t size, size_t k, CompareFn cmp, const void *ctx);

#endif

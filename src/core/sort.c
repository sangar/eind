#include "sort.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static void merge_pass(char *src, char *dst, size_t n, size_t size, size_t width, CompareFn cmp,
                       const void *ctx) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
        size_t mid = MIN(lo + width, n), hi = MIN(lo + 2 * width, n);
        size_t i = lo, j = mid, k = lo;
        while (i < mid && j < hi) {
            if (cmp(ctx, src + j * size, src + i * size) < 0) {
                memcpy(dst + k++ * size, src + j++ * size, size);
            } else {
                memcpy(dst + k++ * size, src + i++ * size, size);
            }
        }
        memcpy(dst + k * size, src + i * size, (mid - i) * size);
        k += mid - i;
        memcpy(dst + k * size, src + j * size, (hi - j) * size);
    }
}

static void insertion_sort(char *base, size_t n, size_t size, CompareFn cmp, const void *ctx, char *tmp) {
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        memcpy(tmp, base + i * size, size);
        while (j > 0 && cmp(ctx, tmp, base + (j - 1) * size) < 0) {
            memcpy(base + j * size, base + (j - 1) * size, size);
            j--;
        }
        memcpy(base + j * size, tmp, size);
    }
}

void sort_stable(void *base, size_t n, size_t size, CompareFn cmp, const void *ctx) {
    if (n < 2) return;
    enum { RUN = 16 };
    char *a = base;
    char *tmp = xmalloc(size);
    for (size_t lo = 0; lo < n; lo += RUN) insertion_sort(a + lo * size, MIN(RUN, n - lo), size, cmp, ctx, tmp);
    free(tmp);
    if (n <= RUN) return;
    char *buf = xmalloc(n * size);
    char *src = a, *dst = buf;
    for (size_t width = RUN; width < n; width *= 2) {
        merge_pass(src, dst, n, size, width, cmp, ctx);
        char *t = src;
        src = dst;
        dst = t;
    }
    if (src != a) memcpy(a, src, n * size);
    free(buf);
}

/* The heap keeps the worst of the best k at its root. */
static void sift_down(char *heap, size_t n, size_t i, size_t size, CompareFn cmp, const void *ctx, char *tmp) {
    for (;;) {
        size_t worst = i, l = 2 * i + 1, r = l + 1;
        if (l < n && cmp(ctx, heap + l * size, heap + worst * size) > 0) worst = l;
        if (r < n && cmp(ctx, heap + r * size, heap + worst * size) > 0) worst = r;
        if (worst == i) return;
        memcpy(tmp, heap + i * size, size);
        memcpy(heap + i * size, heap + worst * size, size);
        memcpy(heap + worst * size, tmp, size);
        i = worst;
    }
}

size_t sort_top(void *base, size_t n, size_t size, size_t k, CompareFn cmp, const void *ctx) {
    if (k >= n || k * 4 > n) {
        sort_stable(base, n, size, cmp, ctx);
        return MIN(k, n);
    }
    if (k == 0) return 0;
    char *a = base;
    char *tmp = xmalloc(size);
    for (size_t i = k / 2 + 1; i-- > 0;) sift_down(a, k, i, size, cmp, ctx, tmp);
    for (size_t i = k; i < n; i++) {
        if (cmp(ctx, a + i * size, a) < 0) {
            memcpy(a, a + i * size, size);
            sift_down(a, k, 0, size, cmp, ctx, tmp);
        }
    }
    free(tmp);
    sort_stable(a, k, size, cmp, ctx);
    return k;
}

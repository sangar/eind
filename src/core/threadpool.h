#ifndef EIND_THREADPOOL_H
#define EIND_THREADPOOL_H

#include <stddef.h>

/* ThreadPool runs tasks on a fixed set of worker threads. */
typedef struct ThreadPool ThreadPool;

ThreadPool *threadpool_create(int threads);
void threadpool_destroy(ThreadPool *pool);
int threadpool_size(const ThreadPool *pool);
void threadpool_submit(ThreadPool *pool, void (*fn)(void *arg), void *arg);

/* One pool per CPU for searching, and a wider one for blocking filesystem reads. */
ThreadPool *threadpool_cpu(void);
ThreadPool *threadpool_io(void);
int cpu_count(void);

/*
 * A ParallelPlan splits n items into chunks of at least min_chunk; callers
 * size per-chunk outputs from it, then threadpool_run_chunks calls
 * fn(ctx, lo, hi, chunk) for every chunk and waits for all of them.
 */
typedef struct {
    size_t n, chunk_size, chunks;
} ParallelPlan;

ParallelPlan parallel_plan(const ThreadPool *pool, size_t n, size_t min_chunk);
void threadpool_run_chunks(ThreadPool *pool, ParallelPlan plan,
                           void (*fn)(void *ctx, size_t lo, size_t hi, size_t chunk), void *ctx);

#endif

#ifndef EIND_THREADPOOL_H
#define EIND_THREADPOOL_H

#include <stddef.h>

/* ThreadPool runs tasks on a fixed set of worker threads. */
typedef struct ThreadPool ThreadPool;

/* A Task lives in the caller's memory, usually inside the work item it runs on. */
typedef struct {
    void (*fn)(void *arg);
    void *arg;
} Task;

ThreadPool *threadpool_create(int threads);
void threadpool_destroy(ThreadPool *pool);
/* threadpool_submit queues a task; the caller keeps it alive until fn has run. */
void threadpool_submit(ThreadPool *pool, Task *task);

/* cpu_count sizes a search pool; io_thread_count caps a scanning pool, since directory reads contend in the kernel. */
int cpu_count(void);
int io_thread_count(void);

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

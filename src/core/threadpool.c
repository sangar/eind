#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "queue.h"
#include "util.h"

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} Task;

struct ThreadPool {
    Queue tasks;
    pthread_t *threads;
    int size;
};

static void *worker(void *arg) {
    ThreadPool *pool = arg;
    Task *task;
    while ((task = queue_pop(&pool->tasks)) != NULL) {
        task->fn(task->arg);
        free(task);
    }
    return NULL;
}

ThreadPool *threadpool_create(int threads) {
    ThreadPool *pool = xcalloc(1, sizeof *pool);
    queue_init(&pool->tasks);
    pool->size = threads < 1 ? 1 : threads;
    pool->threads = xmalloc((size_t)pool->size * sizeof *pool->threads);
    for (int i = 0; i < pool->size; i++) pthread_create(&pool->threads[i], NULL, worker, pool);
    return pool;
}

void threadpool_destroy(ThreadPool *pool) {
    queue_close(&pool->tasks);
    for (int i = 0; i < pool->size; i++) pthread_join(pool->threads[i], NULL);
    queue_destroy(&pool->tasks);
    free(pool->threads);
    free(pool);
}

int threadpool_size(const ThreadPool *pool) { return pool->size; }

void threadpool_submit(ThreadPool *pool, void (*fn)(void *arg), void *arg) {
    Task *task = xmalloc(sizeof *task);
    task->fn = fn;
    task->arg = arg;
    queue_push(&pool->tasks, task);
}

int cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

static ThreadPool *cpu_pool, *io_pool;
static pthread_once_t cpu_once = PTHREAD_ONCE_INIT, io_once = PTHREAD_ONCE_INIT;

static void make_cpu_pool(void) { cpu_pool = threadpool_create(cpu_count()); }

/*
 * Directory reads contend on filesystem locks in the kernel: indexing 150k
 * entries on APFS took 1.0s with 64 threads but 0.37s with 5 or 6, while
 * kernel time fell from 10s to 1.3s. A few threads beat many.
 */
static void make_io_pool(void) { io_pool = threadpool_create(MIN(cpu_count(), 6)); }

ThreadPool *threadpool_cpu(void) {
    pthread_once(&cpu_once, make_cpu_pool);
    return cpu_pool;
}

ThreadPool *threadpool_io(void) {
    pthread_once(&io_once, make_io_pool);
    return io_pool;
}

ParallelPlan parallel_plan(const ThreadPool *pool, size_t n, size_t min_chunk) {
    size_t workers = (size_t)pool->size;
    size_t chunk = (n + workers - 1) / workers;
    if (chunk < min_chunk) chunk = min_chunk;
    if (chunk == 0) chunk = 1;
    return (ParallelPlan){.n = n, .chunk_size = chunk, .chunks = (n + chunk - 1) / chunk};
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t done;
    size_t remaining;
    void (*fn)(void *ctx, size_t lo, size_t hi, size_t chunk);
    void *ctx;
} Batch;

typedef struct {
    Batch *batch;
    size_t lo, hi, chunk;
} ChunkTask;

static void run_chunk(void *arg) {
    ChunkTask *t = arg;
    Batch *b = t->batch;
    b->fn(b->ctx, t->lo, t->hi, t->chunk);
    pthread_mutex_lock(&b->mu);
    if (--b->remaining == 0) pthread_cond_signal(&b->done);
    pthread_mutex_unlock(&b->mu);
}

void threadpool_run_chunks(ThreadPool *pool, ParallelPlan plan,
                           void (*fn)(void *ctx, size_t lo, size_t hi, size_t chunk), void *ctx) {
    if (plan.chunks == 0) return;
    if (plan.chunks == 1) {
        fn(ctx, 0, plan.n, 0);
        return;
    }
    Batch batch = {.remaining = plan.chunks, .fn = fn, .ctx = ctx};
    pthread_mutex_init(&batch.mu, NULL);
    pthread_cond_init(&batch.done, NULL);
    ChunkTask *tasks = xmalloc(plan.chunks * sizeof *tasks);
    for (size_t c = 0; c < plan.chunks; c++) {
        size_t lo = c * plan.chunk_size;
        tasks[c] = (ChunkTask){.batch = &batch, .lo = lo, .hi = MIN(lo + plan.chunk_size, plan.n), .chunk = c};
        threadpool_submit(pool, run_chunk, &tasks[c]);
    }
    pthread_mutex_lock(&batch.mu);
    while (batch.remaining > 0) pthread_cond_wait(&batch.done, &batch.mu);
    pthread_mutex_unlock(&batch.mu);
    pthread_mutex_destroy(&batch.mu);
    pthread_cond_destroy(&batch.done);
    free(tasks);
}

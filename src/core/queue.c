#include "queue.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "util.h"

void queue_init(Queue *q) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->nonempty, NULL);
    q->items = NULL;
    q->head = q->len = q->cap = 0;
    q->closed = false;
}

void queue_destroy(Queue *q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->nonempty);
    free(q->items);
}

static void grow(Queue *q) {
    size_t cap = q->cap ? q->cap * 2 : 64;
    void **items = xmalloc(cap * sizeof *items);
    for (size_t i = 0; i < q->len; i++) items[i] = q->items[(q->head + i) % q->cap];
    free(q->items);
    q->items = items;
    q->cap = cap;
    q->head = 0;
}

void queue_push(Queue *q, void *item) {
    pthread_mutex_lock(&q->mu);
    if (q->len == q->cap) grow(q);
    q->items[(q->head + q->len) % q->cap] = item;
    q->len++;
    pthread_cond_signal(&q->nonempty);
    pthread_mutex_unlock(&q->mu);
}

static void *take(Queue *q) {
    void *item = q->items[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return item;
}

void *queue_pop(Queue *q) {
    pthread_mutex_lock(&q->mu);
    while (q->len == 0 && !q->closed) pthread_cond_wait(&q->nonempty, &q->mu);
    void *item = q->len ? take(q) : NULL;
    pthread_mutex_unlock(&q->mu);
    return item;
}

bool queue_pop_timeout(Queue *q, int timeout_ms, void **out) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec deadline;
    long long ns = (long long)now.tv_usec * 1000 + (long long)(timeout_ms % 1000) * 1000000;
    deadline.tv_sec = now.tv_sec + timeout_ms / 1000 + (time_t)(ns / 1000000000);
    deadline.tv_nsec = (long)(ns % 1000000000);

    pthread_mutex_lock(&q->mu);
    while (q->len == 0 && !q->closed) {
        if (pthread_cond_timedwait(&q->nonempty, &q->mu, &deadline) == ETIMEDOUT) break;
    }
    bool got = q->len > 0;
    if (got) *out = take(q);
    pthread_mutex_unlock(&q->mu);
    return got;
}

void queue_close(Queue *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->nonempty);
    pthread_mutex_unlock(&q->mu);
}

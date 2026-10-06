#ifndef EIND_QUEUE_H
#define EIND_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/* Queue is an unbounded, blocking FIFO of pointers shared between threads. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t nonempty;
    void **items;
    size_t head, len, cap;
    bool closed;
} Queue;

void queue_init(Queue *q);
void queue_destroy(Queue *q);
void queue_push(Queue *q, void *item);
/* queue_pop blocks until an item arrives; it returns NULL once closed and drained. */
void *queue_pop(Queue *q);
/* queue_pop_timeout waits at most timeout_ms; it returns false on timeout or close. */
bool queue_pop_timeout(Queue *q, int timeout_ms, void **out);
void queue_close(Queue *q);

#endif

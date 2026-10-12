#include "ring.h"

#include <stdlib.h>

int ring_init(ring_t *r, int capacity)
{
    r->slots = calloc(capacity, sizeof *r->slots);
    if (!r->slots)
        return -1;
    r->capacity = capacity;
    r->head = r->tail = r->count = 0;
    r->closed = false;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->not_empty, NULL);
    pthread_cond_init(&r->not_full, NULL);
    return 0;
}

void ring_destroy(ring_t *r)
{
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->not_empty);
    pthread_cond_destroy(&r->not_full);
    free(r->slots);
}

void ring_push(ring_t *r, void *item)
{
    pthread_mutex_lock(&r->lock);

    /* 'while', not 'if': a woken thread must re-check the condition
     * (spurious wakeups, or another thread got there first). */
    while (r->count == r->capacity)
        pthread_cond_wait(&r->not_full, &r->lock);   /* releases lock while asleep */

    r->slots[r->head] = item;
    r->head = (r->head + 1) % r->capacity;           /* wrap around: "circular" */
    r->count++;

    pthread_cond_signal(&r->not_empty);              /* wake one consumer */
    pthread_mutex_unlock(&r->lock);
}

void *ring_pop(ring_t *r)
{
    pthread_mutex_lock(&r->lock);

    while (r->count == 0 && !r->closed)
        pthread_cond_wait(&r->not_empty, &r->lock);

    if (r->count == 0) {                             /* closed and drained */
        pthread_mutex_unlock(&r->lock);
        return NULL;
    }

    void *item = r->slots[r->tail];
    r->tail = (r->tail + 1) % r->capacity;
    r->count--;

    pthread_cond_signal(&r->not_full);               /* wake the producer */
    pthread_mutex_unlock(&r->lock);
    return item;
}

void ring_close(ring_t *r)
{
    pthread_mutex_lock(&r->lock);
    r->closed = true;
    pthread_cond_broadcast(&r->not_empty);           /* wake ALL consumers so they exit */
    pthread_mutex_unlock(&r->lock);
}

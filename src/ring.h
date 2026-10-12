/*
 * ring.h - bounded, thread-safe ring buffer (circular queue) of pointers.
 *
 * This is the classic producer-consumer structure used in firmware:
 *   - fixed capacity, allocated once (no malloc while running)
 *   - producer blocks when full  (backpressure)
 *   - consumers block when empty
 */
#ifndef RING_H
#define RING_H

#include <pthread.h>
#include <stdbool.h>

typedef struct {
    void **slots;            /* fixed array of item pointers          */
    int capacity;
    int head;                /* next slot to write                    */
    int tail;                /* next slot to read                     */
    int count;               /* items currently stored                */
    bool closed;             /* producer finished: no more items      */
    pthread_mutex_t lock;    /* protects every field above            */
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} ring_t;

int   ring_init(ring_t *r, int capacity);
void  ring_destroy(ring_t *r);

/* Blocks while full. */
void  ring_push(ring_t *r, void *item);

/* Blocks while empty. Returns NULL once the ring is closed AND drained. */
void *ring_pop(ring_t *r);

/* Producer calls this when done; wakes every waiting consumer. */
void  ring_close(ring_t *r);

#endif

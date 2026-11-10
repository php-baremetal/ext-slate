/* Slate's recycled events-HAL objects: see halpool.h. */
#include "halpool.h"

#define KEEP 8

static bm_hal_lock_t *s_locks[KEEP];
static int s_nlocks;
static bm_hal_queue_t *s_queues[KEEP];
static int s_nqueues;

bm_hal_lock_t *slate_lock_get(void)
{
    return s_nlocks > 0 ? s_locks[--s_nlocks] : bm_hal_lock_create();
}

void slate_lock_put(bm_hal_lock_t *l)
{
    if (l && s_nlocks < KEEP) {
        s_locks[s_nlocks++] = l;   /* beyond KEEP it is lost: the HAL cannot free it */
    }
}

bm_hal_queue_t *slate_queue_get(void)
{
    return s_nqueues > 0 ? s_queues[--s_nqueues] : bm_hal_queue_create(SLATE_QUEUE_DEPTH, sizeof(uint32_t));
}

void slate_queue_put(bm_hal_queue_t *q)
{
    if (!q) {
        return;
    }
    uint32_t item;
    while (bm_hal_queue_receive(q, &item, 0)) {
    }
    if (s_nqueues < KEEP) {
        s_queues[s_nqueues++] = q;
    }
}

/* halpool.h - Slate's events-HAL objects (locks and queues), kept for reuse. Pure C, no ESP-IDF and no PHP.
   The events HAL has no way to delete a lock or a queue, so an engine or a command buffer that goes gives them
   back here and the next one takes them again: starting and stopping Slate any number of times creates them once.
   Called from the PHP task only (where engines and command buffers are created and freed). */
#ifndef SLATE_HALPOOL_H
#define SLATE_HALPOOL_H

#include <stdint.h>

#include "bm_events_hal.h"

#define SLATE_QUEUE_DEPTH 8   /* every Slate queue: this many uint32_t items */
/* The shortest wait (ms) that really blocks: the events HAL turns ms into ticks rounding down (pdMS_TO_TICKS), and
   with CONFIG_FREERTOS_HZ = 100 a wait of 1..9 ms is 0 ticks, a receive that returns at once. A loop that sleeps
   (rather than one bounded by a deadline) waits at least this much per turn, so it yields to the tasks it waits
   for and its count of turns is a time. */
#define SLATE_NAP_MS 10

/* A lock (a recycled one, else bm_hal_lock_create()); NULL when out of memory. */
bm_hal_lock_t *slate_lock_get(void);
void slate_lock_put(bm_hal_lock_t *l);   /* NULL allowed */
/* An empty queue of SLATE_QUEUE_DEPTH uint32_t (recycled, else created); NULL when out of memory. */
bm_hal_queue_t *slate_queue_get(void);
/* Gives a queue back (emptied here; nobody may use it any more). NULL allowed. */
void slate_queue_put(bm_hal_queue_t *q);

#endif

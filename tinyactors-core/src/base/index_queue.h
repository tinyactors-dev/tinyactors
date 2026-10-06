/* index_queue.h — intrusive FIFO of pool indices.
 *
 * {head, tail} lives wherever the queue is owned (instance header for the
 * internal queue, actor record for the mailbox, system header for the run
 * queue). The `next` link is a u32 at `link_off` inside each pool record.
 * Enqueue/dequeue are O(1) with zero allocation; a record can be in one
 * queue at a time (SCXML sends are unicast).
 */
#ifndef TINYACTORS_BASE_INDEX_QUEUE_H
#define TINYACTORS_BASE_INDEX_QUEUE_H

#include "base/pool.h"

typedef struct ta_index_queue {
    uint32_t head, tail;
} ta_index_queue;

TA_INLINE void ta_index_queue_init(ta_index_queue *queue) {
    queue->head = queue->tail = TA_NIL;
}
TA_INLINE int ta_index_queue_empty(const ta_index_queue *queue) {
    return queue->head == TA_NIL;
}

TA_INLINE uint32_t *ta_index_queue__link(const ta_pool *pool, uint32_t link_offset,
                                         uint32_t index) {
    return (uint32_t *)ta_pointer_add(ta_pool_at(pool, index), link_offset);
}

TA_INLINE void ta_index_queue_push(ta_index_queue *queue, const ta_pool *pool, uint32_t link_offset,
                                   uint32_t index) {
    *ta_index_queue__link(pool, link_offset, index) = TA_NIL;
    if (queue->tail == TA_NIL)
        queue->head = index;
    else
        *ta_index_queue__link(pool, link_offset, queue->tail) = index;
    queue->tail = index;
}

TA_INLINE uint32_t ta_index_queue_pop(ta_index_queue *queue, const ta_pool *pool,
                                      uint32_t link_offset) {
    uint32_t index = queue->head;
    if (index == TA_NIL) return TA_NIL;
    queue->head = *ta_index_queue__link(pool, link_offset, index);
    if (queue->head == TA_NIL) queue->tail = TA_NIL;
    return index;
}

#endif /* TINYACTORS_BASE_INDEX_QUEUE_H */

/* journal.c — a lossy ring of variable-size records (journal.h). */
#include "base/journal.h"
#include "base/memory.h"

static unsigned char *journal_storage(const ta_journal *journal) {
    return (unsigned char *)ta_pointer_add(journal, journal->storage_relative);
}

void ta_journal_init(ta_journal *journal, void *storage, uint32_t capacity) {
    TA_ASSERT(((uintptr_t)storage & (TA_JOURNAL_ALIGN - 1)) == 0);
    ta_memset(journal, 0, sizeof *journal);
    journal->storage_relative = (int64_t)((intptr_t)storage - (intptr_t)journal);
    journal->capacity = capacity & ~(TA_JOURNAL_ALIGN - 1);
}

void *ta_journal_reserve(ta_journal *journal, uint32_t size) {
    TA_ASSERT(!journal->reserved);
    size = ta_journal_size(size);
    uint32_t at;
    if (!size) return (void *)0;
    if (journal->wrapped) {
        if (journal->head - journal->tail < size) goto lost;
        at = journal->tail;
    } else if (journal->capacity - journal->tail >= size) {
        at = journal->tail;
    } else if (journal->head >= size) {
        at = 0; /* wrap: the bytes from tail to the end are skipped */
    } else {
        goto lost;
    }
    journal->reserved = size;
    journal->at = at;
    return journal_storage(journal) + at;
lost:
    journal->lost++;
    journal->lost_total++;
    return (void *)0;
}

void ta_journal_commit(ta_journal *journal, uint32_t size) {
    size = ta_journal_size(size);
    TA_ASSERT(size <= journal->reserved);
    if (size && journal->at != journal->tail) { /* the reservation wrapped */
        journal->end = journal->tail;
        journal->wrapped = 1;
        journal->tail = 0;
    }
    journal->tail += size;
    journal->used += size;
    journal->written_total += size ? 1u : 0u;
    journal->reserved = 0;
}

uint32_t ta_journal_peek(const ta_journal *journal, const unsigned char **bytes) {
    *bytes = journal_storage(journal) + journal->head;
    if (!journal->used) return 0;
    return (journal->wrapped ? journal->end : journal->tail) - journal->head;
}

void ta_journal_release(ta_journal *journal, uint32_t length) {
    TA_ASSERT(length <= journal->used);
    TA_ASSERT(length <= (journal->wrapped ? journal->end : journal->tail) - journal->head);
    journal->head += length;
    journal->used -= length;
    if (journal->wrapped && journal->head == journal->end) {
        journal->head = 0;
        journal->end = 0;
        journal->wrapped = 0;
    }
    if (!journal->wrapped && !journal->used && !journal->reserved)
        journal->head = journal->tail = 0; /* empty: the whole storage is in one piece again */
}

int ta_journal_copy(ta_journal *destination, void *storage, uint32_t capacity,
                    const ta_journal *source) {
    TA_ASSERT(!source->reserved);
    capacity &= ~(TA_JOURNAL_ALIGN - 1);
    if (capacity < source->used) return 0;
    const unsigned char *from = journal_storage(source);
    unsigned char *to = (unsigned char *)storage;
    uint32_t first = (source->wrapped ? source->end : source->tail) - source->head;
    uint32_t second = source->wrapped ? source->tail : 0;
    if (source->used) {
        ta_memmove(to, from + source->head, first);
        ta_memmove(to + first, from, second);
    }
    uint64_t lost_total = source->lost_total, written_total = source->written_total;
    uint32_t lost = source->lost, used = source->used;
    ta_journal_init(destination, storage, capacity);
    destination->tail = used;
    destination->used = used;
    destination->lost = lost;
    destination->lost_total = lost_total;
    destination->written_total = written_total;
    return 1;
}

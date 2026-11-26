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
    journal->size_offset = TA_NIL;
}

void ta_journal_retain(ta_journal *journal, uint32_t size_offset) {
    journal->size_offset = size_offset;
}

/* Nothing kept and nothing reserved: the whole storage is in one piece again. */
static void journal_settle(ta_journal *journal) {
    if (!journal->used && !journal->retained && !journal->reserved) {
        journal->head = journal->read = journal->tail = 0;
        journal->end = 0;
        journal->wrapped = 0;
    }
}

/* Every retained record goes: the oldest kept is the oldest pending. */
static void journal_drop_retained(ta_journal *journal) {
    if (journal->wrapped && journal->read < journal->head) { /* read is past the wrap */
        journal->end = 0;
        journal->wrapped = 0;
    }
    journal->head = journal->read;
    journal->retained = 0;
    journal_settle(journal);
}

/* The oldest retained record goes, by the size it holds; all of them when
 * that size cannot be a record's (a damaged region drops its history). */
static void journal_drop_oldest(ta_journal *journal) {
    uint32_t size = 0;
    const unsigned char *at = journal_storage(journal) + journal->head;
    if (journal->size_offset <= journal->capacity - 4u &&
        journal->head <= journal->capacity - 4u - journal->size_offset)
        ta_memcpy(&size, at + journal->size_offset, sizeof size);
    if (size < TA_JOURNAL_ALIGN || (size & (TA_JOURNAL_ALIGN - 1)) || size > journal->retained) {
        journal_drop_retained(journal);
        return;
    }
    journal->head += size;
    journal->retained -= size;
    if (journal->wrapped && journal->head == journal->end) {
        journal->head = 0;
        journal->end = 0;
        journal->wrapped = 0;
    }
    if (!journal->retained) journal_drop_retained(journal);
}

void *ta_journal_reserve_wrap(ta_journal *journal, uint32_t size) {
    uint32_t at;
    if (!size) return (void *)0;
    for (;;) {
        if (journal->wrapped) {
            if (journal->head - journal->tail >= size) {
                at = journal->tail;
                break;
            }
        } else if (journal->capacity - journal->tail >= size) {
            at = journal->tail;
            break;
        } else if (journal->head >= size) {
            at = 0; /* wrap: the bytes from tail to the end are skipped */
            break;
        }
        if (!journal->retained) goto lost;
        journal_drop_oldest(journal); /* history makes room for what is new */
    }
    journal->reserved = size;
    journal->at = at;
    return journal_storage(journal) + at;
lost:
    journal->lost++;
    journal->lost_total++;
    return (void *)0;
}

/* Whether `offset` lies in the records before the wrap. */
static int journal_before_wrap(const ta_journal *journal, uint32_t offset) {
    return journal->wrapped && offset >= journal->head;
}

uint32_t ta_journal_peek(const ta_journal *journal, const unsigned char **bytes) {
    *bytes = journal_storage(journal) + journal->read;
    if (!journal->used) return 0;
    return (journal_before_wrap(journal, journal->read) ? journal->end : journal->tail) -
           journal->read;
}

void ta_journal_kept_get(const ta_journal *journal, ta_journal_kept *out) {
    const unsigned char *storage = journal_storage(journal);
    out->first = storage + journal->head;
    out->second = storage;
    out->second_length = 0;
    if (!journal->used && !journal->retained) {
        out->first_length = 0;
        return;
    }
    if (journal->wrapped) { /* [head, end), then [0, tail) */
        out->first_length = journal->end - journal->head;
        out->second_length = journal->tail;
    } else {
        out->first_length = journal->tail - journal->head;
    }
}

void ta_journal_release(ta_journal *journal, uint32_t length) {
    TA_ASSERT(length <= journal->used);
    TA_ASSERT(length <=
              (journal_before_wrap(journal, journal->read) ? journal->end : journal->tail) -
                  journal->read);
    journal->read += length;
    journal->used -= length;
    journal->retained += length;
    if (journal->wrapped && journal->read == journal->end) journal->read = 0;
    if (journal->size_offset == TA_NIL) journal_drop_retained(journal);
    journal_settle(journal);
}

void ta_journal_clear_unused(ta_journal *journal) {
    unsigned char *storage = journal_storage(journal);
    if (!journal->used && !journal->retained) {
        ta_memset(storage, 0, journal->capacity);
        return;
    }
    if (journal->wrapped) { /* [tail, head) and [end, capacity) */
        ta_memset(storage + journal->tail, 0, journal->head - journal->tail);
        ta_memset(storage + journal->end, 0, journal->capacity - journal->end);
    } else { /* [0, head) and [tail, capacity) */
        ta_memset(storage, 0, journal->head);
        ta_memset(storage + journal->tail, 0, journal->capacity - journal->tail);
    }
}

int ta_journal_copy(ta_journal *destination, void *storage, uint32_t capacity,
                    const ta_journal *source) {
    TA_ASSERT(!source->reserved);
    capacity &= ~(TA_JOURNAL_ALIGN - 1);
    if (capacity < source->used) return 0;
    /* the retained records too when they fit, else the pending ones alone */
    uint32_t keep = capacity - source->used >= source->retained ? source->retained : 0;
    uint32_t start = keep ? source->head : source->read;
    const unsigned char *from = journal_storage(source);
    unsigned char *to = (unsigned char *)storage;
    int before = journal_before_wrap(source, start);
    uint32_t first = (before ? source->end : source->tail) - start;
    uint32_t second = before ? source->tail : 0;
    if (source->used || keep) {
        ta_memmove(to, from + start, first);
        ta_memmove(to + first, from, second);
    }
    uint64_t lost_total = source->lost_total, written_total = source->written_total;
    uint32_t lost = source->lost, used = source->used, size_offset = source->size_offset;
    ta_journal_init(destination, storage, capacity);
    destination->read = keep;
    destination->tail = keep + used;
    destination->used = used;
    destination->retained = keep;
    destination->lost = lost;
    destination->size_offset = size_offset;
    destination->lost_total = lost_total;
    destination->written_total = written_total;
    return 1;
}

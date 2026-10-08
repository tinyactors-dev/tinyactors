/* journal.h — a lossy ring of variable-size records.
 *
 * A writer reserves room for a record, fills it and commits it; a reader
 * peeks at the committed bytes and releases what it has read. A record is
 * always contiguous: when it does not fit before the end of the storage, the
 * ring wraps to the start, and the bytes left at the end are skipped. A
 * reservation the ring has no room for is refused and counted (`lost`), so a
 * writer never waits and never overwrites what the reader has not released.
 *
 * - Relocatable: the storage is a self-relative offset, so the journal and
 *   its storage move together with memcpy (snapshots, growth by relocation);
 *   ta_journal_copy moves the records into storage of another size.
 * - It knows nothing of what the records mean: the bytes are the writer's.
 *   Sizes and capacities are multiples of 8, so records stay 8-aligned.
 * - One reservation at a time: commit (or abandon) it before the next.
 */
#ifndef TINYACTORS_BASE_JOURNAL_H
#define TINYACTORS_BASE_JOURNAL_H

#include "base/config.h"

#define TA_JOURNAL_ALIGN 8u

typedef struct ta_journal {
    int64_t storage_relative; /* storage - (char *)journal */
    uint32_t capacity;        /* bytes, a multiple of TA_JOURNAL_ALIGN */
    uint32_t head;            /* the oldest committed byte */
    uint32_t tail;            /* where the next record goes */
    uint32_t end;             /* while wrapped: where the records before the wrap end */
    uint32_t wrapped;         /* 1: the records are [head, end) then [0, tail) */
    uint32_t reserved;        /* bytes of the reservation in progress, 0 if none */
    uint32_t at;              /* where the reservation in progress starts */
    uint32_t used;            /* committed bytes not yet released (skipped bytes not counted) */
    uint32_t lost;            /* reservations refused since ta_journal_take_lost */
    uint32_t pad;
    uint64_t lost_total;    /* reservations refused, ever */
    uint64_t written_total; /* records committed, ever */
} ta_journal;

TA_INLINE uint32_t ta_journal_size(uint32_t size) {
    return (uint32_t)ta_align_up(size, TA_JOURNAL_ALIGN);
}

/* `storage` must be TA_JOURNAL_ALIGN-aligned; `capacity` is rounded down to
 * a multiple of it. A capacity of 0 refuses every reservation. */
void ta_journal_init(ta_journal *journal, void *storage, uint32_t capacity);

/* Room for `size` bytes (rounded up to a multiple of 8), or NULL when the
 * ring has none: the reservation is then counted as lost. */
void *ta_journal_reserve(ta_journal *journal, uint32_t size);
/* Commit the first `size` bytes of the reservation (at most what was
 * reserved; rounded up to a multiple of 8). 0 abandons it. */
void ta_journal_commit(ta_journal *journal, uint32_t size);

/* The committed bytes that can be read in one piece: from the oldest record
 * to the end of the records before the wrap, or to the newest. Returns their
 * length (0 when there are none); `*bytes` points at them. Release them, and
 * peek again for the rest. */
uint32_t ta_journal_peek(const ta_journal *journal, const unsigned char **bytes);
/* Release `length` bytes from the start of what ta_journal_peek returned:
 * a whole number of records. */
void ta_journal_release(ta_journal *journal, uint32_t length);

/* Reservations refused since the last call, and reset that count. */
TA_INLINE uint32_t ta_journal_take_lost(ta_journal *journal) {
    uint32_t lost = journal->lost;
    journal->lost = 0;
    return lost;
}

/* Zero the storage that holds no committed record (released or skipped
 * bytes), so a copy of it carries no residue. */
void ta_journal_clear_unused(ta_journal *journal);

/* Copy the committed records into another storage, not overlapping the
 * source's (of any capacity that holds them: at least `used` bytes), oldest
 * first from its start. The counters move with them. Returns 0 if they do
 * not fit (nothing changes). */
int ta_journal_copy(ta_journal *destination, void *storage, uint32_t capacity,
                    const ta_journal *source);

#endif /* TINYACTORS_BASE_JOURNAL_H */

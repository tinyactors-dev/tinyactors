/* ta_journal: the lossy ring of variable-size records (src/base/journal.h):
 * writing and reading, wrapping, a full ring refusing and counting, moving
 * by memcpy and copying into storage of another size, and a random workload
 * against a plain queue. */
#include "../harness/harness.h"
#include "base/journal.h"

#include <stdlib.h>
#include <string.h>

/* A record: its size (a multiple of 8, at least 8) then a counter, then
 * bytes derived from the counter. */
static void fill(unsigned char *p, uint32_t size, uint32_t counter) {
    memcpy(p, &size, 4);
    memcpy(p + 4, &counter, 4);
    for (uint32_t i = 8; i < size; i++)
        p[i] = (unsigned char)(counter * 31u + i);
}

static int write_record(ta_journal *journal, uint32_t size, uint32_t counter) {
    unsigned char *p = ta_journal_reserve(journal, size);
    if (!p) return 0;
    size = ta_journal_size(size);
    fill(p, size, counter);
    ta_journal_commit(journal, size);
    return 1;
}

/* Reads every record in one peek; checks each; returns how many, and the
 * counters in `out`. */
static uint32_t read_piece(ta_journal *journal, uint32_t *out, uint32_t capacity,
                           uint32_t *bytes_read) {
    const unsigned char *bytes;
    uint32_t length = ta_journal_peek(journal, &bytes), at = 0, n = 0;
    while (at < length) {
        uint32_t size, counter;
        memcpy(&size, bytes + at, 4);
        memcpy(&counter, bytes + at + 4, 4);
        CHECK(size >= 8 && size % 8 == 0 && at + size <= length);
        if (size < 8 || at + size > length) break;
        for (uint32_t i = 8; i < size; i++)
            if (bytes[at + i] != (unsigned char)(counter * 31u + i)) {
                CHECK(0);
                break;
            }
        if (n < capacity) out[n] = counter;
        n++;
        at += size;
    }
    *bytes_read = at;
    return n;
}

static uint64_t storage_a[512], storage_b[1024];

static void test_write_and_read(void) {
    ta_journal journal;
    ta_journal_init(&journal, storage_a, 256);
    const unsigned char *bytes;
    CHECK_EQUAL(ta_journal_peek(&journal, &bytes), 0);
    CHECK(write_record(&journal, 16, 1));
    CHECK(write_record(&journal, 13, 2)); /* rounded up to 16 */
    CHECK(write_record(&journal, 40, 3));
    CHECK_EQUAL(journal.used, 72);
    CHECK_EQUAL(journal.written_total, 3);
    uint32_t got[8], read;
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 3);
    CHECK(got[0] == 1 && got[1] == 2 && got[2] == 3);
    CHECK_EQUAL(read, 72);
    ta_journal_release(&journal, 32); /* the first two */
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 1);
    CHECK_EQUAL(got[0], 3);
    ta_journal_release(&journal, read);
    CHECK_EQUAL(journal.used, 0);
    CHECK(journal.head == 0 && journal.tail == 0); /* empty: in one piece again */
    /* an abandoned reservation leaves nothing */
    CHECK(ta_journal_reserve(&journal, 64) != NULL);
    ta_journal_commit(&journal, 0);
    CHECK_EQUAL(ta_journal_peek(&journal, &bytes), 0);
    /* a reservation committed shorter */
    unsigned char *p = ta_journal_reserve(&journal, 64);
    fill(p, 24, 9);
    ta_journal_commit(&journal, 24);
    CHECK_EQUAL(journal.used, 24);
}

/* A record that does not fit before the end goes to the start; the reader
 * gets the records before the wrap, then the rest. */
static void test_wrapping(void) {
    ta_journal journal;
    ta_journal_init(&journal, storage_a, 128);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(write_record(&journal, 24, i)); /* 96 bytes */
    uint32_t got[8], read;
    ta_journal_release(&journal, 48);     /* records 0 and 1: [48, 96) left */
    CHECK(write_record(&journal, 32, 4)); /* fits before the end: [96, 128) */
    CHECK(write_record(&journal, 40, 5)); /* 0 bytes left at the end: wraps to [0, 40) */
    CHECK_EQUAL(journal.wrapped, 1);
    CHECK(!write_record(&journal, 16, 6)); /* [40, 48) is all that is free */
    CHECK(write_record(&journal, 8, 7));
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 3);
    CHECK(got[0] == 2 && got[1] == 3 && got[2] == 4);
    ta_journal_release(&journal, read);
    CHECK_EQUAL(journal.wrapped, 0);
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 2);
    CHECK(got[0] == 5 && got[1] == 7);
    ta_journal_release(&journal, read);
    CHECK_EQUAL(journal.used, 0);

    /* bytes skipped at the end are not records */
    ta_journal_init(&journal, storage_a, 128);
    CHECK(write_record(&journal, 104, 1)); /* [0, 104) */
    uint32_t r;
    read_piece(&journal, got, 8, &r);
    CHECK(write_record(&journal, 16, 2)); /* [104, 120) */
    ta_journal_release(&journal, 104);
    CHECK(write_record(&journal, 16, 3)); /* 8 bytes at the end skipped: [0, 16) */
    CHECK_EQUAL(journal.end, 120);
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 1);
    CHECK_EQUAL(got[0], 2);
    ta_journal_release(&journal, read);
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 1);
    CHECK_EQUAL(got[0], 3);
}

/* A full ring refuses and counts; it never overwrites. */
static void test_full(void) {
    ta_journal journal;
    ta_journal_init(&journal, storage_a, 64);
    CHECK(write_record(&journal, 32, 1));
    CHECK(write_record(&journal, 32, 2));
    CHECK(!write_record(&journal, 8, 3));
    CHECK(!write_record(&journal, 8, 4));
    CHECK_EQUAL(journal.lost, 2);
    CHECK_EQUAL(ta_journal_take_lost(&journal), 2);
    CHECK_EQUAL(journal.lost, 0);
    CHECK(!write_record(&journal, 72, 5)); /* larger than the ring */
    CHECK_EQUAL(journal.lost_total, 3);
    uint32_t got[4], read;
    CHECK_EQUAL(read_piece(&journal, got, 4, &read), 2);
    CHECK(got[0] == 1 && got[1] == 2);
    /* nothing to reserve in a ring of no capacity */
    ta_journal none;
    ta_journal_init(&none, storage_b, 0);
    CHECK(ta_journal_reserve(&none, 8) == NULL);
    CHECK_EQUAL(none.lost, 1);
    ta_journal_init(&none, storage_b, 13); /* rounded down to 8 */
    CHECK_EQUAL(none.capacity, 8);
}

/* The journal and its storage move together with memcpy. */
typedef struct block {
    ta_journal journal;
    uint64_t storage[16];
} block;

static void test_relocation(void) {
    static block a, b;
    ta_journal_init(&a.journal, a.storage, sizeof a.storage);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(write_record(&a.journal, 24, i));
    ta_journal_release(&a.journal, 48);
    CHECK(write_record(&a.journal, 48, 4)); /* wraps */
    memcpy(&b, &a, sizeof b);
    memset(&a, 0xAB, sizeof a); /* the old block is gone */
    uint32_t got[8], read;
    CHECK_EQUAL(read_piece(&b.journal, got, 8, &read), 2);
    CHECK(got[0] == 2 && got[1] == 3);
    ta_journal_release(&b.journal, read);
    CHECK_EQUAL(read_piece(&b.journal, got, 8, &read), 1);
    CHECK_EQUAL(got[0], 4);
}

/* Copying into storage of another size keeps the records, oldest first. */
static void test_copy(void) {
    ta_journal journal, bigger, smaller, tiny;
    ta_journal_init(&journal, storage_a, 128);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(write_record(&journal, 24, i));
    ta_journal_release(&journal, 48);
    CHECK(write_record(&journal, 32, 4));
    CHECK(write_record(&journal, 40, 5)); /* wrapped: 2, 3, 4 then 5 */
    CHECK(!write_record(&journal, 40, 6));
    CHECK(ta_journal_copy(&bigger, storage_b, 1024, &journal));
    CHECK_EQUAL(bigger.wrapped, 0);
    CHECK_EQUAL(bigger.used, 120);
    CHECK_EQUAL(bigger.lost, 1);
    CHECK_EQUAL(bigger.written_total, journal.written_total);
    uint32_t got[8], read;
    CHECK_EQUAL(read_piece(&bigger, got, 8, &read), 4);
    CHECK(got[0] == 2 && got[1] == 3 && got[2] == 4 && got[3] == 5);
    CHECK(write_record(&bigger, 512, 6)); /* the room is there now */
    static uint64_t small_storage[16];
    CHECK(ta_journal_copy(&smaller, small_storage, 120, &journal)); /* exactly what it holds */
    CHECK_EQUAL(read_piece(&smaller, got, 8, &read), 4);
    CHECK(!write_record(&smaller, 8, 7));
    CHECK(!ta_journal_copy(&tiny, small_storage, 112, &journal)); /* does not fit */
}

/* A random workload against a plain queue of what was written. */
static void test_random(void) {
    ta_journal journal;
    ta_journal_init(&journal, storage_b, 1000);
    uint32_t queue[4096], queue_head = 0, queue_tail = 0, counter = 0, lost = 0;
    srand(7);
    for (int step = 0; step < 20000; step++) {
        if (rand() % 3) {
            uint32_t size = 8u * (1u + (uint32_t)(rand() % 20));
            if (write_record(&journal, size, counter))
                queue[queue_tail++ % 4096] = counter;
            else
                lost++;
            counter++;
        } else {
            uint32_t got[256], read;
            uint32_t n = read_piece(&journal, got, 256, &read);
            uint32_t take = n ? (uint32_t)rand() % (n + 1) : 0, bytes = 0;
            const unsigned char *p;
            ta_journal_peek(&journal, &p);
            for (uint32_t i = 0; i < take; i++) {
                CHECK_EQUAL(got[i], queue[queue_head % 4096]);
                queue_head++;
                uint32_t size;
                memcpy(&size, p + bytes, 4);
                bytes += size;
            }
            ta_journal_release(&journal, bytes);
        }
        CHECK(queue_tail - queue_head < 4096);
    }
    CHECK_EQUAL(journal.lost_total, lost);
    CHECK(lost > 0); /* the workload does fill the ring */
}

/* Dead storage is zeroed for a snapshot; the records stay. */
static void test_clear_unused(void) {
    ta_journal journal;
    memset(storage_a, 0xEE, sizeof storage_a);
    ta_journal_init(&journal, storage_a, 128);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(write_record(&journal, 24, i));
    ta_journal_release(&journal, 48);
    CHECK(write_record(&journal, 32, 4));
    CHECK(write_record(&journal, 16, 5)); /* wrapped: [48, 128) then [0, 16) */
    ta_journal_clear_unused(&journal);
    const unsigned char *bytes = (const unsigned char *)storage_a;
    int dead_zero = 1;
    for (uint32_t i = 16; i < 48; i++)
        dead_zero &= bytes[i] == 0;
    CHECK(dead_zero);
    uint32_t got[8], read;
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 3);
    ta_journal_release(&journal, read);
    CHECK_EQUAL(read_piece(&journal, got, 8, &read), 1);
    CHECK_EQUAL(got[0], 5);
    ta_journal_release(&journal, read);
    ta_journal_clear_unused(&journal);
    dead_zero = 1;
    for (uint32_t i = 0; i < 128; i++)
        dead_zero &= bytes[i] == 0;
    CHECK(dead_zero);
}

int main(void) {
    RUN(test_write_and_read);
    RUN(test_wrapping);
    RUN(test_full);
    RUN(test_relocation);
    RUN(test_copy);
    RUN(test_clear_unused);
    RUN(test_random);
    TEST_MAIN_END();
}

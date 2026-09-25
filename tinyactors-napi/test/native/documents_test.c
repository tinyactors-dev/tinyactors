/* The cache of invoked child documents (napi ADR 0022). */
#include <string.h>

#include "allocator.h"
#include "check.h"
#include "documents.h"

static ta_bytes text(const char *value) {
    return (ta_bytes){ .ptr = (const uint8_t *)value, .len = (uint32_t)strlen(value) };
}

/* Reserves, then commits, `value` as `definition`. */
static void add(tna_documents *documents, const char *value, ta_definition_id definition) {
    uint32_t entry = 0;
    CHECK(tna_documents_reserve(documents, text(value), &entry) == TA_OK);
    ta_definition_id found = 0;
    CHECK(!tna_documents_find(documents, text(value), &found)); /* a reserved entry is not found */
    tna_documents_commit(documents, entry, definition);
}

static void hits_and_misses(void) {
    tna_allocator allocator;
    tna_allocator_init(&allocator);
    tna_documents documents;
    tna_documents_init(&documents, tna_allocator_for(&allocator, TNA_MEMORY_OTHER),
                       (tna_documents_limits){ .max_count = 8, .max_bytes = 1024 });
    ta_definition_id found = 0;
    CHECK(!tna_documents_find(&documents, text("<scxml/>"), &found));
    add(&documents, "<scxml/>", 7);
    CHECK(tna_documents_find(&documents, text("<scxml/>"), &found) && found == 7);
    CHECK(!tna_documents_find(&documents, text("<scxml />"), &found));
    add(&documents, "", 9); /* an empty document is a document too */
    CHECK(tna_documents_find(&documents, text(""), &found) && found == 9);
    CHECK(documents.count == 2 && documents.bytes == 8);
    tna_documents_destroy(&documents);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

static uint64_t one_hash_for_all(ta_bytes bytes) {
    (void)bytes;
    return 42;
}

/* Entries with the same hash chain; each is still found by its bytes. */
static void same_hash(void) {
    tna_allocator allocator;
    tna_allocator_init(&allocator);
    tna_documents documents;
    tna_documents_init(&documents, tna_allocator_for(&allocator, TNA_MEMORY_OTHER),
                       (tna_documents_limits){ .max_count = 2, .max_bytes = 1024 });
    documents.hash = one_hash_for_all;
    add(&documents, "a", 1);
    add(&documents, "b", 2);
    add(&documents, "c", 3);
    ta_definition_id found = 0;
    CHECK(tna_documents_find(&documents, text("a"), &found) && found == 1);
    CHECK(tna_documents_find(&documents, text("b"), &found) && found == 2);
    CHECK(tna_documents_find(&documents, text("c"), &found) && found == 3);
    CHECK(!tna_documents_find(&documents, text("d"), &found));
    /* Evicting from a chain keeps the rest of it reachable. */
    ta_definition_id evicted = 0;
    CHECK(tna_documents_evict(&documents, &evicted) && evicted == 1);
    CHECK(!tna_documents_find(&documents, text("a"), &found));
    CHECK(tna_documents_find(&documents, text("b"), &found) && found == 2);
    CHECK(tna_documents_find(&documents, text("c"), &found) && found == 3);
    CHECK(tna_documents_forget(&documents, 3));
    CHECK(tna_documents_find(&documents, text("b"), &found) && found == 2);
    tna_documents_destroy(&documents);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

static void evicts_least_recently_used(void) {
    tna_allocator allocator;
    tna_allocator_init(&allocator);
    tna_documents documents;
    tna_documents_init(&documents, tna_allocator_for(&allocator, TNA_MEMORY_OTHER),
                       (tna_documents_limits){ .max_count = 2, .max_bytes = 1024 });
    ta_definition_id found = 0;
    ta_definition_id evicted = 0;
    add(&documents, "one", 1);
    add(&documents, "two", 2);
    CHECK(!tna_documents_evict(&documents, &evicted));
    CHECK(tna_documents_find(&documents, text("one"), &found)); /* "one" is now the newest */
    add(&documents, "three", 3);
    CHECK(tna_documents_evict(&documents, &evicted) && evicted == 2);
    CHECK(!tna_documents_evict(&documents, &evicted));
    CHECK(!tna_documents_find(&documents, text("two"), &found));
    CHECK(tna_documents_find(&documents, text("one"), &found) && found == 1);
    CHECK(tna_documents_find(&documents, text("three"), &found) && found == 3);
    /* A lower bound evicts down to it, oldest first, never the newest. */
    tna_documents_set_limits(&documents, (tna_documents_limits){ .max_count = 1, .max_bytes = 1024 });
    CHECK(tna_documents_evict(&documents, &evicted) && evicted == 1);
    CHECK(!tna_documents_evict(&documents, &evicted));
    CHECK(documents.count == 1);
    tna_documents_destroy(&documents);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

static void evicts_by_bytes(void) {
    tna_allocator allocator;
    tna_allocator_init(&allocator);
    tna_documents documents;
    tna_documents_init(&documents, tna_allocator_for(&allocator, TNA_MEMORY_OTHER),
                       (tna_documents_limits){ .max_count = 100, .max_bytes = 10 });
    ta_definition_id evicted = 0;
    add(&documents, "12345", 1);
    add(&documents, "67890", 2);
    CHECK(!tna_documents_evict(&documents, &evicted));
    add(&documents, "abc", 3);
    CHECK(tna_documents_evict(&documents, &evicted) && evicted == 1);
    CHECK(!tna_documents_evict(&documents, &evicted));
    /* One document past the byte bound stays, alone. */
    add(&documents, "a document longer than ten bytes", 4);
    CHECK(tna_documents_evict(&documents, &evicted) && evicted == 2);
    CHECK(tna_documents_evict(&documents, &evicted) && evicted == 3);
    CHECK(!tna_documents_evict(&documents, &evicted));
    ta_definition_id found = 0;
    CHECK(tna_documents_find(&documents, text("a document longer than ten bytes"), &found) && found == 4);
    tna_documents_destroy(&documents);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

static void reserve_cancel_and_forget(void) {
    tna_allocator allocator;
    tna_allocator_init(&allocator);
    tna_documents documents;
    tna_documents_init(&documents, tna_allocator_for(&allocator, TNA_MEMORY_OTHER),
                       (tna_documents_limits){ .max_count = 4, .max_bytes = 1024 });
    uint32_t entry = 0;
    CHECK(tna_documents_reserve(&documents, text("refused"), &entry) == TA_OK);
    tna_documents_cancel(&documents, entry);
    ta_definition_id found = 0;
    CHECK(!tna_documents_find(&documents, text("refused"), &found));
    CHECK(documents.count == 0 && documents.bytes == 0);
    add(&documents, "kept", 5);
    add(&documents, "retired", 6);
    CHECK(tna_documents_forget(&documents, 6));
    CHECK(!tna_documents_forget(&documents, 6));
    CHECK(!tna_documents_find(&documents, text("retired"), &found));
    CHECK(tna_documents_find(&documents, text("kept"), &found) && found == 5);
    /* Freed entries are reused. */
    uint32_t count = documents.entries.count;
    add(&documents, "again", 7);
    CHECK(documents.entries.count == count);
    tna_documents_destroy(&documents);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

int main(void) {
    hits_and_misses();
    same_hash();
    evicts_least_recently_used();
    evicts_by_bytes();
    reserve_cancel_and_forget();
    puts("documents_test passed");
    return 0;
}

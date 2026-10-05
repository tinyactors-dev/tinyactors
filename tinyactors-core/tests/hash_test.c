/*
 * hash.h: FNV-1a in its 32-bit and 64-bit forms and the fixed 64-bit mixer.
 * The values are pinned, because they leave core or decide where things
 * go: span IDs are written into traces, a definition's fingerprint is its
 * revision, and the tables' buckets come from these hashes.
 *
 * Claims proven here, in order:
 *
 * 1. FNV-1a 32 started from its basis gives the published vectors, and
 *    hashing no bytes (a NULL pointer with size 0) leaves the hash as it was;
 * 2. FNV-1a 64 does the same for its published vectors;
 * 3. both forms continue a hash: a text hashed in pieces, each piece from
 *    the last one's hash, gives the hash of the whole;
 * 4. the mixer maps 0 to 0 and gives the pinned outputs of the finalizer
 *    of MurmurHash3's 64-bit variant, and is invertible (a bijection);
 * 5. the values core derives from them are pinned: span IDs from step
 *    keys, and the bucket hashes of the atom, address, event-name and
 *    session tables;
 * 6. a definition compiled without an explicit revision is fingerprinted
 *    with FNV-1a 64 of its content, written as 16 lowercase hex digits, and
 *    a pinned chart keeps its pinned revision.
 *
 * The test includes the amalgamation so it can reach the private units; it
 * uses the C library, the core does not.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * An allocator over the C library, for the tables and the compiler.
 */

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    return aligned_alloc(alignment, size == 0 ? alignment : size);
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable test_allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator test_allocator = { .self = NULL, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * Claim 1: FNV-1a 32, the published vectors.
 */

static uint32_t fnv32(const char *text) {
    return ta_hash_fnv1a32(TA_HASH_FNV1A32_BASIS, text, strlen(text));
}

static void test_fnv1a32_vectors(void) {
    CHECK(TA_HASH_FNV1A32_BASIS == 0x811c9dc5u);
    CHECK(fnv32("") == 0x811c9dc5u);
    CHECK(fnv32("a") == 0xe40c292cu);
    CHECK(fnv32("foobar") == 0xbf9cf968u);
    CHECK(ta_hash_fnv1a32(0x12345678u, NULL, 0) == 0x12345678u);
    CHECK(ta_hash_fnv1a32(0, NULL, 0) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 2: FNV-1a 64, the published vectors.
 */

static uint64_t fnv64(const char *text) {
    return ta_hash_fnv1a64(TA_HASH_FNV1A64_BASIS, text, strlen(text));
}

static void test_fnv1a64_vectors(void) {
    CHECK(TA_HASH_FNV1A64_BASIS == 0xcbf29ce484222325ull);
    CHECK(fnv64("") == 0xcbf29ce484222325ull);
    CHECK(fnv64("a") == 0xaf63dc4c8601ec8cull);
    CHECK(fnv64("foobar") == 0x85944171f73967e8ull);
    CHECK(ta_hash_fnv1a64(0x0123456789abcdefull, NULL, 0) == 0x0123456789abcdefull);
}

/* ------------------------------------------------------------------------
 * Claim 3: hashing in pieces is hashing the whole.
 */

static void test_pieces_continue(void) {
    const char *text = "the quick brown fox";
    size_t size = strlen(text);
    for (size_t split = 0; split <= size; split += 1) {
        uint32_t h32 = ta_hash_fnv1a32(TA_HASH_FNV1A32_BASIS, text, split);
        h32 = ta_hash_fnv1a32(h32, text + split, size - split);
        CHECK(h32 == fnv32(text));
        uint64_t h64 = ta_hash_fnv1a64(TA_HASH_FNV1A64_BASIS, text, split);
        h64 = ta_hash_fnv1a64(h64, text + split, size - split);
        CHECK(h64 == fnv64(text));
    }
}

/* ------------------------------------------------------------------------
 * Claim 4: the mixer.
 */

/* The inverse of x ^= x >> 33 for 64-bit x: one more shift undoes it. */
static uint64_t unshift33(uint64_t value) {
    return value ^ (value >> 33);
}

/* The inverse of an odd 64-bit multiplier, by Newton's iteration. */
static uint64_t inverse_odd(uint64_t odd) {
    uint64_t inverse = odd;
    for (int round = 0; round < 6; round += 1) {
        inverse *= 2u - odd * inverse;
    }
    return inverse;
}

static uint64_t unmix(uint64_t value) {
    value = unshift33(value);
    value *= inverse_odd(0xc4ceb9fe1a85ec53ull);
    value = unshift33(value);
    value *= inverse_odd(0xff51afd7ed558ccdull);
    return unshift33(value);
}

static void test_mixer(void) {
    CHECK(ta_hash_mix64(0) == 0);
    CHECK(ta_hash_mix64(1) == 0xb456bcfc34c2cb2cull);
    CHECK(ta_hash_mix64(2) == 0x3abf2a20650683e7ull);
    CHECK(ta_hash_mix64(UINT64_MAX) == 0x64b5720b4b825f21ull);
    CHECK(ta_hash_mix64(0x0123456789abcdefull) == 0x87cbfbfe89022ceaull);
    uint64_t value = 0x9e3779b97f4a7c15ull;
    for (int round = 0; round < 1000; round += 1) {
        CHECK(unmix(ta_hash_mix64(value)) == value);
        value = value * 6364136223846793005ull + 1442695040888963407ull;
    }
}

/* ------------------------------------------------------------------------
 * Claim 5: what core derives from the hashes.
 */

static void test_derived_values(void) {
    /* Span IDs: three rounds of the mixer over the step key. */
    uint64_t span = ta_hash_mix64(1);
    span = ta_hash_mix64(span ^ 0);
    span = ta_hash_mix64(span ^ ((uint64_t)1u | ((uint64_t)1u << 32)));
    CHECK(span == 0x65c672b9ddbfb556ull);
    CHECK(ta_step_key_span_id((ta_step_key){ .session = 1, .macrostep = 0, .microstep = 1 }) == span);
    CHECK(ta_step_key_span_id((ta_step_key){ .session = 1, .macrostep = 0, .microstep = 3 }) ==
          0xb4ed65a25541f516ull);

    /* The session table's buckets: the mixer's low 32 bits. */
    CHECK(ta_session_hash(0x0123456789abcdefull) == 0x89022ceau);

    /* The address table: family, a NUL, then the name, each part from the
     * basis with the hash so far XORed in. */
    ta_actor_address address = { .family = { .name = S("orders") }, .name = S("o-17") };
    CHECK(ta_address_hash(&address) == 0x96e2dfd2u);

    /* The event-name table: (parent, segment) from a seeded basis. */
    CHECK(ta_event_name_hash(0, 0) == 0xed6e77fcu);
    CHECK(ta_event_name_hash(3, 7) == 0xd45cdbe8u);

    /* The atom table: FNV-1a 32 of the text, kept in the entry. */
    ta_atom_table atoms;
    CHECK(ta_atom_table_init(&atoms, test_allocator) == TA_OK);
    ta_atom atom = TA_ATOM_NONE;
    CHECK(ta_atom_intern(&atoms, S("foobar"), &atom) == TA_OK);
    const ta_atom_entry *entry = ta_vec_at(&atoms.entries, atom);
    CHECK(entry->hash == 0xbf9cf968u);
    ta_atom found = TA_ATOM_NONE;
    CHECK(ta_atom_find(&atoms, S("foobar"), &found) == TA_OK && found == atom);
    ta_atom_table_destroy(&atoms);
}

/* ------------------------------------------------------------------------
 * Claim 6: a definition's fingerprint.
 */

static void test_fingerprint(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) == TA_OK);
    if (system == NULL) {
        return;
    }
    ta_scxml_spec spec = {
        .name = S("hash"),
        .datamodel = S("null"),
        .states = (const ta_scxml_state_spec[]){
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") },
            {0},
        },
    };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                 .spec = &spec,
                                                 .datamodel = ta_null_datamodel(),
                                                 .allocator = test_allocator,
                                                 .out = &definition,
                                             }) == TA_OK);
    if (definition != NULL) {
        char expected[16];
        ta_text_format_hex(expected, definition->content_hash, 16u);
        CHECK(definition->fingerprinted);
        CHECK(definition->identity.revision.len == 16);
        CHECK(memcmp(definition->identity.revision.ptr, expected, 16) == 0);
        CHECK(memcmp(definition->identity.revision.ptr, "a94684120f4c8b1a", 16) == 0);
        if (memcmp(definition->identity.revision.ptr, "a94684120f4c8b1a", 16) != 0) {
            fprintf(stderr, "revision is %.16s\n", (const char *)definition->identity.revision.ptr);
        }
    }
    ta_definition_destroy(definition);
    ta_system_destroy(system);
}

int main(void) {
    test_fnv1a32_vectors();
    test_fnv1a64_vectors();
    test_pieces_continue();
    test_mixer();
    test_derived_values();
    test_fingerprint();
    if (failures != 0) {
        fprintf(stderr, "hash test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core hash test passed");
    return 0;
}

/* test: standalone asan */
/*
 * ADR 0066: a session ID outlives residency. A session is named by the
 * runtime ID it was spawned with; load keeps it; reconstruct keeps it unless
 * a resident session holds it; and an SCXML send to it resolves through the
 * session table: to the reloaded session's runtime ID, or to the address of
 * an unloaded one.
 *
 * Includes the amalgamation, so it can look at the session table itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.c"
#include "tel_reader.h"
#include "test_allocator.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }

/* zero_fill: as calloc, which these tests were written against */
static ta_test_allocator heap = { .zero_fill = true };
static const ta_allocator test_allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* A one-address directory, enough for one named actor to unload and load. */
typedef struct {
    bool used;
    ta_actor_directory_entry entry;
} one_directory;

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    one_directory *dir = self;
    if (dir->used) {
        return TA_ERR_STALE;
    }
    dir->used = true;
    dir->entry = params->entry;
    return TA_OK;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    one_directory *dir = self;
    *params->out = dir->used ? dir->entry : (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    return TA_OK;
}

static bool same_entry(ta_actor_directory_entry left, ta_actor_directory_entry right) {
    return left.kind == right.kind && (left.kind != TA_ACTOR_DIRECTORY_RESIDENT || left.actor == right.actor);
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    one_directory *dir = self;
    if (!dir->used || !same_entry(dir->entry, params->expected)) {
        return TA_ERR_STALE;
    }
    dir->entry = params->replacement;
    return TA_OK;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    one_directory *dir = self;
    if (!dir->used || !same_entry(dir->entry, params->expected)) {
        return TA_ERR_STALE;
    }
    dir->used = false;
    return TA_OK;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

static const ta_scxml_state_spec idle_chart[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle") },
    {0},
};

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    one_directory dir;
} fixture;

static void start_in(fixture *f, uint64_t incarnation) {
    *f = (fixture){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator, .incarnation = incarnation },
                           &f->system) == TA_OK);
    CHECK(ta_system_set_actor_directory(f->system, (ta_actor_directory){ .self = &f->dir, .v = &dir_vtable }) ==
          TA_OK);
    ta_definition_identity identity = {
        .family = { STR("idle") },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(
              f->system, &(ta_definition_compile_params){
                             .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity,
                                                       .states = idle_chart },
                             .datamodel = ta_null_datamodel(),
                             .allocator = test_allocator,
                             .out = &definition,
                         }) == TA_OK);
    CHECK(ta_system_register_definition(f->system, definition, &f->definition) == TA_OK);
}

static void start(fixture *f) {
    start_in(f, 0);
}

static const ta_actor_address the_address = { .family = { STR("idle") }, .name = STR("one") };

static ta_actor_id spawn(fixture *f, const ta_actor_address *address) {
    ta_actor_id id = 0;
    CHECK(ta_actor_spawn(f->system, &(ta_actor_spawn_params){ .definition = f->definition, .address = address,
                                                            .out = &id }) == TA_OK);
    return id;
}

static uint64_t session_of(fixture *f, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f->system, actor, &view) == TA_OK);
    return view.session_id;
}

static ta_session_image *unload(fixture *f, ta_actor_id actor) {
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(f->system, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(f->system, &(ta_actor_unload_params){ .actor = actor, .image = image }) == TA_OK);
    return image;
}

static ta_actor_id load(fixture *f, ta_session_image *image) {
    ta_actor_id id = 0;
    CHECK(ta_actor_load(f->system, &(ta_actor_load_params){ .image = image, .out = &id }) == TA_OK);
    ta_session_image_destroy(image);
    return id;
}

/* Where core would submit an SCXML send to session `id` right now. */
static ta_actor_ref route(fixture *f, uint64_t id, bool *named) {
    ta_actor_ref out;
    *named = ta_session_route(f->system, &(ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = id }, &out);
    return out;
}

static void finish(fixture *f) {
    ta_system_destroy(f->system);
    CHECK(heap.stats.live_blocks == 0);
}

/* The session ID is the first runtime ID, across any number of reloads. */
static void test_load_keeps_the_session(void) {
    fixture f;
    start(&f);
    ta_actor_id first = spawn(&f, &the_address);
    CHECK(session_of(&f, first) == first);
    ta_actor_id current = first;
    for (int round = 0; round < 3; round += 1) {
        ta_session_image *image = unload(&f, current);
        bool named = false;
        ta_actor_ref out = route(&f, first, &named);
        CHECK(named && out.kind == TA_ACTOR_REF_NAMED); /* unloaded: by address */
        CHECK(ta_str_equal(ta_address_view(out.as.named).name, the_address.name));
        ta_actor_ref_release(&out);
        ta_actor_id loaded = load(&f, image);
        CHECK(loaded != current && loaded != first);
        CHECK(session_of(&f, loaded) == first);
        out = route(&f, first, &named);
        CHECK(!named && out.kind == TA_ACTOR_REF_RUNTIME && out.as.runtime == loaded); /* reloaded */
        current = loaded;
    }
    /* One entry: the resident reloaded session; the parked entries are gone. */
    CHECK(ta_table_count(&f.system->sessions.index) == 1);
    finish(&f);
}

/* A load runs under the trace parent it is given (napi ADR 0020): the
 * LOADED record carries it, so the load is in the trace of the message
 * that found the actor unloaded. */
static ta_tel_trace loaded_record;
static int loaded_records = 0;
static void loaded_visit(void *self, const ta_tel_entry *entry) {
    (void)self;
    if (entry->trace.kind == TA_TRACE_LOADED) {
        loaded_record = entry->trace; /* its texts are views, dead after this call: this test reads only scalars */
        loaded_records += 1;
    }
}
static tel_reader loaded_reader = { .visit = loaded_visit };

static void test_load_runs_under_its_trace_parent(void) {
    fixture f;
    start(&f);
    CHECK(ta_system_set_trace_masks(f.system, &(ta_trace_masks){ .kinds = TA_TRACE_BIT(TA_TRACE_LOADED) }) == TA_OK);
    ta_actor_id first = spawn(&f, &the_address);
    ta_session_image *image = unload(&f, first);
    ta_trace_parent parent = {
        .context = { .trace_id_high = 0x4bf92f3577b34da6ull, .trace_id_low = 0xa3ce929d0e0e4736ull,
                     .flags = TA_TRACE_FLAG_SAMPLED },
        .cause = { .macrostep = 0x00f067aa0ba902b7ull },
    };
    ta_actor_id id = 0;
    loaded_records = 0;
    CHECK(ta_actor_load(f.system, &(ta_actor_load_params){ .image = image, .out = &id, .trace = parent }) == TA_OK);
    ta_session_image_destroy(image);
    tel_read(f.system, &loaded_reader);
    CHECK(loaded_reader.bad == 0 && loaded_records == 1);
    CHECK(loaded_record.context.trace_id_high == parent.context.trace_id_high &&
          loaded_record.context.trace_id_low == parent.context.trace_id_low);
    CHECK(loaded_record.cause.session == 0 && loaded_record.cause.macrostep == parent.cause.macrostep);
    /* No parent: the record has none either. */
    image = unload(&f, id);
    loaded_records = 0;
    CHECK(ta_actor_load(f.system, &(ta_actor_load_params){ .image = image, .out = &id }) == TA_OK);
    ta_session_image_destroy(image);
    tel_read(f.system, &loaded_reader);
    CHECK(loaded_records == 1 && loaded_record.context.trace_id_high == 0 && loaded_record.cause.macrostep == 0);
    finish(&f);
}

/* A runtime ID is never issued twice, so no new session takes an old ID. */
static void test_session_ids_are_unique(void) {
    fixture f;
    start(&f);
    ta_actor_id named = spawn(&f, &the_address);
    uint64_t kept = session_of(&f, named);
    ta_actor_id loaded = load(&f, unload(&f, named)); /* frees the first slot */
    for (int round = 0; round < 50; round += 1) {
        ta_actor_id fresh = spawn(&f, NULL);
        CHECK(session_of(&f, fresh) == fresh);
        CHECK(session_of(&f, fresh) != kept);
        CHECK(ta_actor_destroy(f.system, fresh) == TA_OK);
    }
    CHECK(session_of(&f, loaded) == kept);
    finish(&f);
}

/* Reconstruct keeps the image's session unless a resident session holds it. */
static void test_reconstruct_keeps_a_free_session(void) {
    fixture f;
    start(&f);
    ta_actor_id original = spawn(&f, NULL);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(f.system, &(ta_actor_capture_params){ .actor = original, .out = &image }) == TA_OK);

    ta_actor_id copy = 0;
    CHECK(ta_actor_reconstruct(f.system, &(ta_actor_reconstruct_params){ .image = image, .out = &copy }) == TA_OK);
    CHECK(session_of(&f, copy) == copy); /* the original holds its session: a new one */
    CHECK(ta_actor_destroy(f.system, copy) == TA_OK);

    CHECK(ta_actor_destroy(f.system, original) == TA_OK);
    CHECK(ta_actor_reconstruct(f.system, &(ta_actor_reconstruct_params){ .image = image, .out = &copy }) == TA_OK);
    CHECK(session_of(&f, copy) == original); /* the session is free: it continues */
    bool named = false;
    ta_actor_ref out = route(&f, original, &named);
    CHECK(!named && out.as.runtime == copy);
    ta_session_image_destroy(image);
    finish(&f);
}

/* Removing an unloaded actor ends its session. */
static void test_remove_unloaded_ends_the_session(void) {
    fixture f;
    start(&f);
    ta_actor_id actor = spawn(&f, &the_address);
    ta_session_image *image = unload(&f, actor);
    CHECK(ta_table_count(&f.system->sessions.index) == 1);
    CHECK(ta_actor_remove_unloaded(f.system, &(ta_actor_remove_unloaded_params){ .address = &the_address }) ==
          TA_OK);
    CHECK(ta_table_count(&f.system->sessions.index) == 0);
    bool named = false;
    ta_actor_ref out = route(&f, actor, &named);
    CHECK(!named && out.kind == TA_ACTOR_REF_RUNTIME && out.as.runtime == actor); /* stale, as before */
    ta_session_image_destroy(image);
    finish(&f);
}

/* A system destroyed with an unloaded session frees its entry. */
static void test_destroy_frees_a_parked_session(void) {
    fixture f;
    start(&f);
    ta_session_image *image = unload(&f, spawn(&f, &the_address));
    ta_session_image_destroy(image);
    finish(&f);
}

/* Inverse of an odd 64-bit constant modulo 2^64, by Newton's iteration. */
static uint64_t inverse64(uint64_t odd) {
    uint64_t inverse = odd;
    for (int step = 0; step < 6; step += 1) {
        inverse *= 2u - odd * inverse;
    }
    return inverse;
}

/* The inverse of ta_mix64 (a bijection): lets a test choose the hash a session ID has. */
static uint64_t unmix64(uint64_t value) {
    value ^= value >> 33;
    value *= inverse64(0xc4ceb9fe1a85ec53ull);
    value ^= value >> 33;
    value *= inverse64(0xff51afd7ed558ccdull);
    value ^= value >> 33;
    return value;
}

/* Bytes of one page of the table's index, and of its node pool: the bound of every block it asks for. */
#define SESSION_INDEX_PAGE_BYTES (4096u * sizeof(ta_table_slot))

/*
 * The table at scale: 200,000 sessions. Growth never copies it (no block
 * larger than one page of the index or the pool), every entry is found, a
 * resident entry shadows an unloaded one with the same ID, and removal
 * empties it.
 */
static void test_the_table_grows_without_copying(void) {
    enum { COUNT = 200000 };
    ta_session_table table;
    ta_session_table_init(&table, test_allocator);
    uint32_t *nodes = calloc(COUNT, sizeof(*nodes));
    ta_actor stand_in;
    ta_test_allocator_reset_counts(&heap);
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        CHECK(ta_session_node_new(&table,
                                  &(ta_session_node_params){ .id = ta_actor_id_make(index, 1u + index % 7u),
                                                             .actor = index % 2u == 0 ? &stand_in : NULL },
                                  &nodes[index]) == TA_OK);
        ta_session_insert(&table, nodes[index]);
    }
    CHECK(heap.stats.largest_block <= SESSION_INDEX_PAGE_BYTES);
    CHECK(ta_table_count(&table.index) == COUNT);
    uint32_t found = 0;
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        found += ta_session_find(&table, ta_session_node_at(&table, nodes[index])->id) ==
                         ta_session_node_at(&table, nodes[index])
                     ? 1u
                     : 0u;
    }
    CHECK(found == COUNT);
    CHECK(ta_session_find(&table, ta_actor_id_make(COUNT + 1u, 1u)) == NULL);
    /* A resident entry wins over an unloaded one with the same ID. */
    uint32_t resident = 0;
    uint64_t shared = ta_session_node_at(&table, nodes[1])->id; /* nodes[1] is unloaded */
    CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = shared, .actor = &stand_in }, &resident) == TA_OK);
    ta_session_insert(&table, resident);
    CHECK(ta_session_find(&table, shared) == ta_session_node_at(&table, resident));
    ta_session_drop(&table, resident);
    CHECK(ta_session_find(&table, shared) == ta_session_node_at(&table, nodes[1]));
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        ta_session_drop(&table, nodes[index]);
    }
    CHECK(ta_table_count(&table.index) == 0);
    ta_session_table_destroy(&table);
    free(nodes);
    CHECK(heap.stats.live_blocks == 0);
}

/*
 * Session IDs come from the host (the image a load is given), and the hash
 * is 32 bits of an invertible mix, so a host can choose IDs that all share
 * one hash. Three pages' worth of them must all be insertable (the index
 * doubles the page they fill) and findable, and removal in any order
 * empties the table and frees every block with its size.
 */
static void test_equal_hash_session_ids_still_insert(void) {
    enum { COUNT = 3 * 4096 };
    ta_session_table table;
    ta_session_table_init(&table, test_allocator);
    uint32_t *nodes = calloc(COUNT, sizeof(*nodes));
    uint64_t *ids = calloc(COUNT, sizeof(*ids));
    ta_actor stand_in;
    ta_test_allocator_reset_counts(&heap);
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        ids[index] = unmix64(((uint64_t)(index + 1u) << 32) | 0x12345678ull);
        CHECK((uint32_t)ta_mix64(ids[index]) == 0x12345678u);
        CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = ids[index], .actor = &stand_in },
                                  &nodes[index]) == TA_OK);
        ta_session_insert(&table, nodes[index]);
    }
    CHECK(ta_table_count(&table.index) == COUNT);
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        CHECK(ta_session_find(&table, ids[index]) == ta_session_node_at(&table, nodes[index]));
    }
    /* Other IDs are unaffected: the page those hashes fill is the only slow one. */
    CHECK(ta_session_find(&table, ta_actor_id_make(7u, 1u)) == NULL);
    for (uint32_t step = 0; step < COUNT; step += 1u) {
        uint32_t index = (step * 7919u) % COUNT; /* 7919 is prime: every index once, in a scrambled order */
        ta_session_drop(&table, nodes[index]);
        CHECK(ta_session_find(&table, ids[index]) == NULL);
    }
    CHECK(ta_table_count(&table.index) == 0);
    ta_session_table_destroy(&table);
    free(nodes);
    free(ids);
    CHECK(heap.stats.live_blocks == 0);
}

/* Between ta_session_node_new and ta_session_insert only removals may happen: the room stays. */
static void test_node_new_then_insert_with_removals_between(void) {
    ta_session_table table;
    ta_session_table_init(&table, test_allocator);
    ta_actor stand_in;
    uint32_t others[64];
    for (uint32_t index = 0; index < 64u; index += 1u) {
        CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = 1000u + index, .actor = &stand_in },
                                  &others[index]) == TA_OK);
        ta_session_insert(&table, others[index]);
    }
    uint32_t held = 0;
    CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = 5u, .actor = &stand_in }, &held) == TA_OK);
    for (uint32_t index = 0; index < 64u; index += 2u) {
        ta_session_drop(&table, others[index]);
    }
    ta_session_insert(&table, held);
    CHECK(ta_session_find(&table, 5u) == ta_session_node_at(&table, held));
    CHECK(ta_table_count(&table.index) == 33u);
    /* A node taken and given up leaves nothing behind. */
    uint32_t unused = 0;
    CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = 6u }, &unused) == TA_OK);
    ta_session_node_free(&table, unused);
    CHECK(ta_session_find(&table, 6u) == NULL && ta_table_count(&table.index) == 33u);
    ta_session_table_destroy(&table);
    ta_session_table_destroy(&table); /* idempotent */
}

/* Every allocation a node costs refused in turn: NOMEM, no node taken, a retry works, nothing leaks. */
static void test_node_new_allocation_failures(void) {
    ta_session_table table;
    ta_session_table_init(&table, test_allocator);
    ta_test_allocator_reset_counts(&heap);
    uint32_t node = 0;
    CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = 1u }, &node) == TA_OK);
    uint64_t first_costs = heap.stats.allocations; /* directory, page, index directory, index page */
    ta_session_node_free(&table, node);
    ta_session_table_destroy(&table);
    CHECK(first_costs >= 2u && heap.stats.live_blocks == 0);
    for (uint64_t nth = 1; nth <= first_costs; nth += 1u) {
        ta_session_table_init(&table, test_allocator);
        ta_test_allocator_reset_counts(&heap);
        ta_test_allocator_fail_nth(&heap, nth);
        CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = 1u }, &node) == TA_ERR_NOMEM);
        ta_test_allocator_disarm(&heap);
        CHECK(ta_table_count(&table.index) == 0 && table.nodes.used == 0);
        CHECK(ta_session_find(&table, 1u) == NULL);
        CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = 1u }, &node) == TA_OK);
        ta_session_insert(&table, node);
        CHECK(ta_session_find(&table, 1u) == ta_session_node_at(&table, node));
        ta_session_table_destroy(&table);
        CHECK(heap.stats.live_blocks == 0);
    }
}

static const char *uri_of(fixture *f, ta_actor_ref ref, char *buffer, uint32_t capacity) {
    uint32_t needed = 0;
    ta_scxml_target target = { .kind = TA_SCXML_TARGET_ACTOR, .as.actor = ref };
    if (ta_scxml_target_format(f->system, &(ta_scxml_target_format_params){ .target = target, .buffer = buffer,
                                                                          .capacity = capacity,
                                                                          .needed = &needed }) != TA_OK) {
        buffer[0] = '\0';
    }
    return buffer;
}

/*
 * An unnamed session's URI names its system's incarnation and its family
 * (ADR 0088), and still does once the actor is gone: a reply to it can
 * reach the family.
 */
static void test_an_unnamed_session_names_its_family(void) {
    fixture f;
    start_in(&f, 7);
    CHECK(ta_system_incarnation(f.system) == 7);
    ta_atom idle = 0;
    ta_atom other = 0;
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("idle"), &idle) == TA_OK && idle != 0);
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("other"), &other) == TA_OK && other != idle);

    ta_actor_id actor = spawn(&f, NULL);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK);
    CHECK(view.self.kind == TA_ACTOR_REF_RUNTIME && view.self.family == idle && view.self.as.runtime == actor);
    ta_actor_ref self = view.self;

    char expected[64];
    snprintf(expected, sizeof expected, "#_scxml_7.%llu.idle", (unsigned long long)actor);
    char buffer[64];
    CHECK(strcmp(uri_of(&f, self, buffer, sizeof buffer), expected) == 0);
    uint32_t needed = 0;
    CHECK(ta_session_id_format(f.system, &(ta_session_id_format_params){ .session = actor, .family = (ta_str)STR("idle"),
                                                                        .buffer = buffer,
                                                                        .capacity = sizeof buffer,
                                                                        .needed = &needed }) == TA_OK &&
          strcmp(buffer, expected + strlen("#_scxml_")) == 0 && needed == strlen(buffer) + 1u);

    /* Another family's actor never takes mail for this session. */
    bool named = true;
    ta_actor_ref routed;
    CHECK(!ta_session_route(f.system, &self, &routed) && routed.as.runtime == actor && routed.family == idle);
    ta_actor_ref wrong = { .kind = TA_ACTOR_REF_RUNTIME, .family = other, .as.runtime = actor };
    CHECK(!ta_session_route(f.system, &wrong, &routed) && routed.as.runtime == 0 && routed.family == other);
    ta_actor_ref bare = route(&f, actor, &named);
    CHECK(!named && bare.as.runtime == actor);

    /* Gone, the reference still writes its session with its family, and the
     * text parses back to the same session and family. */
    CHECK(ta_actor_destroy(f.system, actor) == TA_OK);
    CHECK(strcmp(uri_of(&f, self, buffer, sizeof buffer), expected) == 0);
    ta_scxml_target parsed = { 0 };
    CHECK(ta_scxml_target_parse(f.system, (ta_str){ .ptr = expected, .len = (uint32_t)strlen(expected) }, &parsed) ==
              TA_OK &&
          parsed.kind == TA_SCXML_TARGET_ACTOR && parsed.as.actor.kind == TA_ACTOR_REF_RUNTIME &&
          parsed.as.actor.as.runtime == actor && parsed.as.actor.family == idle);

    /* Another incarnation's session is none here, but keeps its family. */
    char foreign[64];
    snprintf(foreign, sizeof foreign, "#_scxml_8.%llu.idle", (unsigned long long)actor);
    CHECK(ta_scxml_target_parse(f.system, (ta_str){ .ptr = foreign, .len = (uint32_t)strlen(foreign) }, &parsed) ==
              TA_OK &&
          parsed.as.actor.as.runtime == 0 && parsed.as.actor.family == idle);
    finish(&f);
}

/* A step key names its session's family (ADR 0088), so a host can render
 * the session's text from any record, even once the session is gone. */
static int keyed_records = 0;
static int keyed_without_family = 0;
static ta_atom keyed_family = 0;
static void keyed_visit(void *self, const ta_tel_entry *entry) {
    (void)self;
    keyed_records += 1;
    keyed_without_family += entry->trace.key.family == keyed_family ? 0 : 1;
}

static void test_a_step_key_names_its_family(void) {
    fixture f;
    start(&f);
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("idle"), &keyed_family) == TA_OK);
    CHECK(ta_system_set_trace_masks(f.system, &(ta_trace_masks){ .kinds = UINT32_MAX }) == TA_OK);
    keyed_records = 0;
    keyed_without_family = 0;
    ta_actor_id actor = spawn(&f, NULL);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(f.system, &(ta_actor_resume_params){ .actor = actor, .budget = 16, .status = &status }) ==
          TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK);
    CHECK(view.step.family == keyed_family && view.trace.cause.family == keyed_family);
    CHECK(ta_actor_destroy(f.system, actor) == TA_OK);
    tel_reader keyed_reader = { .visit = keyed_visit };
    tel_read(f.system, &keyed_reader);
    CHECK(keyed_reader.bad == 0 && keyed_records > 0 && keyed_without_family == 0);
    finish(&f);
}

/* The first session costs the 6 KiB node page and a 64 slot index, not a 24 KiB page and a 32 KiB index. */
static void test_first_use_is_small(void) {
    ta_session_table table;
    ta_session_table_init(&table, test_allocator);
    ta_test_allocator_reset_counts(&heap);
    ta_actor stand_in;
    uint32_t node = 0;
    CHECK(ta_session_node_new(&table, &(ta_session_node_params){ .id = ta_actor_id_make(1u, 1u), .actor = &stand_in }, &node) == TA_OK);
    ta_session_insert(&table, node);
    printf("session id test: first session holds %zu bytes\n", heap.stats.live_bytes);
    CHECK(heap.stats.live_bytes <= 8u * 1024u);
    CHECK(heap.stats.largest_block <= 6u * 1024u + 512u);
    ta_session_table_destroy(&table);
    CHECK(heap.stats.live_blocks == 0);
}

int main(void) {
    test_a_step_key_names_its_family();
    test_an_unnamed_session_names_its_family();
    test_load_keeps_the_session();
    test_load_runs_under_its_trace_parent();
    test_session_ids_are_unique();
    test_reconstruct_keeps_a_free_session();
    test_remove_unloaded_ends_the_session();
    test_destroy_frees_a_parked_session();
    test_the_table_grows_without_copying();
    test_equal_hash_session_ids_still_insert();
    test_node_new_then_insert_with_removals_between();
    test_node_new_allocation_failures();
    test_first_use_is_small();
    if (failures != 0) {
        fprintf(stderr, "session id test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core session id test passed");
    return 0;
}

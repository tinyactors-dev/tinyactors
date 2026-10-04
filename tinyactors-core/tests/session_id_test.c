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
    CHECK(f.system->sessions.count == 1);
    finish(&f);
}

/* A load runs under the trace parent it is given (napi ADR 0020): the
 * LOADED record carries it, so the load is in the trace of the message
 * that found the actor unloaded. */
static ta_trace_record loaded_record;
static int loaded_records = 0;
static void loaded_write(void *self, const ta_trace_record *record) {
    (void)self;
    if (record->kind == TA_TRACE_LOADED) {
        loaded_record = *record;
        loaded_records += 1;
    }
}
static const ta_trace_sink_vtable loaded_vtable = { .write = loaded_write };

static void test_load_runs_under_its_trace_parent(void) {
    fixture f;
    start(&f);
    CHECK(ta_system_set_trace_sink(f.system, (ta_trace_sink){ .v = &loaded_vtable,
                                                             .kinds = TA_TRACE_BIT(TA_TRACE_LOADED) }) == TA_OK);
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
    CHECK(loaded_records == 1);
    CHECK(loaded_record.context.trace_id_high == parent.context.trace_id_high &&
          loaded_record.context.trace_id_low == parent.context.trace_id_low);
    CHECK(loaded_record.cause.session == 0 && loaded_record.cause.macrostep == parent.cause.macrostep);
    /* No parent: the record has none either. */
    image = unload(&f, id);
    loaded_records = 0;
    CHECK(ta_actor_load(f.system, &(ta_actor_load_params){ .image = image, .out = &id }) == TA_OK);
    ta_session_image_destroy(image);
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
    CHECK(f.system->sessions.count == 1);
    CHECK(ta_actor_remove_unloaded(f.system, &(ta_actor_remove_unloaded_params){ .address = &the_address }) ==
          TA_OK);
    CHECK(f.system->sessions.count == 0);
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

/*
 * The table at scale: 200,000 sessions. Growth never copies it (no block
 * larger than one page of buckets), every entry is found, a resident entry
 * shadows an unloaded one with the same ID, and removal empties it.
 */
static void test_the_table_grows_without_copying(void) {
    enum { COUNT = 200000 };
    ta_session_table table;
    CHECK(ta_session_table_init(&table, test_allocator) == TA_OK);
    ta_session_node **nodes = calloc(COUNT, sizeof(*nodes));
    ta_actor stand_in;
    ta_test_allocator_reset_counts(&heap);
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        CHECK(ta_session_node_new(&table, &nodes[index]) == TA_OK);
        nodes[index]->id = ta_actor_id_make(index, 1u + index % 7u);
        nodes[index]->actor = index % 2u == 0 ? &stand_in : NULL;
        ta_session_insert(&table, nodes[index]);
    }
    CHECK(heap.stats.largest_block <= TA_SESSION_PAGE_BUCKETS * sizeof(ta_session_node *));
    CHECK(table.count == COUNT);
    uint32_t found = 0;
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        found += ta_session_find(&table, nodes[index]->id) == nodes[index] ? 1u : 0u;
    }
    CHECK(found == COUNT);
    CHECK(ta_session_find(&table, ta_actor_id_make(COUNT + 1u, 1u)) == NULL);
    /* A resident entry wins over an unloaded one with the same ID. */
    ta_session_node *resident = NULL;
    CHECK(ta_session_node_new(&table, &resident) == TA_OK);
    resident->id = nodes[1]->id; /* nodes[1] is unloaded */
    resident->actor = &stand_in;
    ta_session_insert(&table, resident);
    CHECK(ta_session_find(&table, resident->id) == resident);
    ta_session_remove(&table, resident);
    ta_session_node_free(&table, resident);
    CHECK(ta_session_find(&table, nodes[1]->id) == nodes[1]);
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        ta_session_remove(&table, nodes[index]);
        ta_session_node_free(&table, nodes[index]);
    }
    CHECK(table.count == 0);
    ta_session_table_destroy(&table);
    free(nodes);
    CHECK(heap.stats.live_blocks == 0);
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
static void keyed_write(void *self, const ta_trace_record *record) {
    (void)self;
    keyed_records += 1;
    keyed_without_family += record->key.family == keyed_family ? 0 : 1;
}
static const ta_trace_sink_vtable keyed_vtable = { .write = keyed_write };

static void test_a_step_key_names_its_family(void) {
    fixture f;
    start(&f);
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("idle"), &keyed_family) == TA_OK);
    CHECK(ta_system_set_trace_sink(f.system, (ta_trace_sink){ .v = &keyed_vtable, .kinds = UINT32_MAX }) == TA_OK);
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
    CHECK(keyed_records > 0 && keyed_without_family == 0);
    finish(&f);
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
    if (failures != 0) {
        fprintf(stderr, "session id test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core session id test passed");
    return 0;
}

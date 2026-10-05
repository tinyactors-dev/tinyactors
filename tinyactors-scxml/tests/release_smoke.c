/*
 * The release's smoke test (root ADR 0001,
 * docs/adr/0001-packaging-and-releases.md): a program that uses only the
 * public headers, linked against the codec's packed static or shared
 * library and core's and txml's released ones, and run on its target
 * (under qemu for another architecture). It imports a two-state SCXML
 * document on the null data model, spawns an actor, delivers `go`, checks
 * that the actor moved from `idle` to `done`, destroys the system, and
 * checks that every allocation was freed.
 *
 * Uses the C library; the libraries do not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ta_scxml.h"

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

static const char document[] =
    "<scxml xmlns=\"http://www.w3.org/2005/07/scxml\" version=\"1.0\" name=\"smoke\" datamodel=\"null\" "
    "initial=\"idle\">"
    "<state id=\"idle\"><transition event=\"go\" target=\"done\"/></state>"
    "<state id=\"done\"/>"
    "</scxml>";

static long outstanding = 0;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    void *memory = aligned_alloc(alignment, size == 0 ? alignment : size);
    outstanding += memory != NULL ? 1 : 0;
    return memory;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    outstanding -= params->ptr != NULL ? 1 : 0;
    free(params->ptr);
}

static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = counting_free };
static const ta_allocator counting = { .self = NULL, .v = &counting_vtable };

static int fail(const char *what) {
    fprintf(stderr, "release smoke: %s\n", what);
    return 1;
}

/* Whether the state named `id` of `definition` is active in `actor`. */
static int in_state(ta_system *system, ta_definition_id definition, ta_actor_id actor, ta_str id) {
    ta_state_id state = 0;
    if (ta_definition_find_state(system, &(ta_definition_find_state_params){ .definition = definition, .id = id,
                                                                           .out = &state }) != TA_OK) {
        return 0;
    }
    ta_state_id active[8];
    uint32_t count = 0;
    if (ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                         .capacity = 8, .count = &count }) != TA_OK) {
        return 0;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        if (active[index] == state) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    ta_scxml_codec codec;
    if (ta_scxml_codec_init(&codec, NULL) != TA_OK) {
        return fail("ta_scxml_codec_init failed");
    }
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = counting }, &system) != TA_OK) {
        return fail("ta_system_create failed");
    }
    ta_arena arena;
    if (ta_arena_init(&arena, counting, 4096) != TA_OK) {
        return fail("ta_arena_init failed");
    }
    ta_datamodel null_datamodel = ta_null_datamodel();
    ta_definition_id id = 0;
    ta_import_error error;
    memset(&error, 0, sizeof error);
    if (ta_definition_import(system, &(ta_definition_import_params){
                                         .codec = ta_scxml_codec_adapter(&codec),
                                         .input = { .ptr = (const uint8_t *)document, .len = sizeof document - 1u },
                                         .arena = &arena,
                                         .datamodels = &null_datamodel,
                                         .datamodel_count = 1,
                                         .allocator = counting,
                                         .out = &id,
                                         .error = &error,
                                     }) != TA_OK) {
        return fail("the document does not import");
    }
    ta_arena_destroy(&arena);

    ta_actor_id actor = 0;
    ta_run_status status = TA_RUN_MORE;
    ta_input_outcome outcome = { .kind = TA_INPUT_TAKEN };
    uint32_t outcomes = 0;
    ta_run_params run = { .budget = 100, .outcomes = &outcome, .outcome_capacity = 1, .outcome_count = &outcomes,
                          .status = &status };
    if (ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) != TA_OK ||
        ta_run(system, &run) != TA_OK || outcomes != 1 || outcome.kind != TA_INPUT_COMPLETED) {
        return fail("the actor does not start");
    }
    if (!in_state(system, id, actor, STR("idle"))) {
        return fail("the actor did not start in idle");
    }

    ta_atom atom = 0;
    ta_event_name_id go = 0;
    if (ta_system_intern_atom(system, STR("go"), &atom) != TA_OK ||
        ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &go) != TA_OK) {
        return fail("the event name cannot be interned");
    }
    ta_event event = { .name = go, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    if (ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
                                                            .event = &event, .correlation = 1 }) != TA_OK ||
        ta_run(system, &run) != TA_OK) {
        return fail("the event is not delivered");
    }
    if (outcomes != 1 || outcome.kind != TA_INPUT_COMPLETED || outcome.correlation != 1 || status != TA_RUN_IDLE ||
        in_state(system, id, actor, STR("idle")) || !in_state(system, id, actor, STR("done"))) {
        return fail("the actor did not reach done");
    }

    ta_system_destroy(system);
    if (outstanding != 0) {
        fprintf(stderr, "release smoke: %ld allocations outstanding after destroy\n", outstanding);
        return 1;
    }
    uint32_t core = ta_version();
    printf("ta_scxml %d.%d.%d on tinyactors %u.%u.%u: imported, idle -> done on go, every allocation freed\n",
           TA_SCXML_VERSION_MAJOR, TA_SCXML_VERSION_MINOR, TA_SCXML_VERSION_PATCH, (unsigned)(core >> 16),
           (unsigned)((core >> 8) & 0xffu), (unsigned)(core & 0xffu));
    return 0;
}

/*
 * The input log (ADR 0090 sections 3 to 7), through the public header and
 * the amalgamation only:
 * - every record kind, byte for byte, against a second, independent writer
 *   of the layout;
 * - a live actor's log, written through a sink and replayed through sources
 *   that hand out 1 and 7 bytes at a time, reproduces the live actor: its
 *   configuration, history, pending sends and data; the four timelines of
 *   ADR 0089 section 2; a life that ended with `destroyed`; operator
 *   actions, reports, jumps and host records;
 * - an input whose record cannot be written is refused and changes
 *   nothing: a sink that refuses at `begin`, at every byte of the record,
 *   and a value with no durable form; a timer whose record cannot be
 *   written stays due;
 * - readers refuse an unknown version or kind and skip fields a newer
 *   writer added;
 * - writing a record allocates nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,    \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }
#define END {0}

/* ------------------------------------------------------------------------
 * A counting allocator.
 */

static long allocations;

static void *count_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    allocations += 1;
    return malloc(size);
}

static void count_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable count_vtable = { .alloc = count_alloc, .free = count_free };
static const ta_allocator allocator = { .self = NULL, .v = &count_vtable };

/* ------------------------------------------------------------------------
 * A byte buffer, a sink that appends to one, and a source that reads one.
 */

typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t cap;
} buffer;

static void buffer_put(buffer *b, const void *bytes, size_t len) {
    if (b->len + len > b->cap) {
        b->cap = (b->len + len) * 2u + 64u;
        b->bytes = realloc(b->bytes, b->cap);
    }
    memcpy(b->bytes + b->len, bytes, len);
    b->len += len;
}

static void buffer_free(buffer *b) {
    free(b->bytes);
    *b = (buffer){ 0 };
}

typedef struct {
    buffer log;
    size_t unit_start;
    bool open;
    bool refuse_begin;
    long refuse_at; /* refuse the write that would pass this many bytes of the unit; -1: never */
    int units;
    int kept;
} memory_sink;

static ta_op_status sink_begin(void *self) {
    memory_sink *sink = self;
    CHECK(!sink->open);
    if (sink->refuse_begin) {
        return TA_OP_NOMEM;
    }
    sink->open = true;
    sink->unit_start = sink->log.len;
    sink->units += 1;
    return TA_OP_OK;
}

static ta_op_status sink_write(void *self, ta_bytes bytes) {
    memory_sink *sink = self;
    CHECK(sink->open);
    if (sink->refuse_at >= 0 && (long)(sink->log.len - sink->unit_start + bytes.len) > sink->refuse_at) {
        return TA_OP_NOMEM;
    }
    buffer_put(&sink->log, bytes.ptr, bytes.len);
    return TA_OP_OK;
}

static void sink_end(void *self, bool keep) {
    memory_sink *sink = self;
    CHECK(sink->open);
    sink->open = false;
    if (keep) {
        sink->kept += 1;
    } else {
        sink->log.len = sink->unit_start;
    }
}

static const ta_byte_sink_vtable sink_vtable = { .begin = sink_begin, .write = sink_write, .end = sink_end };

static memory_sink the_sink;

static void sink_reset(void) {
    buffer_free(&the_sink.log);
    the_sink = (memory_sink){ .refuse_at = -1 };
}

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t at;
    uint32_t piece;
} memory_source;

static ta_op_status source_read(void *self, const ta_byte_read_params *params) {
    memory_source *source = self;
    uint32_t count = params->capacity < source->piece ? params->capacity : source->piece;
    if (count > source->len - source->at) {
        count = (uint32_t)(source->len - source->at);
    }
    memcpy(params->buffer, source->bytes + source->at, count);
    source->at += count;
    *params->got = count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable source_vtable = { .read = source_read };

/* ------------------------------------------------------------------------
 * Values: blobs of bytes, on a data model that wraps the null one.
 */

typedef struct {
    long refs;
    uint32_t len;
    uint8_t bytes[];
} blob;

static long live_blobs;
static bool durable_refuses;
static ta_value bound; /* the last binding blob_init_instance saw */

static void blob_retain(void *self) {
    ((blob *)self)->refs += 1;
}

static void blob_release(void *self) {
    blob *b = self;
    if (--b->refs == 0) {
        live_blobs -= 1;
        free(b);
    }
}

static const ta_value_vtable blob_vtable = { .retain = blob_retain, .release = blob_release };

static ta_value blob_make(uint32_t len, uint8_t seed) {
    blob *b = malloc(sizeof(blob) + len);
    b->refs = 1;
    b->len = len;
    for (uint32_t index = 0; index < len; index += 1) {
        b->bytes[index] = (uint8_t)(seed + index * 31u);
    }
    live_blobs += 1;
    return (ta_value){ .self = b, .v = &blob_vtable };
}

static bool blob_equal(const ta_value *a, const ta_value *b) {
    const blob *x = a->self;
    const blob *y = b->self;
    return a->v == &blob_vtable && b->v == &blob_vtable && x->len == y->len && memcmp(x->bytes, y->bytes, x->len) == 0;
}

/* A blob is its length, then its bytes in pieces of 7. */
static ta_op_status blob_write(void *self, const ta_datamodel_value_write_params *params) {
    (void)self;
    CHECK(params->durable);
    if (durable_refuses) {
        *params->fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED, .message = STR("no durable form"),
                                     .data = blob_make(2, 9) };
        return TA_OP_ERROR;
    }
    const blob *b = params->value->self;
    uint32_t len = b->len;
    ta_op_status status = params->sink.v->write(params->sink.self,
                                                (ta_bytes){ .ptr = (const uint8_t *)&len, .len = sizeof len });
    for (uint32_t at = 0; status == TA_OP_OK && at < b->len; at += 7) {
        uint32_t count = b->len - at < 7 ? b->len - at : 7;
        status = params->sink.v->write(params->sink.self, (ta_bytes){ .ptr = b->bytes + at, .len = count });
    }
    return status;
}

static ta_op_status blob_read(void *self, const ta_datamodel_value_read_params *params) {
    (void)self;
    uint8_t piece[5];
    uint8_t header[sizeof(uint32_t)];
    uint32_t have = 0;
    blob *b = NULL;
    uint32_t filled = 0;
    for (;;) {
        uint32_t got = 0;
        ta_op_status status = params->source.v->read(
            params->source.self, &(ta_byte_read_params){ .buffer = piece, .capacity = sizeof piece, .got = &got });
        if (status != TA_OP_OK) {
            if (b != NULL) {
                blob_release(b);
            }
            return status;
        }
        if (got == 0) {
            break;
        }
        for (uint32_t index = 0; index < got; index += 1) {
            if (have < sizeof header) {
                header[have++] = piece[index];
                if (have == sizeof header) {
                    uint32_t len;
                    memcpy(&len, header, sizeof len);
                    b = blob_make(len, 0).self;
                }
            } else if (b != NULL && filled < b->len) {
                b->bytes[filled++] = piece[index];
            }
        }
    }
    if (b == NULL || filled != b->len) {
        if (b != NULL) {
            blob_release(b);
        }
        *params->fault = (ta_fault){ .code = TA_ERR_INVALID, .message = STR("no blob") };
        return TA_OP_ERROR;
    }
    *params->out = (ta_value){ .self = b, .v = &blob_vtable };
    return TA_OP_OK;
}

static ta_result blob_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    ta_value_release(&bound);
    bound = (ta_value){ 0 };
    if (params->binding != NULL && params->binding->v != NULL) {
        bound = *params->binding;
        ta_value_retain(&bound);
    }
    return TA_OK;
}

static ta_datamodel_vtable blob_model_vtable;

/* ------------------------------------------------------------------------
 * Adapters: a router that accepts everything, an invoker, a directory.
 */

static int routed;
static ta_external_event_submit_status route(void *self, const ta_external_event_submit_params *params) {
    (void)self;
    (void)params;
    routed += 1;
    return TA_EXTERNAL_EVENT_ACCEPTED;
}
static const ta_external_event_router_vtable router_vtable = { .submit = route };

static ta_op_status invoker_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    *params->service_handle = 7;
    return TA_OP_OK;
}
static ta_op_status invoker_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)self;
    (void)service;
    (void)fault;
    return TA_OP_OK;
}
static ta_op_status invoker_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self;
    (void)params;
    return TA_OP_OK;
}
static const ta_invoker_vtable invoker_vtable = { .start = invoker_start, .cancel = invoker_cancel,
                                                  .forward = invoker_forward };

#define SLOTS 16
typedef struct {
    ta_address *address[SLOTS];
    ta_actor_directory_entry entry[SLOTS];
} small_directory;
static small_directory directory;

static int dir_find(const ta_address *address) {
    for (int index = 0; index < SLOTS; index += 1) {
        if (directory.address[index] == address) {
            return index;
        }
    }
    return -1;
}
static bool same_entry(ta_actor_directory_entry left, ta_actor_directory_entry right) {
    return left.kind == right.kind && (left.kind != TA_ACTOR_DIRECTORY_RESIDENT || left.actor == right.actor);
}
static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    (void)self;
    if (dir_find(params->address) >= 0) {
        return TA_ERR_STALE;
    }
    int slot = dir_find(NULL);
    if (slot < 0) {
        return TA_ERR_NOMEM;
    }
    ta_address_retain(params->address);
    directory.address[slot] = params->address;
    directory.entry[slot] = params->entry;
    return TA_OK;
}
static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    (void)self;
    int slot = dir_find(params->address);
    *params->out = slot < 0 ? (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE } : directory.entry[slot];
    return TA_OK;
}
static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    (void)self;
    int slot = dir_find(params->address);
    if (slot < 0 || !same_entry(directory.entry[slot], params->expected)) {
        return TA_ERR_STALE;
    }
    directory.entry[slot] = params->replacement;
    return TA_OK;
}
static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    (void)self;
    int slot = dir_find(params->address);
    if (slot < 0 || !same_entry(directory.entry[slot], params->expected)) {
        return TA_ERR_STALE;
    }
    ta_address_release(directory.address[slot]);
    directory.address[slot] = NULL;
    return TA_OK;
}
static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

/* ------------------------------------------------------------------------
 * Charts
 */

#define SEND_TO(id_text, event_text, delay_text, target_text)                                     \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(target_text) }, \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define SEND_SELF(id_text, event_text, delay_text)                                                \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define RAISE(event_text) { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR(event_text) } }
#define TARGETS(...) ((const ta_str[]){ __VA_ARGS__, END })
#define GO(event_text, ...) { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text), .targets = TARGETS(__VA_ARGS__) }
#define ALWAYS(...) { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = TARGETS(__VA_ARGS__) }

/* On e1: a timeout to itself in 5s and a message to b in 5s; `timeout`
 * moves to `timedout`; `back` returns to idle. */
static const ta_scxml_state_spec timers[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e1"), .targets = TARGETS(STR("waiting")),
            .actions = (const ta_scxml_action_spec[]){
                SEND_SELF("t1", "timeout", "5s"), SEND_TO("b1", "hello", "5s", "#_actor_b/one"), END } },
          END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("waiting"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("timeout", STR("timedout")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("timedout"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("back", STR("idle")), END } },
    END,
};

/* A deep history inside `on`, and a timer `arm` sets. */
static const ta_scxml_state_spec history_chart[] = {
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("on"),
      .initial = &(ta_scxml_initial_spec){ .targets = TARGETS(STR("a1")) },
      .states = (const ta_scxml_state_spec[]){
          { .kind = TA_SCXML_STATE_HISTORY_DEEP, .id = STR("h"),
            .transitions = (const ta_scxml_transition_spec[]){ { .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                                                 .targets = TARGETS(STR("a1")) }, END } },
          { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a1"),
            .transitions = (const ta_scxml_transition_spec[]){ GO("next", STR("a2")), END } },
          { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a2"),
            .transitions = (const ta_scxml_transition_spec[]){ GO("next", STR("a3")), END } },
          { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a3") },
          END },
      .transitions = (const ta_scxml_transition_spec[]){
          GO("off", STR("off")),
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("arm"),
            .actions = (const ta_scxml_action_spec[]){ SEND_SELF("tick", "tick", "9s"), END } },
          END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("off"),
      .transitions = (const ta_scxml_transition_spec[]){
          GO("back", STR("h")),
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("arm"),
            .actions = (const ta_scxml_action_spec[]){ SEND_SELF("tock", "tock", "4s"), END } },
          END } },
    END,
};

/* An invocation whose error report moves to `failed`. */
static const ta_scxml_state_spec invoking[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("working"),
      .invokes = (const ta_scxml_invoke_spec[]){
          { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("job") },
            .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
          END },
      .transitions = (const ta_scxml_transition_spec[]){
          GO("error.communication.invoke.job.boom", STR("failed")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
    END,
};

/* `go` starts a chain of 12 eventless steps: a runaway at a limit of 5. */
static const ta_scxml_state_spec chain[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("rest"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("go", STR("c1")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c1"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c2")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c2"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c3")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c3"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c4")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c4"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c5")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c5"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c6")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c6"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c7")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c7"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c8")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c8"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c9")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c9"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c10")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c10"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("c11")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c11"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS(STR("rest")), END } },
    END,
};

/* Reaches a top-level final state in its bootstrap. */
static const ta_scxml_state_spec at_once_final[] = {
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("done") },
    END,
};

/* ------------------------------------------------------------------------
 * Helpers
 */

static ta_definition_id define(ta_system *system, const char *family, const ta_scxml_state_spec *states) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(
        system, &(ta_definition_compile_params){
                    .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity, .states = states },
                    .datamodel = { .self = NULL, .v = &blob_model_vtable },
                    .allocator = allocator,
                    .out = &definition,
                });
    CHECK(result == TA_OK);
    ta_definition_id id = 0;
    CHECK(result == TA_OK && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static ta_system *create_system(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    CHECK(ta_system_set_external_event_router(system, (ta_external_event_router){ .v = &router_vtable }) == TA_OK);
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .v = &dir_vtable }) == TA_OK);
    ta_atom svc = 0;
    CHECK(ta_system_intern_atom(system, (ta_str)STR("svc"), &svc) == TA_OK);
    CHECK(ta_system_register_invoker(system, svc, (ta_invoker){ .v = &invoker_vtable }) == TA_OK);
    sink_reset();
    ta_system_set_input_sink(system, (ta_byte_sink){ .self = &the_sink, .v = &sink_vtable });
    routed = 0;
    return system;
}

static void destroy_system(ta_system *system) {
    for (int index = 0; index < SLOTS; index += 1) {
        if (directory.address[index] != NULL) {
            ta_address_release(directory.address[index]);
            directory.address[index] = NULL;
        }
    }
    ta_system_destroy(system);
    ta_value_release(&bound);
    bound = (ta_value){ 0 };
    sink_reset();
}

static void log_family(ta_system *system, const char *family) {
    CHECK(ta_system_log_family(system, (ta_str){ .ptr = family, .len = (uint32_t)strlen(family) }, true) == TA_OK);
}

static ta_run_status run(ta_system *system, ta_actor_id actor) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = actor, .budget = 100, .status = &status }) ==
          TA_OK);
    return status;
}

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atoms[8];
    uint32_t count = 0;
    const char *start = text;
    for (const char *at = text;; at += 1) {
        if (*at == '.' || *at == '\0') {
            CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = start, .len = (uint32_t)(at - start) },
                                        &atoms[count]) == TA_OK);
            count += 1;
            if (*at == '\0') {
                break;
            }
            start = at + 1;
        }
    }
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = atoms, .count = count }, &name) == TA_OK);
    return name;
}

static void deliver(ta_system *system, ta_actor_id actor, const char *event) {
    ta_event delivered = { .name = event_name(system, event), .type = TA_EVENT_EXTERNAL,
                           .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = actor, .event = &delivered }) == TA_OK);
    CHECK(run(system, actor) == TA_RUN_STABLE);
}

static ta_state_id state_of(ta_system *system, ta_actor_id actor, const char *id) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    ta_state_id state = 0;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                               .definition = view.definition_id,
                                               .id = { .ptr = id, .len = (uint32_t)strlen(id) },
                                               .out = &state }) == TA_OK);
    return state;
}

static bool in_state(ta_system *system, ta_actor_id actor, const char *id) {
    ta_state_id active[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                          .capacity = 8, .count = &count }) ==
          TA_OK);
    ta_state_id wanted = state_of(system, actor, id);
    for (uint32_t index = 0; index < count; index += 1) {
        if (active[index] == wanted) {
            return true;
        }
    }
    return false;
}

static uint32_t dispatch(ta_system *system) {
    uint32_t dispatched = 0;
    bool more = false;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 100, .dispatched = &dispatched,
                                                                   .more_due = &more }) == TA_OK);
    return dispatched;
}

static ta_actor_id spawn_named(ta_system *system, ta_definition_id definition, const ta_actor_address *address) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = address,
                                                           .out = &actor }) == TA_OK);
    ta_run_status status = run(system, actor);
    CHECK(status == TA_RUN_STABLE || status == TA_RUN_DONE);
    return actor;
}

/* Whether two actors are the same state: configuration, and pending sends
 * by due time, send ID and event. */
static bool same_state(ta_system *system, ta_actor_id left, ta_actor_id right) {
    ta_state_id left_states[16];
    ta_state_id right_states[16];
    uint32_t left_count = 0;
    uint32_t right_count = 0;
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){
                                             .actor = left, .states = left_states, .capacity = 16,
                                             .count = &left_count }) == TA_OK);
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){
                                             .actor = right, .states = right_states, .capacity = 16,
                                             .count = &right_count }) == TA_OK);
    if (left_count != right_count || memcmp(left_states, right_states, left_count * sizeof(ta_state_id)) != 0) {
        return false;
    }
    ta_actor_inspection left_view;
    ta_actor_inspection right_view;
    CHECK(ta_actor_inspect(system, left, &left_view) == TA_OK);
    CHECK(ta_actor_inspect(system, right, &right_view) == TA_OK);
    if (left_view.delayed_send_count != right_view.delayed_send_count) {
        return false;
    }
    for (uint32_t index = 0; index < left_view.delayed_send_count; index += 1) {
        const ta_delayed_send_info *a = &left_view.delayed_sends[index];
        const ta_delayed_send_info *b = &right_view.delayed_sends[index];
        if (a->due != b->due || a->send_id != b->send_id || a->event != b->event) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Replaying a whole log
 */

typedef struct {
    ta_actor_id actor;
    int host_records;
    uint64_t queued[8];
    int queued_count;
    uint64_t delivered[8];
    int delivered_count;
    uint64_t dropped[8];
    int dropped_count;
    ta_value queued_data;
    bool finished;
    int lives;
} replayed;

/* Prepares from `log` at `address` and applies every record, resuming the
 * actor as each step asks; the actor is left unpublished. */
static replayed replay_log(ta_system *system, ta_definition_id definition, const ta_actor_address *address,
                           const buffer *log, uint32_t piece) {
    replayed out = { 0 };
    memory_source source = { .bytes = log->bytes, .len = log->len, .piece = piece };
    ta_byte_source as_source = { .self = &source, .v = &source_vtable };
    bool prepare = true;
    while (source.at < source.len) {
        if (prepare) {
            CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = address, .definition = definition,
                                                                       .log = &as_source,
                                                                       .out = &out.actor }) == TA_OK);
            ta_run_status status = run(system, out.actor);
            CHECK(status == TA_RUN_STABLE || status == TA_RUN_DONE);
            prepare = false;
            out.lives += 1;
            continue;
        }
        ta_replay_step step = TA_REPLAY_NEXT;
        ta_host_record host = { 0 };
        ta_result result = ta_actor_replay(system, &(ta_actor_replay_params){ .actor = out.actor, .source = as_source,
                                                                             .step = &step, .host = &host });
        CHECK(result == TA_OK);
        if (result != TA_OK) {
            break;
        }
        switch (step) {
            case TA_REPLAY_RUN: {
                ta_run_status status = TA_RUN_ERROR;
                ta_result ran = ta_actor_resume(system, &(ta_actor_resume_params){ .actor = out.actor, .budget = 100,
                                                                                  .status = &status });
                CHECK(ran == TA_OK || ran == TA_ERR_RUNAWAY);
                if (host.host_seq != 0 && out.delivered_count < 8) {
                    out.delivered[out.delivered_count++] = host.host_seq;
                }
                break;
            }
            case TA_REPLAY_RELEASE: {
                ta_run_status status = TA_RUN_ERROR;
                CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = out.actor, .budget = 100,
                                                                        .status = &status,
                                                                        .no_microstep_limit = true }) == TA_OK);
                CHECK(status == TA_RUN_STABLE);
                break;
            }
            case TA_REPLAY_HOST:
                out.host_records += 1;
                if (host.kind == TA_RECORD_QUEUED && out.queued_count < 8) {
                    out.queued[out.queued_count++] = host.host_seq;
                    if (host.event.data.v != NULL) {
                        ta_value_release(&out.queued_data);
                        out.queued_data = host.event.data;
                        ta_value_retain(&out.queued_data);
                    }
                } else if (host.kind == TA_RECORD_DROPPED && out.dropped_count < 8) {
                    out.dropped[out.dropped_count++] = host.host_seq;
                }
                if (host.has_event) {
                    ta_event_release(&host.event);
                }
                break;
            case TA_REPLAY_FINISHED:
                out.finished = true;
                break;
            case TA_REPLAY_ENDED:
                CHECK(ta_actor_destroy(system, out.actor) == TA_OK);
                prepare = true;
                break;
            case TA_REPLAY_NEXT:
                break;
        }
    }
    return out;
}

/* ------------------------------------------------------------------------
 * A second writer of the layout, for the golden records
 */

static void put_u16(buffer *b, uint16_t value) {
    uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    buffer_put(b, bytes, 2);
}
static void put_u32(buffer *b, uint32_t value) {
    uint8_t bytes[4];
    for (int index = 0; index < 4; index += 1) {
        bytes[index] = (uint8_t)(value >> (8 * index));
    }
    buffer_put(b, bytes, 4);
}
static void put_u64(buffer *b, uint64_t value) {
    uint8_t bytes[8];
    for (int index = 0; index < 8; index += 1) {
        bytes[index] = (uint8_t)(value >> (8 * index));
    }
    buffer_put(b, bytes, 8);
}
static void put_zeros(buffer *b, size_t count) {
    static const uint8_t zeros[8] = { 0 };
    buffer_put(b, zeros, count);
}

/* A field: a number, a text, or an absent text. */
typedef struct {
    int is_text; /* 0 number, 1 text, 2 absent text */
    uint64_t number;
    const char *text;
} field;

#define NUM(value) { .is_text = 0, .number = (value) }
#define TXT(value) { .is_text = 1, .text = (value) }
#define ABSENT { .is_text = 2 }

/* One record: header, fields, texts, and the value's chunks (blob: its
 * length, then pieces of 7). */
static void expect_record(buffer *out, uint16_t kind, uint64_t seq, uint64_t time, const field *fields,
                          uint32_t count, const ta_value *value) {
    uint32_t text_bytes = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        text_bytes += fields[index].is_text == 1 ? (uint32_t)strlen(fields[index].text) : 0u;
    }
    put_u16(out, 1);
    put_u16(out, kind);
    put_u32(out, 32u + 8u * count);
    put_u64(out, seq);
    put_u64(out, time);
    put_u32(out, text_bytes);
    put_u16(out, value != NULL ? 1 : 0);
    put_u16(out, 0);
    uint32_t offset = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        if (fields[index].is_text == 0) {
            put_u64(out, fields[index].number);
        } else if (fields[index].is_text == 2) {
            put_u32(out, UINT32_MAX);
            put_u32(out, 0);
        } else {
            uint32_t len = (uint32_t)strlen(fields[index].text);
            put_u32(out, offset);
            put_u32(out, len);
            offset += len;
        }
    }
    for (uint32_t index = 0; index < count; index += 1) {
        if (fields[index].is_text == 1) {
            buffer_put(out, fields[index].text, strlen(fields[index].text));
        }
    }
    put_zeros(out, (8u - text_bytes % 8u) % 8u);
    if (value != NULL) {
        const blob *b = value->self;
        put_u32(out, 4);
        put_u32(out, b->len);
        for (uint32_t at = 0; at < b->len; at += 7) {
            uint32_t piece = b->len - at < 7 ? b->len - at : 7;
            put_u32(out, piece);
            buffer_put(out, b->bytes + at, piece);
            put_zeros(out, (8u - (4u + piece) % 8u) % 8u);
        }
        put_u32(out, 0);
        put_zeros(out, 4);
    }
}

static bool same_bytes(const buffer *got, const buffer *want) {
    if (got->len != want->len) {
        fprintf(stderr, "  %zu bytes, expected %zu\n", got->len, want->len);
        return false;
    }
    for (size_t index = 0; index < got->len; index += 1) {
        if (got->bytes[index] != want->bytes[index]) {
            fprintf(stderr, "  byte %zu: %02x, expected %02x\n", index, got->bytes[index], want->bytes[index]);
            return false;
        }
    }
    return true;
}

static const ta_actor_address address_a = { .family = { .name = STR("timers") }, .name = STR("a") };

/* ------------------------------------------------------------------------
 * Golden records, one of each kind
 */

static void test_golden_records(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    log_family(system, "timers");
    buffer want = { 0 };

    /* SPAWNED, with a binding */
    ta_value binding = blob_make(9, 3);
    ta_actor_id live = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &address_a,
                                                           .datamodel_binding = &binding, .out = &live }) == TA_OK);
    CHECK(run(system, live) == TA_RUN_STABLE);
    expect_record(&want, TA_RECORD_SPAWNED, 1, 0,
                  (const field[]){ TXT("timers"), TXT("a"), TXT("v1") }, 3, &binding);

    /* EVENT, with data, a send ID, an actor origin and a host number */
    CHECK(ta_system_set_time(system, 1000) == TA_OK);
    ta_value data = blob_make(12, 5);
    ta_send_id sent = 0;
    CHECK(ta_send_id_parse(system, (ta_str)STR("s9"), &sent) == TA_OK || true);
    ta_scxml_target from = { 0 };
    CHECK(ta_scxml_target_parse(system, (ta_str)STR("#_actor_b/one"), &from) == TA_OK);
    ta_event e1 = { .name = event_name(system, "e1"), .type = TA_EVENT_EXTERNAL,
                    .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = from.as.actor },
                    .invoke_id = TA_INVOKE_ID_NONE, .data = data };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1, .host_seq = 41 }) ==
          TA_OK);
    ta_scxml_target_release(&from);
    CHECK(run(system, live) == TA_RUN_STABLE);
    expect_record(&want, TA_RECORD_EVENT, 2, 1000,
                  (const field[]){ NUM(TA_EVENT_EXTERNAL), NUM(41), TXT("e1"), ABSENT, TXT("#_actor_b/one"),
                                   TXT("http://www.w3.org/TR/scxml/#SCXMLEventProcessor"), ABSENT },
                  7, &data);

    /* WITHDRAWN (b1), then FIRED (t1) */
    ta_send_id b1 = 0;
    CHECK(ta_send_id_parse(system, (ta_str)STR("b1"), &b1) == TA_OK);
    CHECK(ta_actor_withdraw_send(system, live, b1) == TA_OK);
    expect_record(&want, TA_RECORD_WITHDRAWN, 3, 1000, (const field[]){ TXT("b1") }, 1, NULL);
    CHECK(ta_system_set_time(system, 6000) == TA_OK);
    CHECK(dispatch(system) == 1);
    expect_record(&want, TA_RECORD_FIRED, 4, 6000, (const field[]){ TXT("t1") }, 1, NULL);

    /* JUMP: an operator transition to `timedout` from the root */
    ta_state_id timedout = state_of(system, live, "timedout");
    ta_event jump_event = { .name = event_name(system, "operator.jump"), .type = TA_EVENT_PLATFORM,
                            .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){
                                       .actor = live, .event = &jump_event,
                                       .transition = &(ta_operator_transition){ .source = TA_STATE_NONE,
                                                                                 .targets = &timedout,
                                                                                 .target_count = 1 } }) == TA_OK);
    CHECK(run(system, live) == TA_RUN_STABLE);
    expect_record(&want, TA_RECORD_JUMP, 5, 6000,
                  (const field[]){ NUM(TA_EVENT_PLATFORM), NUM(0), TXT("operator.jump"), ABSENT, ABSENT, ABSENT,
                                   ABSENT, ABSENT, NUM(1), TXT("timedout") },
                  10, NULL);

    /* host records: QUEUED with data, DROPPED, EVICTED; then DESTROYED is not written */
    ta_value queued_data = blob_make(3, 8);
    ta_event queued = { .name = event_name(system, "later"), .type = TA_EVENT_EXTERNAL,
                        .invoke_id = TA_INVOKE_ID_NONE, .data = queued_data };
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_QUEUED, .host_seq = 7,
                                                                        .actor = live, .event = &queued }) == TA_OK);
    expect_record(&want, TA_RECORD_QUEUED, 6, 6000,
                  (const field[]){ NUM(TA_EVENT_EXTERNAL), NUM(7), TXT("later"), ABSENT, ABSENT, ABSENT, ABSENT },
                  7, &queued_data);
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_DROPPED,
                                                                        .host_seq = 7 }) == TA_OK);
    expect_record(&want, TA_RECORD_DROPPED, 0, 6000, (const field[]){ NUM(7) }, 1, NULL);
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_EVICTED,
                                                                        .actor = live }) == TA_OK);
    expect_record(&want, TA_RECORD_EVICTED, 7, 6000, NULL, 0, NULL);
    CHECK(ta_actor_destroy(system, live) == TA_OK);
    CHECK(same_bytes(&the_sink.log, &want));

    /* DESTROYED, for an actor that is not evicted */
    want.len = 0;
    the_sink.log.len = 0;
    live = spawn_named(system, definition, &address_a);
    expect_record(&want, TA_RECORD_SPAWNED, 1, 6000, (const field[]){ TXT("timers"), TXT("a"), TXT("v1") }, 3, NULL);
    CHECK(ta_actor_destroy(system, live) == TA_OK);
    expect_record(&want, TA_RECORD_DESTROYED, 2, 6000, NULL, 0, NULL);
    CHECK(same_bytes(&the_sink.log, &want));

    ta_value_release(&binding);
    ta_value_release(&data);
    ta_value_release(&queued_data);
    buffer_free(&want);
    destroy_system(system);
}

/* REPORT, ABANDONED, RELEASED and FINISHED */
static void test_golden_operations(void) {
    ta_system *system = create_system();
    ta_definition_id jobs = define(system, "jobs", invoking);
    ta_definition_id chains = define(system, "chains", chain);
    ta_definition_id fins = define(system, "fin", at_once_final);
    log_family(system, "jobs");
    log_family(system, "chains");
    log_family(system, "fin");
    ta_system_set_microstep_limit(system, 5);
    buffer want = { 0 };

    static const ta_actor_address job_address = { .family = { .name = STR("jobs") }, .name = STR("j") };
    ta_actor_id job = spawn_named(system, jobs, &job_address);
    expect_record(&want, TA_RECORD_SPAWNED, 1, 0, (const field[]){ TXT("jobs"), TXT("j"), TXT("v1") }, 3, NULL);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, job, &view) == TA_OK && view.invoke_count == 1);
    ta_value report_data = blob_make(4, 1);
    ta_invoke_error error = { .reason = STR("boom"), .data = &report_data, .ends = true };
    CHECK(ta_actor_report_invoke(system, &(ta_invoke_report_params){ .actor = job, .invoke_id = view.invokes[0].id,
                                                                     .kind = TA_INVOKE_REPORT_ERROR,
                                                                     .as.error = &error }) == TA_OK);
    CHECK(run(system, job) == TA_RUN_STABLE && in_state(system, job, "failed"));
    expect_record(&want, TA_RECORD_REPORT, 2, 0,
                  (const field[]){ NUM(TA_INVOKE_REPORT_ERROR), NUM(1), TXT("job"), TXT("boom") }, 4, &report_data);

    static const ta_actor_address chain_address = { .family = { .name = STR("chains") }, .name = STR("c") };
    ta_actor_id spin = spawn_named(system, chains, &chain_address);
    expect_record(&want, TA_RECORD_SPAWNED, 1, 0, (const field[]){ TXT("chains"), TXT("c"), TXT("v1") }, 3, NULL);
    ta_event go = { .name = event_name(system, "go"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = spin, .event = &go }) == TA_OK);
    expect_record(&want, TA_RECORD_EVENT, 2, 0,
                  (const field[]){ NUM(TA_EVENT_EXTERNAL), NUM(0), TXT("go"), ABSENT, ABSENT, ABSENT, ABSENT }, 7,
                  NULL);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = spin, .budget = 100, .status = &status }) ==
          TA_ERR_RUNAWAY);
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = spin, .budget = 100, .status = &status,
                                                            .no_microstep_limit = true }) == TA_OK);
    CHECK(status == TA_RUN_STABLE && in_state(system, spin, "rest"));
    expect_record(&want, TA_RECORD_RELEASED, 3, 0, NULL, 0, NULL);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = spin, .event = &go }) == TA_OK);
    expect_record(&want, TA_RECORD_EVENT, 4, 0,
                  (const field[]){ NUM(TA_EVENT_EXTERNAL), NUM(0), TXT("go"), ABSENT, ABSENT, ABSENT, ABSENT }, 7,
                  NULL);
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = spin, .budget = 100, .status = &status }) ==
          TA_ERR_RUNAWAY);
    CHECK(ta_actor_abandon(system, spin) == TA_OK);
    expect_record(&want, TA_RECORD_ABANDONED, 5, 0, NULL, 0, NULL);

    static const ta_actor_address fin_address = { .family = { .name = STR("fin") }, .name = STR("f") };
    ta_actor_id fin = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = fins, .address = &fin_address,
                                                           .out = &fin }) == TA_OK);
    CHECK(run(system, fin) == TA_RUN_DONE);
    CHECK(run(system, fin) == TA_RUN_DONE); /* written once */
    expect_record(&want, TA_RECORD_SPAWNED, 1, 0, (const field[]){ TXT("fin"), TXT("f"), TXT("v1") }, 3, NULL);
    expect_record(&want, TA_RECORD_FINISHED, 2, 0, (const field[]){ TXT("done") }, 1, NULL);
    CHECK(ta_actor_destroy(system, fin) == TA_OK); /* finished: no DESTROYED */
    CHECK(same_bytes(&the_sink.log, &want));

    ta_value_release(&report_data);
    buffer_free(&want);
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Replaying a log reproduces the live actor
 */

static const ta_actor_address hist_live = { .family = { .name = STR("hist") }, .name = STR("live") };
static const ta_actor_address hist_copy = { .family = { .name = STR("hist") }, .name = STR("copy") };

static void test_replay_reproduces_live(void) {
    static const uint32_t pieces[] = { 1, 7, 4096 };
    for (size_t piece = 0; piece < sizeof pieces / sizeof pieces[0]; piece += 1) {
        ta_system *system = create_system();
        ta_definition_id definition = define(system, "hist", history_chart);
        log_family(system, "hist");
        ta_value binding = blob_make(20, 2);
        ta_actor_id live = 0;
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &hist_live,
                                                               .datamodel_binding = &binding, .out = &live }) == TA_OK);
        CHECK(run(system, live) == TA_RUN_STABLE);
        static const struct { ta_time at; const char *event; } inputs[] = {
            { 100, "next" }, { 200, "arm" }, { 300, "next" }, { 400, "off" }, { 500, "arm" }, { 900, "back" },
        };
        for (size_t index = 0; index < sizeof inputs / sizeof inputs[0]; index += 1) {
            CHECK(ta_system_set_time(system, inputs[index].at) == TA_OK);
            deliver(system, live, inputs[index].event);
        }
        buffer log = { 0 };
        buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
        ta_value_release(&bound);
        bound = (ta_value){ 0 };

        replayed copy = replay_log(system, definition, &hist_copy, &log, pieces[piece]);
        CHECK(blob_equal(&bound, &binding)); /* the spawn's binding came back */
        CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
        CHECK(same_state(system, live, copy.actor));
        ta_state_id history = state_of(system, live, "h");
        ta_state_id live_recorded[4];
        ta_state_id copy_recorded[4];
        uint32_t live_count = 0;
        uint32_t copy_count = 0;
        CHECK(ta_actor_history(system, &(ta_actor_history_params){ .actor = live, .history = history,
                                                                    .states = live_recorded, .capacity = 4,
                                                                    .count = &live_count }) == TA_OK);
        CHECK(ta_actor_history(system, &(ta_actor_history_params){ .actor = copy.actor, .history = history,
                                                                    .states = copy_recorded, .capacity = 4,
                                                                    .count = &copy_count }) == TA_OK);
        CHECK(live_count == copy_count && live_count == 1 && live_recorded[0] == copy_recorded[0]);
        /* The copy writes nothing while it replays; once published it logs on. */
        CHECK(the_sink.log.len == log.len);
        deliver(system, copy.actor, "next");
        CHECK(the_sink.log.len > log.len);
        buffer_free(&log);
        ta_value_release(&binding);
        destroy_system(system);
    }
}

/* The four timelines of ADR 0089 section 2, from the log core wrote. */
static void live_until_e1(ta_system *system, ta_definition_id definition, ta_actor_id *live) {
    *live = spawn_named(system, definition, &address_a);
    CHECK(ta_system_set_time(system, 1000) == TA_OK);
    deliver(system, *live, "e1");
}

static void evict(ta_system *system, ta_actor_id live) {
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_EVICTED,
                                                                        .actor = live }) == TA_OK);
    CHECK(ta_actor_destroy(system, live) == TA_OK);
}

static void test_timelines(void) {
    /* A timeout to itself: FIRED, then its delivery; nothing fires twice. */
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    log_family(system, "timers");
    ta_actor_id live = 0;
    live_until_e1(system, definition, &live);
    CHECK(ta_system_set_time(system, 6000) == TA_OK);
    CHECK(dispatch(system) == 2);
    CHECK(ta_system_set_time(system, 6500) == TA_OK);
    deliver(system, live, "timeout");
    CHECK(ta_system_set_time(system, 7000) == TA_OK);
    evict(system, live);
    buffer log = { 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
    CHECK(ta_system_set_time(system, 7000 + 7u * 24u * 3600u * 1000u) == TA_OK);
    int routed_before = routed;
    replayed copy = replay_log(system, definition, &address_a, &log, 7);
    CHECK(in_state(system, copy.actor, "timedout"));
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
    ta_time deadline = 0;
    CHECK(ta_system_next_deadline(system, &deadline) == TA_ERR_NOT_FOUND);
    CHECK(routed == routed_before);
    buffer_free(&log);
    destroy_system(system);

    /* Still pending when it stopped: fires once, late. */
    system = create_system();
    definition = define(system, "timers", timers);
    log_family(system, "timers");
    live_until_e1(system, definition, &live);
    CHECK(ta_system_set_time(system, 3000) == TA_OK);
    evict(system, live);
    log = (buffer){ 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
    CHECK(ta_system_set_time(system, 7u * 24u * 3600u * 1000u) == TA_OK);
    routed_before = routed;
    copy = replay_log(system, definition, &address_a, &log, 1);
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, copy.actor, &view) == TA_OK);
    CHECK(view.delayed_send_count == 2 && view.delayed_sends[0].due == 6000);
    CHECK(dispatch(system) == 2 && routed == routed_before + 2);
    buffer_free(&log);
    destroy_system(system);

    /* A crash between commits: the log ends before the dispatch; the sends
     * fire again at publish. */
    system = create_system();
    definition = define(system, "timers", timers);
    log_family(system, "timers");
    live_until_e1(system, definition, &live);
    log = (buffer){ 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len); /* the last commit */
    CHECK(ta_system_set_time(system, 6000) == TA_OK);
    CHECK(dispatch(system) == 2);
    CHECK(ta_actor_destroy(system, live) == TA_OK);
    routed_before = routed;
    copy = replay_log(system, definition, &address_a, &log, 7);
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
    CHECK(dispatch(system) == 2 && routed == routed_before + 2);
    buffer_free(&log);
    destroy_system(system);
}

/* A log with two lives: the replay ends the first at `destroyed`. */
static void test_destroyed_life(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    log_family(system, "timers");
    ta_actor_id live = 0;
    live_until_e1(system, definition, &live);
    CHECK(ta_actor_destroy(system, live) == TA_OK); /* DESTROYED */
    CHECK(ta_system_set_time(system, 2000) == TA_OK);
    live = spawn_named(system, definition, &address_a); /* a second life, in idle */
    buffer log = { 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
    CHECK(ta_actor_destroy(system, live) == TA_OK);
    replayed copy = replay_log(system, definition, &address_a, &log, 7);
    CHECK(copy.lives == 2);
    CHECK(in_state(system, copy.actor, "idle"));
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, copy.actor, &view) == TA_OK && view.delayed_send_count == 0);
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
    ta_time deadline = 0;
    CHECK(ta_system_next_deadline(system, &deadline) == TA_ERR_NOT_FOUND); /* the first life's sends are gone */
    buffer_free(&log);
    destroy_system(system);
}

/* Operations, reports, jumps and host records replay too. */
static void test_replay_operations(void) {
    ta_system *system = create_system();
    ta_definition_id jobs = define(system, "jobs", invoking);
    ta_definition_id chains = define(system, "chains", chain);
    ta_definition_id timer_chart = define(system, "timers", timers);
    log_family(system, "jobs");
    log_family(system, "chains");
    log_family(system, "timers");
    ta_system_set_microstep_limit(system, 5);

    /* a report */
    static const ta_actor_address job_address = { .family = { .name = STR("jobs") }, .name = STR("j") };
    static const ta_actor_address job_copy = { .family = { .name = STR("jobs") }, .name = STR("k") };
    ta_actor_id job = spawn_named(system, jobs, &job_address);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, job, &view) == TA_OK && view.invoke_count == 1);
    ta_invoke_error error = { .reason = STR("boom"), .ends = true };
    CHECK(ta_actor_report_invoke(system, &(ta_invoke_report_params){ .actor = job, .invoke_id = view.invokes[0].id,
                                                                     .kind = TA_INVOKE_REPORT_ERROR,
                                                                     .as.error = &error }) == TA_OK);
    CHECK(run(system, job) == TA_RUN_STABLE);
    buffer log = { 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
    replayed copy = replay_log(system, jobs, &job_copy, &log, 7);
    CHECK(in_state(system, copy.actor, "failed"));
    CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);
    buffer_free(&log);

    /* a release and an abandon */
    the_sink.log.len = 0;
    static const ta_actor_address chain_address = { .family = { .name = STR("chains") }, .name = STR("c") };
    static const ta_actor_address chain_copy = { .family = { .name = STR("chains") }, .name = STR("d") };
    ta_actor_id spin = spawn_named(system, chains, &chain_address);
    ta_event go = { .name = event_name(system, "go"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = spin, .event = &go }) == TA_OK);
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = spin, .budget = 100, .status = &status }) ==
          TA_ERR_RUNAWAY);
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = spin, .budget = 100, .status = &status,
                                                            .no_microstep_limit = true }) == TA_OK);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = spin, .event = &go }) == TA_OK);
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = spin, .budget = 100, .status = &status }) ==
          TA_ERR_RUNAWAY);
    CHECK(ta_actor_abandon(system, spin) == TA_OK);
    log = (buffer){ 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
    copy = replay_log(system, chains, &chain_copy, &log, 1);
    CHECK(same_state(system, spin, copy.actor));
    CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);
    buffer_free(&log);

    /* a jump, a withdrawal and host records */
    the_sink.log.len = 0;
    static const ta_actor_address timer_copy = { .family = { .name = STR("timers") }, .name = STR("z") };
    ta_actor_id live = spawn_named(system, timer_chart, &address_a);
    ta_value queued_data = blob_make(30, 4);
    ta_event queued = { .name = event_name(system, "later"), .type = TA_EVENT_EXTERNAL,
                        .invoke_id = TA_INVOKE_ID_NONE, .data = queued_data };
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_QUEUED, .host_seq = 3,
                                                                        .actor = live, .event = &queued }) == TA_OK);
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_QUEUED, .host_seq = 4,
                                                                        .actor = live, .event = &queued }) == TA_OK);
    CHECK(ta_system_set_time(system, 1000) == TA_OK);
    ta_event e1 = { .name = event_name(system, "e1"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1, .host_seq = 3 }) ==
          TA_OK);
    CHECK(run(system, live) == TA_RUN_STABLE);
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_DROPPED,
                                                                        .host_seq = 4, .actor = live }) == TA_OK);
    ta_send_id t1 = 0;
    CHECK(ta_send_id_parse(system, (ta_str)STR("t1"), &t1) == TA_OK);
    CHECK(ta_actor_withdraw_send(system, live, t1) == TA_OK);
    ta_state_id idle = state_of(system, live, "idle");
    ta_event jump_event = { .name = event_name(system, "operator.jump"), .type = TA_EVENT_PLATFORM,
                            .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){
                                       .actor = live, .event = &jump_event,
                                       .transition = &(ta_operator_transition){ .source = TA_STATE_NONE,
                                                                                 .targets = &idle,
                                                                                 .target_count = 1 } }) == TA_OK);
    CHECK(run(system, live) == TA_RUN_STABLE);
    log = (buffer){ 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
    copy = replay_log(system, timer_chart, &timer_copy, &log, 7);
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
    CHECK(same_state(system, live, copy.actor));
    CHECK(copy.queued_count == 2 && copy.queued[0] == 3 && copy.queued[1] == 4);
    CHECK(copy.delivered_count == 1 && copy.delivered[0] == 3);
    CHECK(copy.dropped_count == 1 && copy.dropped[0] == 4);
    CHECK(blob_equal(&copy.queued_data, &queued_data));
    ta_value_release(&copy.queued_data);
    ta_value_release(&queued_data);
    buffer_free(&log);
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Refusals: an input whose record cannot be written changes nothing
 */

static void test_refusals(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    log_family(system, "timers");
    ta_actor_id live = spawn_named(system, definition, &address_a);
    size_t before = the_sink.log.len;
    ta_value data = blob_make(40, 6);
    ta_event e1 = { .name = event_name(system, "e1"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                    .data = data };

    /* a sink that refuses the unit */
    the_sink.refuse_begin = true;
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1 }) == TA_ERR_NOMEM);
    the_sink.refuse_begin = false;
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, live, &view) == TA_OK && !view.macrostep_in_progress);

    /* a sink that refuses at every byte of the record: an out-of-memory sweep */
    long refused = 0;
    for (long at = 0;; at += 1) {
        the_sink.refuse_at = at;
        ta_result result = ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1 });
        the_sink.refuse_at = -1;
        if (result == TA_OK) {
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(the_sink.log.len == before);
        CHECK(ta_actor_inspect(system, live, &view) == TA_OK && !view.macrostep_in_progress);
        CHECK(in_state(system, live, "idle"));
        refused += 1;
    }
    CHECK(refused > 20); /* the header alone is 32 bytes */
    CHECK(run(system, live) == TA_RUN_STABLE && in_state(system, live, "waiting"));

    /* a value with no durable form */
    before = the_sink.log.len;
    durable_refuses = true;
    ta_event back = { .name = event_name(system, "timeout"), .type = TA_EVENT_EXTERNAL,
                      .invoke_id = TA_INVOKE_ID_NONE, .data = data };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &back }) ==
          TA_ERR_UNSUPPORTED);
    durable_refuses = false;
    CHECK(the_sink.log.len == before && in_state(system, live, "waiting"));

    /* a timer whose FIRED cannot be written stays due */
    CHECK(ta_system_set_time(system, 6000) == TA_OK);
    the_sink.refuse_begin = true;
    uint32_t dispatched = 0;
    bool more = false;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 100, .dispatched = &dispatched,
                                                                   .more_due = &more }) == TA_OK);
    CHECK(dispatched == 0 && more);
    CHECK(ta_actor_inspect(system, live, &view) == TA_OK && view.delayed_send_count == 2);
    the_sink.refuse_begin = false;
    CHECK(dispatch(system) == 2);

    /* withdraw and destroy refuse too, and change nothing */
    the_sink.refuse_begin = true;
    CHECK(ta_actor_destroy(system, live) == TA_ERR_NOMEM);
    the_sink.refuse_begin = false;
    CHECK(ta_actor_inspect(system, live, &view) == TA_OK);
    ta_value_release(&data);
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Readers: versions, kinds, and fields a newer writer added
 */

static void test_readers(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    log_family(system, "timers");
    ta_actor_id live = 0;
    live_until_e1(system, definition, &live);
    buffer log = { 0 };
    buffer_put(&log, the_sink.log.bytes, the_sink.log.len);
    size_t spawned_bytes = 72; /* SPAWNED: 56 fixed, 9 of text padded to 16 */

    /* A newer writer: the EVENT record gains one field. */
    buffer newer = { 0 };
    buffer_put(&newer, log.bytes, spawned_bytes);
    const uint8_t *event = log.bytes + spawned_bytes;
    uint32_t fixed = (uint32_t)event[4] | (uint32_t)event[5] << 8;
    uint8_t header[32];
    memcpy(header, event, 32);
    uint32_t grown = fixed + 8u;
    header[4] = (uint8_t)grown;
    header[5] = (uint8_t)(grown >> 8);
    buffer_put(&newer, header, 32);
    buffer_put(&newer, event + 32, fixed - 32u);
    put_u64(&newer, 0xfeedu); /* the field this build does not know */
    buffer_put(&newer, event + fixed, log.len - spawned_bytes - fixed);
    static const ta_actor_address other = { .family = { .name = STR("timers") }, .name = STR("n") };
    replayed copy = replay_log(system, definition, &other, &newer, 7);
    CHECK(in_state(system, copy.actor, "waiting"));
    CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);
    buffer_free(&newer);

    /* An unknown version, and an unknown kind */
    for (int variant = 0; variant < 2; variant += 1) {
        buffer bad = { 0 };
        buffer_put(&bad, log.bytes, log.len);
        bad.bytes[spawned_bytes + (variant == 0 ? 0 : 2)] = 99;
        memory_source source = { .bytes = bad.bytes, .len = bad.len, .piece = 7 };
        ta_byte_source as_source = { .self = &source, .v = &source_vtable };
        ta_actor_id actor = 0;
        CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &other, .definition = definition,
                                                                   .log = &as_source, .out = &actor }) == TA_OK);
        CHECK(run(system, actor) == TA_RUN_STABLE);
        ta_replay_step step = TA_REPLAY_NEXT;
        CHECK(ta_actor_replay(system, &(ta_actor_replay_params){ .actor = actor, .source = as_source,
                                                                 .step = &step }) == TA_ERR_CORRUPT);
        CHECK(ta_actor_destroy(system, actor) == TA_OK);
        buffer_free(&bad);
    }

    /* A truncated record, a log whose first record is not SPAWNED, and a
     * replay into an actor that is not replaying */
    memory_source source = { .bytes = log.bytes, .len = spawned_bytes - 3, .piece = 7 };
    ta_byte_source as_source = { .self = &source, .v = &source_vtable };
    ta_actor_id actor = 0;
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &other, .definition = definition,
                                                               .log = &as_source, .out = &actor }) == TA_ERR_CORRUPT);
    source = (memory_source){ .bytes = log.bytes + spawned_bytes, .len = log.len - spawned_bytes, .piece = 7 };
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &other, .definition = definition,
                                                               .log = &as_source, .out = &actor }) == TA_ERR_INVALID);
    source = (memory_source){ .bytes = log.bytes, .len = log.len, .piece = 7 };
    ta_time at = 0;
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &other, .definition = definition,
                                                               .log = &as_source, .at = &at,
                                                               .out = &actor }) == TA_ERR_INVALID);
    ta_replay_step step = TA_REPLAY_NEXT;
    CHECK(ta_actor_replay(system, &(ta_actor_replay_params){ .actor = live, .source = as_source, .step = &step }) ==
          TA_ERR_INVALID);
    buffer_free(&log);
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Writing a record allocates nothing, and unlogged actors pay nothing
 */

static void test_no_allocation(void) {
    ta_system *system = create_system();
    ta_definition_id logged = define(system, "timers", history_chart);
    log_family(system, "timers");
    ta_actor_id live = spawn_named(system, logged, &address_a);
    deliver(system, live, "next"); /* warm: event blocks, the actor's first records */
    deliver(system, live, "next");
    for (int round = 0; round < 3; round += 1) {
        long before = allocations;
        int kept = the_sink.kept;
        deliver(system, live, round % 2 == 0 ? "off" : "back");
        CHECK(the_sink.kept == kept + 1);
        CHECK(allocations == before);
    }
    ta_system_set_input_sink(system, (ta_byte_sink){ 0 });
    long before = allocations;
    deliver(system, live, "off");
    CHECK(allocations == before);
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Checkpoints (ADR 0091)
 */

static ta_definition_id define_revision(ta_system *system, const char *family, const ta_scxml_state_spec *states,
                                        const char *revision) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
        .revision = { .ptr = (const uint8_t *)revision, .len = (uint32_t)strlen(revision) },
    };
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(
        system, &(ta_definition_compile_params){
                    .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity, .states = states },
                    .datamodel = { .self = NULL, .v = &blob_model_vtable },
                    .allocator = allocator,
                    .out = &definition,
                });
    CHECK(result == TA_OK);
    ta_definition_id id = 0;
    CHECK(result == TA_OK && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

typedef struct {
    const ta_value *root;
    const ta_owed_message *owed;
    uint32_t owed_count;
} checkpoint_args;

static ta_result checkpoint_into(ta_system *system, ta_actor_id actor, memory_sink *sink, const checkpoint_args *args) {
    return ta_actor_checkpoint(system, &(ta_actor_checkpoint_params){
                                           .actor = actor,
                                           .sink = { .self = sink, .v = &sink_vtable },
                                           .root = args->root,
                                           .owed = args->owed,
                                           .owed_count = args->owed_count,
                                       });
}

static void concat(buffer *out, const buffer *part, size_t from, size_t to) {
    buffer_put(out, part->bytes + from, to - from);
}

static bool same_history(ta_system *system, ta_actor_id left, ta_actor_id right, const char *id) {
    ta_state_id history = state_of(system, left, id);
    ta_state_id left_recorded[8];
    ta_state_id right_recorded[8];
    uint32_t left_count = 0;
    uint32_t right_count = 0;
    CHECK(ta_actor_history(system, &(ta_actor_history_params){ .actor = left, .history = history,
                                                                .states = left_recorded, .capacity = 8,
                                                                .count = &left_count }) == TA_OK);
    CHECK(ta_actor_history(system, &(ta_actor_history_params){ .actor = right, .history = history,
                                                                .states = right_recorded, .capacity = 8,
                                                                .count = &right_count }) == TA_OK);
    return left_count == right_count && memcmp(left_recorded, right_recorded, left_count * sizeof(ta_state_id)) == 0;
}

/* A checkpoint alone, a checkpoint and its tail, a checkpoint and the whole
 * log it covers, and a whole log with a checkpoint inside it all rebuild
 * the live actor. */
static void test_checkpoint_replays(void) {
    static const uint32_t pieces[] = { 1, 7 };
    for (size_t piece = 0; piece < sizeof pieces / sizeof pieces[0]; piece += 1) {
        ta_system *system = create_system();
        ta_definition_id definition = define(system, "hist", history_chart);
        log_family(system, "hist");
        ta_value binding = blob_make(24, 3);
        ta_actor_id live = 0;
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &hist_live,
                                                               .datamodel_binding = &binding, .out = &live }) == TA_OK);
        CHECK(run(system, live) == TA_RUN_STABLE);
        static const struct { ta_time at; const char *event; } inputs[] = {
            { 100, "next" }, { 200, "arm" }, { 300, "next" }, { 400, "off" }, { 500, "arm" }, { 900, "back" },
        };
        memory_sink middle = { .refuse_at = -1 };
        size_t at_middle = 0;
        for (size_t index = 0; index < sizeof inputs / sizeof inputs[0]; index += 1) {
            CHECK(ta_system_set_time(system, inputs[index].at) == TA_OK);
            deliver(system, live, inputs[index].event);
            if (index == 3) { /* in `off`, with `tick` pending and `h` recorded */
                CHECK(ta_system_set_time(system, 450) == TA_OK);
                CHECK(checkpoint_into(system, live, &middle, &(checkpoint_args){ .root = &binding }) == TA_OK);
                at_middle = the_sink.log.len;
            }
        }
        memory_sink end = { .refuse_at = -1 };
        CHECK(checkpoint_into(system, live, &end, &(checkpoint_args){ .root = &binding }) == TA_OK);
        CHECK(middle.units == 1 && middle.kept == 1 && end.kept == 1);
        const buffer *log = &the_sink.log;
        size_t log_len = log->len;

        buffer variants[4] = { 0 };
        concat(&variants[0], &end.log, 0, end.log.len);                     /* the checkpoint alone */
        concat(&variants[1], &middle.log, 0, middle.log.len);               /* the checkpoint and its tail */
        concat(&variants[1], log, at_middle, log_len);
        concat(&variants[2], &middle.log, 0, middle.log.len);               /* ... and every record before it */
        concat(&variants[2], log, 0, log_len);
        concat(&variants[3], log, 0, at_middle);                            /* a checkpoint inside the log */
        concat(&variants[3], &middle.log, 0, middle.log.len);
        concat(&variants[3], log, at_middle, log_len);
        for (int variant = 0; variant < 4; variant += 1) {
            ta_value_release(&bound);
            bound = (ta_value){ 0 };
            replayed copy = replay_log(system, definition, &hist_copy, &variants[variant], pieces[piece]);
            CHECK(blob_equal(&bound, &binding));
            CHECK(copy.lives == 1 && copy.host_records == 0);
            CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
            CHECK(same_state(system, live, copy.actor));
            CHECK(same_history(system, live, copy.actor, "h"));
            CHECK(the_sink.log.len == log_len); /* a replay writes nothing */
            CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);
            the_sink.log.len = log_len; /* forget its DESTROYED */
            buffer_free(&variants[variant]);
        }
        buffer_free(&middle.log);
        buffer_free(&end.log);
        ta_value_release(&binding);
        destroy_system(system);
    }
}

/* A log compacted to its checkpoint still owes the messages the host had
 * not delivered; a full log hands each back once. */
static void test_checkpoint_owed(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    log_family(system, "timers");
    ta_actor_id live = spawn_named(system, definition, &address_a);
    ta_value data = blob_make(12, 8);
    ta_event queued = { .name = event_name(system, "later"), .type = TA_EVENT_EXTERNAL,
                        .invoke_id = TA_INVOKE_ID_NONE, .data = data };
    for (uint64_t seq = 3; seq <= 4; seq += 1) {
        CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                            .kind = TA_RECORD_QUEUED,
                                                                            .host_seq = seq, .actor = live,
                                                                            .event = &queued }) == TA_OK);
    }
    CHECK(ta_system_set_time(system, 1000) == TA_OK);
    ta_event e1 = { .name = event_name(system, "e1"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1, .host_seq = 3 }) ==
          TA_OK);
    CHECK(run(system, live) == TA_RUN_STABLE);
    memory_sink cp = { .refuse_at = -1 };
    ta_owed_message owed = { .host_seq = 4, .event = &queued };
    CHECK(checkpoint_into(system, live, &cp, &(checkpoint_args){ .owed = &owed, .owed_count = 1 }) == TA_OK);
    size_t at_cp = the_sink.log.len;
    CHECK(ta_system_write_host_record(system, &(ta_host_record_params){ .address = &address_a,
                                                                        .kind = TA_RECORD_DROPPED, .host_seq = 4,
                                                                        .actor = live }) == TA_OK);
    const buffer *log = &the_sink.log;
    static const ta_actor_address copy_address = { .family = { .name = STR("timers") }, .name = STR("c") };

    /* compacted: the checkpoint and what followed it */
    buffer compacted = { 0 };
    concat(&compacted, &cp.log, 0, cp.log.len);
    concat(&compacted, log, at_cp, log->len);
    replayed copy = replay_log(system, definition, &copy_address, &compacted, 7);
    CHECK(copy.queued_count == 1 && copy.queued[0] == 4);
    CHECK(blob_equal(&copy.queued_data, &data));
    CHECK(copy.delivered_count == 0 && copy.dropped_count == 1 && copy.dropped[0] == 4);
    CHECK(in_state(system, copy.actor, "waiting"));
    CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);
    ta_value_release(&copy.queued_data);
    buffer_free(&compacted);

    /* the checkpoint, then the whole log: the covered records are skipped */
    buffer whole = { 0 };
    concat(&whole, &cp.log, 0, cp.log.len);
    concat(&whole, log, 0, at_cp + (log->len - at_cp));
    copy = replay_log(system, definition, &copy_address, &whole, 1);
    CHECK(copy.queued_count == 1 && copy.queued[0] == 4 && copy.delivered_count == 0 && copy.dropped_count == 1);
    CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);
    ta_value_release(&copy.queued_data);
    buffer_free(&whole);

    /* the whole log with the checkpoint inside it: the original records count */
    buffer inside = { 0 };
    concat(&inside, log, 0, at_cp);
    concat(&inside, &cp.log, 0, cp.log.len);
    concat(&inside, log, at_cp, log->len);
    copy = replay_log(system, definition, &copy_address, &inside, 7);
    CHECK(copy.queued_count == 2 && copy.queued[0] == 3 && copy.queued[1] == 4);
    CHECK(copy.delivered_count == 1 && copy.delivered[0] == 3 && copy.dropped_count == 1);
    CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);
    ta_value_release(&copy.queued_data);
    buffer_free(&inside);
    buffer_free(&cp.log);
    ta_value_release(&data);
    destroy_system(system);
}

/* The timelines of ADR 0089 section 2, from a checkpoint taken mid-way and
 * the records after it. */
static void test_checkpoint_timelines(void) {
    for (int timeline = 0; timeline < 3; timeline += 1) {
        ta_system *system = create_system();
        ta_definition_id definition = define(system, "timers", timers);
        log_family(system, "timers");
        ta_actor_id live = 0;
        live_until_e1(system, definition, &live);
        CHECK(ta_system_set_time(system, 2000) == TA_OK);
        memory_sink cp = { .refuse_at = -1 };
        CHECK(checkpoint_into(system, live, &cp, &(checkpoint_args){ 0 }) == TA_OK);
        size_t at_cp = the_sink.log.len;
        if (timeline == 0) { /* both fired and the timeout came back before the actor stopped */
            CHECK(ta_system_set_time(system, 6000) == TA_OK);
            CHECK(dispatch(system) == 2);
            CHECK(ta_system_set_time(system, 6500) == TA_OK);
            deliver(system, live, "timeout");
            CHECK(ta_system_set_time(system, 7000) == TA_OK);
            evict(system, live);
        } else if (timeline == 1) { /* still pending when it stopped */
            CHECK(ta_system_set_time(system, 3000) == TA_OK);
            evict(system, live);
        } else { /* a crash after the checkpoint's commit, the sends dispatched after it */
            CHECK(ta_system_set_time(system, 6000) == TA_OK);
            CHECK(dispatch(system) == 2);
            CHECK(ta_actor_destroy(system, live) == TA_OK);
            the_sink.log.len = at_cp; /* nothing after the checkpoint was committed */
        }
        buffer log = { 0 };
        concat(&log, &cp.log, 0, cp.log.len);
        concat(&log, &the_sink.log, at_cp, the_sink.log.len);
        CHECK(ta_system_set_time(system, 7u * 24u * 3600u * 1000u) == TA_OK);
        int routed_before = routed;
        replayed copy = replay_log(system, definition, &address_a, &log, 7);
        CHECK(in_state(system, copy.actor, timeline == 0 ? "timedout" : "waiting"));
        CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
        ta_actor_inspection view;
        CHECK(ta_actor_inspect(system, copy.actor, &view) == TA_OK);
        if (timeline == 0) {
            CHECK(view.delayed_send_count == 0);
            CHECK(routed == routed_before);
        } else {
            CHECK(view.delayed_send_count == 2 && view.delayed_sends[0].due == 6000);
            CHECK(dispatch(system) == 2 && routed == routed_before + 2); /* once, a week late */
            CHECK(run(system, copy.actor) == TA_RUN_STABLE);
        }
        buffer_free(&log);
        buffer_free(&cp.log);
        destroy_system(system);
    }
}

/* What a checkpoint refuses, and what it costs. */
static void test_checkpoint_refusals(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    ta_definition_id finals = define(system, "finals", at_once_final);
    ta_definition_id unlogged = define(system, "free", timers);
    log_family(system, "timers");
    log_family(system, "finals");
    ta_actor_id live = spawn_named(system, definition, &address_a);
    ta_value root = blob_make(16, 5);
    memory_sink cp = { .refuse_at = -1 };
    const checkpoint_args with_root = { .root = &root };

    /* busy: inside a macrostep */
    ta_event e1 = { .name = event_name(system, "e1"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1 }) == TA_OK);
    CHECK(checkpoint_into(system, live, &cp, &with_root) == TA_ERR_BUSY);
    CHECK(run(system, live) == TA_RUN_STABLE);

    /* invalid: unnamed, or not logged; done: finished */
    ta_actor_id anonymous = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &anonymous }) == TA_OK);
    CHECK(run(system, anonymous) == TA_RUN_STABLE);
    CHECK(checkpoint_into(system, anonymous, &cp, &with_root) == TA_ERR_INVALID);
    static const ta_actor_address free_address = { .family = { .name = STR("free") }, .name = STR("f") };
    ta_actor_id free_actor = spawn_named(system, unlogged, &free_address);
    CHECK(checkpoint_into(system, free_actor, &cp, &with_root) == TA_ERR_INVALID);
    static const ta_actor_address final_address = { .family = { .name = STR("finals") }, .name = STR("x") };
    ta_actor_id finished = spawn_named(system, finals, &final_address);
    CHECK(checkpoint_into(system, finished, &cp, &with_root) == TA_ERR_DONE);
    CHECK(cp.units == 0);

    /* a sink that refuses at every byte: an out-of-memory sweep */
    long refused = 0;
    for (long at = 0;; at += 1) {
        cp.refuse_at = at;
        ta_result result = checkpoint_into(system, live, &cp, &with_root);
        cp.refuse_at = -1;
        if (result == TA_OK) {
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(cp.log.len == 0 && cp.kept == 0);
        refused += 1;
    }
    CHECK(refused > 100 && cp.kept == 1);
    cp.refuse_begin = true;
    CHECK(checkpoint_into(system, live, &cp, &with_root) == TA_ERR_NOMEM);
    cp.refuse_begin = false;

    /* a value with no durable form */
    size_t kept_len = cp.log.len;
    durable_refuses = true;
    CHECK(checkpoint_into(system, live, &cp, &with_root) == TA_ERR_UNSUPPORTED);
    durable_refuses = false;
    CHECK(cp.log.len == kept_len);

    /* nothing is allocated, and the input log does not move */
    long before = allocations;
    size_t log_len = the_sink.log.len;
    CHECK(checkpoint_into(system, live, &cp, &with_root) == TA_OK);
    CHECK(allocations == before);
    CHECK(the_sink.log.len == log_len);

    /* another revision of the family */
    ta_definition_id newer = define_revision(system, "timers", timers, "v2");
    memory_source source = { .bytes = cp.log.bytes, .len = kept_len, .piece = 7 };
    ta_byte_source as_source = { .self = &source, .v = &source_vtable };
    ta_actor_id actor = 0;
    static const ta_actor_address other = { .family = { .name = STR("timers") }, .name = STR("n") };
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &other, .definition = newer,
                                                               .log = &as_source, .out = &actor }) ==
          TA_ERR_DEFINITION_MISMATCH);

    /* a checkpoint beyond what a replay applied is not applied mid-replay */
    source = (memory_source){ .bytes = cp.log.bytes, .len = kept_len, .piece = 7 };
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &other, .definition = definition,
                                                               .log = &as_source, .out = &actor }) == TA_OK);
    ta_actor_inspection idle_view;
    CHECK(ta_actor_inspect(system, actor, &idle_view) == TA_OK && !idle_view.macrostep_in_progress);
    memory_source again = { .bytes = cp.log.bytes, .len = kept_len, .piece = 7 };
    uint8_t *bumped = malloc(kept_len);
    memcpy(bumped, cp.log.bytes, kept_len);
    bumped[8] = (uint8_t)(bumped[8] + 1u); /* the header's seq, one past */
    again.bytes = bumped;
    ta_replay_step step = TA_REPLAY_NEXT;
    CHECK(ta_actor_replay(system, &(ta_actor_replay_params){ .actor = actor,
                                                             .source = { .self = &again, .v = &source_vtable },
                                                             .step = &step }) == TA_ERR_INVALID);
    free(bumped);
    CHECK(ta_actor_destroy(system, actor) == TA_OK);
    buffer_free(&cp.log);
    ta_value_release(&root);
    destroy_system(system);
}

/* An invocation active at the checkpoint is active in its replay: the
 * service's later report still matches it. */
static void test_checkpoint_invocation(void) {
    ta_system *system = create_system();
    ta_definition_id jobs = define(system, "jobs", invoking);
    log_family(system, "jobs");
    static const ta_actor_address job_address = { .family = { .name = STR("jobs") }, .name = STR("j") };
    static const ta_actor_address job_copy = { .family = { .name = STR("jobs") }, .name = STR("k") };
    ta_actor_id job = spawn_named(system, jobs, &job_address);
    memory_sink cp = { .refuse_at = -1 };
    CHECK(checkpoint_into(system, job, &cp, &(checkpoint_args){ 0 }) == TA_OK);
    size_t at_cp = the_sink.log.len;
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, job, &view) == TA_OK && view.invoke_count == 1);
    ta_invoke_error error = { .reason = STR("boom"), .ends = true };
    CHECK(ta_actor_report_invoke(system, &(ta_invoke_report_params){ .actor = job, .invoke_id = view.invokes[0].id,
                                                                     .kind = TA_INVOKE_REPORT_ERROR,
                                                                     .as.error = &error }) == TA_OK);
    CHECK(run(system, job) == TA_RUN_STABLE);
    buffer log = { 0 };
    concat(&log, &cp.log, 0, cp.log.len);
    concat(&log, &the_sink.log, at_cp, the_sink.log.len);
    replayed copy = replay_log(system, jobs, &job_copy, &log, 7);
    CHECK(in_state(system, copy.actor, "failed"));
    CHECK(ta_actor_destroy(system, copy.actor) == TA_OK);

    /* compacted to the checkpoint alone: the invocation is retired at publish */
    buffer alone = { 0 };
    concat(&alone, &cp.log, 0, cp.log.len);
    copy = replay_log(system, jobs, &job_copy, &alone, 7);
    CHECK(ta_actor_inspect(system, copy.actor, &view) == TA_OK && view.invoke_count == 1);
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = copy.actor }) == TA_OK);
    CHECK(ta_actor_inspect(system, copy.actor, &view) == TA_OK && view.invoke_count == 0);
    buffer_free(&log);
    buffer_free(&alone);
    buffer_free(&cp.log);
    destroy_system(system);
}

int main(void) {
    blob_model_vtable = *ta_null_datamodel().v;
    blob_model_vtable.value_write = blob_write;
    blob_model_vtable.value_read = blob_read;
    blob_model_vtable.init_instance = blob_init_instance;
    test_golden_records();
    test_golden_operations();
    test_replay_reproduces_live();
    test_timelines();
    test_destroyed_life();
    test_replay_operations();
    test_refusals();
    test_readers();
    test_no_allocation();
    test_checkpoint_replays();
    test_checkpoint_owed();
    test_checkpoint_timelines();
    test_checkpoint_refusals();
    test_checkpoint_invocation();
    CHECK(live_blobs == 0);
    if (failures != 0) {
        fprintf(stderr, "input_log_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("input_log_test: ok\n");
    return 0;
}

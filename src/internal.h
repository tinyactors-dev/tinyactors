#ifndef TINYACTORS_INTERNAL_H_INCLUDED
#define TINYACTORS_INTERNAL_H_INCLUDED

#include "tinyactors.h"

/* Root-owned private module contract. No libc and no value macros. */
union tai_alignment { long double floating; uint64_t integer; void *pointer; };
struct tai_alignment_probe { char byte; union tai_alignment value; };
enum { TAI_ALIGNMENT = offsetof(struct tai_alignment_probe, value) };

typedef struct tai_actor tai_actor_t;
typedef struct tai_message tai_message_t;
typedef struct tai_timer tai_timer_t;
typedef struct tai_io tai_io_t;
typedef struct tai_invocation tai_invocation_t;
typedef struct tai_registry tai_registry_t;
typedef struct tai_actor_extra tai_actor_extra_t;
typedef struct tai_history tai_history_t;
typedef struct tai_activation tai_activation_t;
typedef struct tai_initial_input tai_initial_input_t;
typedef struct tai_span tai_span_t;

struct ta_definition {
    ta_system_t *system;
    ta_arena_t *arena;
    ta_definition_desc_t description;
    ta_definition_options_t storage;
    uint64_t revision;
    size_t references;
    size_t held_by_definitions; /* Included in references; incoming inline pins. */
    size_t held_by_registry;    /* Included in references; registry key pins. */
    ta_definition_t *next;
    uint32_t *subtree_end; /* Exclusive preorder subtree end, including history. */
    uint32_t *depth;       /* Root depth is zero. */
};

struct tai_registry {
    tai_registry_t *left;
    tai_registry_t *right;
    tai_registry_t *parent;
    uint64_t priority;
    ta_arena_t *arena;
    ta_text_t key;
    ta_definition_t *definition;
    ta_actor_ref_t actor;
};

struct ta_system {
    ta_arena_t *arena;
    ta_system_config_t config;
    ta_system_stats_t stats;
    ta_definition_t *definitions;
    tai_registry_t *definition_registry;
    tai_registry_t *names;
    tai_actor_t *actors;
    tai_actor_t *actor_index;
    tai_actor_t *ready_head;
    tai_actor_t *ready_tail;
    tai_timer_t *timers;
    tai_io_t *requests;
    tai_io_t *outbox_head;
    tai_io_t *outbox_tail;
    tai_invocation_t *invocations;
    uint64_t next_revision;
    uint64_t next_actor;
    uint64_t next_message;
    uint64_t next_request;
    uint64_t next_invocation;
    uint64_t next_sendid;
    uint64_t now_ns;
    uint64_t mutation;
    size_t processor_count;
    size_t service_count;
    uint8_t running;
    uint8_t shutdown;
    uint8_t time_set;
    uint8_t mutation_exhausted;
    char *identity_buffer; /* Reused borrowed session/location rendering. */
    size_t identity_capacity;
};

/* Only the common actor path is inline. Optional configurations, histories,
 * locations, tracing and custom limits are allocated on first use. */
struct tai_actor {
    ta_system_t *system;
    ta_arena_t *arena;
    ta_definition_t *definition;
    ta_definition_t *identity_definition;
    uint64_t id;
    tai_actor_t *left, *right, *parent;
    tai_actor_t *next, *previous;
    tai_actor_t *ready_next, *ready_previous;
    tai_registry_t *name;
    const ta_actor_limits_t *limits;
    ta_data_view_t data;
    tai_actor_extra_t *extra;
    tai_message_t *external_head, *external_tail;
    tai_message_t *internal_head, *internal_tail;
    tai_message_t *current;
    tai_message_t *error_next;
    tai_initial_input_t *initial;
    size_t external_count, internal_count, queued_bytes;
    size_t microsteps, actions;
    uint32_t leaf;
    ta_actor_status_t status;
    ta_status_t reason;
    uint8_t queued, stable, stop, initialized;
    uint8_t eventless, error_slot, error_dequeued, notified;
    uint8_t current_error;
};

struct tai_initial_input {
    ta_arena_t *arena;
    ta_value_t seed;
    ta_body_t parameters;
    const ta_trace_context_t *trace;
};

struct tai_activation {
    tai_activation_t *next;
    ta_arena_t *arena;
    uint32_t state;
};

struct tai_history {
    tai_history_t *next;
    ta_arena_t *arena;
    uint32_t state;
    uint32_t *targets;
    size_t count;
};

struct tai_actor_extra {
    ta_arena_t *arena;
    ta_actor_limits_t custom_limits;
    uint8_t *parallel; /* Active-state bitset, only for parallel machines. */
    uint8_t *late; /* Initialized declaration owners for late binding. */
    uint32_t *invoke_progress; /* Per-owner next invoke, only when invokes used. */
    tai_history_t *history;
    tai_activation_t *activations;
    ta_arena_t *configuration_arena;
    ta_arena_t *state_arena; /* Separate replacement state, reclaimed on upgrade. */
    ta_arena_t *macrostep;
    tai_span_t *span;
    const ta_trace_context_t *trace;
    ta_io_location_t *locations;
    size_t location_count;
    size_t delayed_count, invocation_count, request_count, request_bytes;
    tai_timer_t *timers;
    tai_invocation_t *invocations;
    tai_invocation_t *parent_invocation;
};

struct ta_context {
    tai_actor_t *actor;
    ta_context_view_t view;
    ta_system_values_t values;
    ta_io_location_t scxml_location;
    ta_event_t error_event;
    uint8_t effects;
};

/* Error-slot messages use tagged queue links 1/2, never dereferenced. Regular
 * message metadata is allocated only for actual queued/current events. */
struct tai_message {
    tai_message_t *next;
    ta_arena_t *arena;
    ta_event_t event;
    size_t capacity;
    ta_invocation_ref_t invocation;
};

typedef struct tai_delivery {
    ta_event_t event;
    ta_invocation_ref_t invocation;
    uint8_t internal;
} tai_delivery_t;
ta_status_t tai_enqueue(tai_actor_t *actor, const tai_delivery_t *delivery,
                         uint64_t *out_id);
ta_status_t tai_dequeue(tai_actor_t *actor, uint8_t internal);
const ta_event_t *tai_current_event(const tai_actor_t *actor, ta_event_t *scratch);
void tai_execution_error(ta_context_t *context, ta_status_t status, ta_text_t sendid);
void tai_cancel_timers(tai_actor_t *actor);
uint8_t tai_timer_due(ta_system_t *system);
void tai_timer_dispatch(ta_system_t *system);
ta_status_t tai_timer_schedule(tai_actor_t *actor, const ta_send_t *send, tai_timer_t **out);
tai_timer_t *tai_timer_take(ta_system_t *system);
void tai_timer_dispose(tai_timer_t *timer, ta_status_t status);

struct tai_span {
    ta_system_t *system;
    ta_arena_t *arena;
    ta_span_t value;
    ta_status_t status;
    uint8_t started;
};
struct tai_io {
    ta_system_t *system;
    ta_arena_t *arena;
    ta_io_request_t request;
    tai_io_t *left, *right, *parent;
    tai_io_t *out_next, *out_previous;
    tai_span_t *span;
    tai_invocation_t *invocation;
    size_t capacity;
    uint8_t taken, exposed;
};
struct tai_invocation {
    ta_system_t *system;
    ta_arena_t *arena;
    ta_invocation_ref_t ref;
    ta_actor_ref_t actor, child;
    tai_invocation_t *left, *right, *parent;
    tai_invocation_t *next;
    const ta_invoke_desc_t *description;
    const ta_service_desc_t *service;
    ta_text_t id, source;
    uint32_t owner;
    tai_io_t *start, *cancel;
    size_t capacity;
    uint8_t started, cancelled, done, external;
};

struct tai_timer {
    ta_arena_t *arena;
    tai_actor_t *actor;
    tai_timer_t *left, *right, *parent;
    tai_timer_t *next, *previous;
    ta_send_t send;
    const ta_trace_context_t *trace;
    tai_span_t *span;
    uint64_t accepted_ns, deadline, sequence;
};
ta_status_t tai_io_open(tai_actor_t *actor, const ta_io_request_t *request, tai_io_t **out);
void tai_io_expose(tai_io_t *request);
void tai_io_discard(tai_io_t *request);
void tai_invocation_completed(tai_io_t *request, ta_status_t result);
void tai_span_begin(ta_system_t *system, const ta_span_start_t *start, tai_span_t **out);
void tai_span_finish(tai_span_t *span, ta_status_t status);
void tai_span_error(tai_span_t *span, const ta_diagnostic_t *error);

size_t tai_arena_control_bytes(void);
tai_actor_t *tai_actor_find(const ta_system_t *system, ta_actor_ref_t ref);
void tai_actor_insert(ta_system_t *system, tai_actor_t *actor);
void tai_actor_remove(ta_system_t *system, tai_actor_t *actor);
void tai_ready(tai_actor_t *actor);
void tai_unready(tai_actor_t *actor);
uint8_t tai_accepting(const tai_actor_t *actor);
ta_status_t tai_extra(tai_actor_t *actor);
ta_text_t tai_session(tai_actor_t *actor, uint8_t location);
ta_status_t tai_actor_scope(tai_actor_t *actor, ta_lifetime_t lifetime, ta_arena_t **out);
ta_status_t tai_spawn_actor(ta_system_t *system, const ta_spawn_options_t *options, ta_actor_ref_t *out);
void tai_release_child(tai_actor_t *actor);
void tai_actor_info(tai_actor_t *actor, ta_actor_info_t *out);
void tai_context(tai_actor_t *actor, ta_phase_t phase, ta_context_t *out);
uint8_t tai_active(const tai_actor_t *actor, uint32_t state);
ta_status_t tai_set_active(tai_actor_t *actor, uint32_t state, uint8_t active);
void tai_activation_close(tai_actor_t *actor, uint32_t state);
void tai_actor_fault(tai_actor_t *actor, ta_status_t reason);
void tai_actor_terminal(tai_actor_t *actor, ta_actor_status_t status, ta_status_t reason);

/* Executable-block runner returns one boundary error; the interpreter queues
 * it once and continues other blocks. TA_FAULTED is a hard action ceiling. */
ta_status_t tai_execute(ta_context_t *context, const ta_block_t *block);
ta_status_t tai_text_eval(ta_context_t *context, const ta_text_expr_t *expression,
                          ta_text_t *out);
ta_status_t tai_body_eval(ta_context_t *context, const ta_body_expr_t *expression, ta_body_t *out);
ta_status_t tai_body_payload(ta_context_t *context, const ta_body_t *body, ta_payload_t *out);
void tai_invoke_cancel_state(tai_actor_t *actor, uint32_t state);
ta_status_t tai_invoke_pending(tai_actor_t *actor, uint8_t *worked);
void tai_invoke_event(tai_actor_t *actor);
void tai_invoke_child_terminal(tai_actor_t *actor, const ta_body_t *done);
ta_status_t tai_interpret(tai_actor_t *actor, ta_run_result_t *result);

typedef struct tai_array_layout {
    size_t count;
    size_t stride;
    size_t alignment;
} tai_array_layout_t;

/* Copy destinations bind host value roots to an arena. A failed copy exposes
 * no output; closing the destination also releases roots from partial copies.
 * Callers stage fallible admission in an independently closeable arena. */
typedef struct tai_value_scope {
    ta_system_t *system;
    ta_arena_t *arena;
} tai_value_scope_t;
ta_status_t tai_value_snapshot(const tai_value_scope_t *scope, ta_value_t source,
                              ta_value_t *out);
ta_status_t tai_payload_snapshot(const tai_value_scope_t *scope,
                                const ta_payload_t *source, ta_payload_t *out);
ta_status_t tai_body_snapshot(const tai_value_scope_t *scope,
                             const ta_body_t *source, ta_body_t *out);
ta_status_t tai_trace_copy(const tai_value_scope_t *scope,
                          const ta_trace_context_t *source,
                          const ta_trace_context_t **out);

/* These helpers copy bytes only; typed structs must be initialized as C values,
 * not assumed valid from zero bytes (pointer/floating representations differ). */
void tai_copy(void *destination, const void *source, size_t size);
void tai_zero(void *destination, size_t size);
ta_status_t tai_alloc(ta_arena_t *arena, size_t size, void **out);
ta_status_t tai_array(ta_arena_t *arena, const tai_array_layout_t *layout, void **out);
ta_status_t tai_text_copy(ta_arena_t *arena, ta_text_t text, ta_text_t *out);
ta_status_t tai_cstr_copy(ta_arena_t *arena, const char *text, const char **out);
int tai_text_compare(ta_text_t left, ta_text_t right);
uint8_t tai_text_valid(ta_text_t text, uint8_t allow_empty);
uint8_t tai_event_matches(ta_text_t descriptor, ta_text_t event);
uint64_t tai_hash(ta_text_t text);
void tai_mutated(ta_system_t *system);
void tai_saturate(uint64_t *counter);
ta_status_t tai_scope(ta_system_t *system, ta_lifetime_t lifetime, ta_arena_t **out);
const ta_ioprocessor_desc_t *tai_processor(const ta_system_t *system, ta_text_t type);
const ta_service_desc_t *tai_service(const ta_system_t *system, ta_text_t type);
ta_status_t tai_definition_validate(ta_definition_t *candidate, ta_diagnostic_t *error);
tai_registry_t *tai_registry_find(tai_registry_t *root, ta_text_t key);
tai_registry_t *tai_registry_first(tai_registry_t *root);
tai_registry_t *tai_registry_next(tai_registry_t *entry);
void tai_registry_insert(tai_registry_t **root, tai_registry_t *entry);
void tai_registry_remove(tai_registry_t **root, tai_registry_t *entry);

/* Definition agent owns prepare/build/references/description/revision and
 * topology. Root owns registry and footprint, which need actor internals.
 * On prepare success: link system.definitions; ++stats.definitions; allocate a
 * revision from next_revision (initially 1; never issue 0 or UINT64_MAX).
 * Retain code_owner/inline definitions only once the accepted graph is owned;
 * unwind temporary retains on failure. Free removes list entry and decrements
 * stats.definitions before closing the definition arena. */

#endif

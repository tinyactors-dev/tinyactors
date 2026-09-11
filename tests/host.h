#ifndef TINYACTORS_TEST_HOST_H_INCLUDED
#define TINYACTORS_TEST_HOST_H_INCLUDED

#include "../src/tinyactors.h"
#include <stddef.h>

/* Hosted-only support.  None of these declarations are part of the runtime. */
enum ta_test_trace_kind {
    TA_TEST_TRACE_EVENT = 1,
    TA_TEST_TRACE_ACTION,
    TA_TEST_TRACE_TERMINAL
};

typedef struct ta_test_release {
    ta_chunk_t chunk;
    int exact;
} ta_test_release_t;

typedef struct ta_test_allocator ta_test_allocator_t;

struct ta_test_allocator {
    size_t round_bytes;
    size_t fail_acquire;
    size_t acquire_count;
    size_t live_bytes;
    size_t peak_bytes;
    size_t live_blocks;
    size_t peak_blocks;
    size_t failed_acquires;
    size_t bad_releases;
    void *active;
    ta_test_release_t *releases;
    size_t release_count;
    size_t release_capacity;
};

void ta_test_allocator_init(ta_test_allocator_t *allocator, size_t round_bytes,
                            size_t fail_acquire);
void ta_test_allocator_reset(ta_test_allocator_t *allocator);
ta_chunk_allocator_t ta_test_allocator_callbacks(ta_test_allocator_t *allocator);
int ta_test_allocator_releases_ok(const ta_test_allocator_t *allocator);

typedef struct ta_test_trace_entry {
    enum ta_test_trace_kind kind;
    ta_actor_ref_t actor;
    ta_state_id_t state;
    ta_transition_id_t transition;
    ta_action_kind_t action;
    ta_actor_status_t actor_status;
    ta_status_t status;
    ta_code_t code;
    ta_text_t text;
    char *owned_text;
} ta_test_trace_entry_t;

typedef struct ta_test_trace {
    ta_test_trace_entry_t *entries;
    size_t count;
    size_t capacity;
    size_t dropped;
} ta_test_trace_t;

typedef struct ta_test_action {
    ta_actor_ref_t actor;
    ta_state_id_t state;
    ta_transition_id_t transition;
    ta_action_kind_t kind;
    ta_code_t code;
} ta_test_action_t;

void ta_test_trace_init(ta_test_trace_t *trace);
void ta_test_trace_reset(ta_test_trace_t *trace);
int ta_test_trace_event(ta_test_trace_t *trace, ta_actor_ref_t actor,
                        const ta_event_t *event);
int ta_test_trace_action(ta_test_trace_t *trace, const ta_test_action_t *action);
int ta_test_trace_terminal(ta_test_trace_t *trace,
                           const ta_actor_termination_t *termination);

#endif

/* GENERATED FILE. Module order is defined by include/tinyactors.h. */
#include "tinyactors.h"






















/*
 * Private structural image. Every pointer is image-owned. Values embedded in
 * events, timers, continuation, and completion are retained, never traversed.
 */
struct ta_session_image {
    ta_allocator allocator;
    ta_actor_id source_actor;
    uint64_t source_epoch;
    ta_actor_ref self;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;
    void *configuration_storage;
    uint32_t configuration_bytes;
    void *history_storage;
    uint32_t history_bytes;
    ta_continuation continuation;
    ta_queued_event *internal_events;
    uint32_t internal_count;
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    ta_delayed_send *delayed_sends;
    uint32_t delayed_send_count;
    bool has_parent;
    ta_actor_parent parent;
    ta_actor_completion completion;
};




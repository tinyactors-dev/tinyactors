/* region_layout.c — the description of a segment's region: every type a snapshot
 * of it holds (ta_system and what it embeds, and the records of its pools
 * and arrays), each member's offset, size and type as this build laid them
 * out, written as CBOR. Its FNV-1a 64 is the layout id: ta_system_init
 * writes it into the region's header, and ta_system_restore refuses a
 * snapshot whose id differs, so a snapshot is read only by a build that
 * lays the region out the same way. `mise run layout` checks the table
 * against the debug information: every member of every type is here, at
 * the offset and size the compiler gave it.
 *
 * The numbers are the compiler's (offsetof, sizeof), never typed by hand. A
 * member's `type` is a scalar (u8 … i64), a described type, "bytes" or
 * "pointer"; its `size` is the whole member's, its `count` the elements of
 * an array (1 otherwise). Its `kind`, when it has one: "relative" (an
 * offset from the start of the struct that holds it), "index:<pool>" (a
 * slot of that pool, TA_NIL for none), "handle:<pool>" (the slot + 1 in the
 * low 24 bits and its generation in the high 8, 0 for none), "pointer" (a
 * host pointer, zero in a snapshot), "string" (a string reference, which
 * the description's "strings" says how to read: a slab string of the
 * region or a string of the segment's image), "event-name" (an index of
 * the image's event names), "slab" (an offset in the slab's pages: an
 * actor's instance) or "global:<section>" (an index of an image section).
 * The image's records add "chart:<section>" (an index local to the chart's
 * records) and "u32-pool" (an offset into the image's u32s, with its
 * `list`). After the region come "image", "instance" and "trace": how a
 * reader finds every chart of the image, an actor's configuration and the
 * trace records the journal keeps. A member whose kind holds only for some records
 * has `when`: {"member": the record's member, "values": the values of it
 * for which the kind holds}. An anonymous union is described by its
 * members, each at the union's offset. */
#include <stddef.h>

#include <tinyactors/cbor.h>

#include "base/memory.h"
#include "image/records.h"
#include "system/system_internal.h"

/* A kind that holds only when the record's `member` is one of `values`. */
typedef struct layout_condition {
    const char *member;
    const uint32_t *values;
    uint32_t count;
} layout_condition;

/* The u32s a member's offset names in the image's u32 section: `count`
 * entries (a member of the same record), each of the kinds in `elements`
 * (one u32 apiece). */
typedef struct layout_list {
    const char *count;
    const char *const *elements;
    uint32_t width;
} layout_list;

typedef struct layout_member {
    const char *name;
    uint32_t offset, size, count;
    const char *type;
    const char *kind;             /* NULL: none */
    const layout_condition *when; /* NULL: always */
    const layout_list *list;      /* NULL: none */
} layout_member;

typedef struct layout_type {
    const char *name;
    uint32_t size;
    const layout_member *members;
    uint32_t count;
} layout_type;

/* Only the preprocessor can name a member for offsetof and sizeof. */
#define MEMBER(record, member, type, kind)                                                         \
    {#member, offsetof(record, member), sizeof(((record *)0)->member), 1, type, kind, 0, 0}
#define MEMBER_WHEN(record, member, type, kind, when)                                              \
    {#member, offsetof(record, member), sizeof(((record *)0)->member), 1, type, kind, when, 0}
#define MEMBER_LIST(record, member, type, list)                                                    \
    {#member, offsetof(record, member), sizeof(((record *)0)->member), 1, type, "u32-pool", 0, list}
#define ARRAY(record, member, type, kind)                                                          \
    {#member, offsetof(record, member), sizeof(((record *)0)->member),                             \
     sizeof(((record *)0)->member) / sizeof(((record *)0)->member[0]), type, kind, 0, 0}

/* A request's source is a chart index in a pending spawn, its type and next
 * share a word (route.c). */
static const uint32_t request_named_kinds[] = {REQUEST_EXTERNAL_INVOKE, REQUEST_RESOLVE_SOURCE};
static const uint32_t request_external_kinds[] = {REQUEST_EXTERNAL_INVOKE};
static const uint32_t request_spawn_kinds[] = {REQUEST_PENDING_SPAWN};
static const layout_condition request_named = {"kind", request_named_kinds, 2};
static const layout_condition request_external = {"kind", request_external_kinds, 1};
static const layout_condition request_spawn = {"kind", request_spawn_kinds, 1};
/* An actor's instance is a slab offset while it is resident; a hibernated
 * actor's holds its chart (system_internal.h, ta_actor_chart). */
static const uint32_t actor_resident_states[] = {TA_ACTOR_IDLE, TA_ACTOR_RUNNABLE,
                                                 TA_ACTOR_WAIT_WORKSPACE, TA_ACTOR_WAIT_EXTERNAL,
                                                 TA_ACTOR_DONE};
static const uint32_t actor_hibernated_states[] = {TA_ACTOR_HIBERNATED};
static const layout_condition actor_resident = {"state", actor_resident_states, 5};
static const layout_condition actor_hibernated = {"state", actor_hibernated_states, 1};
/* A send's target is a string or an invocation's slot by its target_kind. */
static const uint32_t send_string_targets[] = {TA_SEND_TARGET_STRING};
static const uint32_t send_invoke_targets[] = {TA_SEND_TARGET_INVOKE};
static const layout_condition send_string_target = {"target_kind", send_string_targets, 1};
static const layout_condition send_invoke_target = {"target_kind", send_invoke_targets, 1};

/* The lists a record's offset names in the image's u32 section. */
static const char *const list_event_interval[] = {"event-name", "event-name-end"};
static const char *const list_states[] = {"chart:states"};
static const char *const list_expressions[] = {"chart:expressions"};
static const char *const list_params[] = {"string", "chart:expressions"};
static const layout_list transition_events = {"event_count", list_event_interval, 2};
static const layout_list transition_targets = {"target_count", list_states, 1};
static const layout_list capture_namelist = {"namelist_count", list_expressions, 1};
static const layout_list capture_params = {"param_count", list_params, 2};

static const layout_member system_members[] = {
    MEMBER(ta_system, magic, "u32", 0),
    MEMBER(ta_system, pad, "u32", 0),
    MEMBER(ta_system, layout_id, "u64", 0),
    MEMBER(ta_system, region_size, "u64", 0),
    MEMBER(ta_system, config, "ta_system_config", 0),
    MEMBER(ta_system, core, "ta_core", 0),
    MEMBER(ta_system, actors, "ta_pool", 0),
    MEMBER(ta_system, events, "ta_pool", 0),
    MEMBER(ta_system, timers, "ta_pool", 0),
    MEMBER(ta_system, workspaces, "ta_pool", 0),
    MEMBER(ta_system, requests, "ta_pool", 0),
    MEMBER(ta_system, slab, "ta_slab", 0),
    MEMBER(ta_system, heap, "ta_heap", 0),
    MEMBER(ta_system, run_queue, "ta_index_queue", 0),
    MEMBER(ta_system, workspace_queue, "ta_index_queue", 0),
    MEMBER(ta_system, wake_queue, "ta_index_queue", 0),
    MEMBER(ta_system, pending_spawns, "ta_index_queue", 0),
    MEMBER(ta_system, runnable_count, "u32", 0),
    MEMBER(ta_system, hibernated_count, "u32", 0),
    MEMBER(ta_system, pending_wake_count, "u32", 0),
    MEMBER(ta_system, stats_hibernations, "u64", 0),
    MEMBER(ta_system, stats_wakes, "u64", 0),
    MEMBER(ta_system, effects_relative, "i64", "relative"),
    MEMBER(ta_system, effect_views_relative, "i64", "relative"),
    MEMBER(ta_system, sessions_relative, "i64", "relative"),
    MEMBER(ta_system, chart_datamodels_relative, "i64", "relative"),
    MEMBER(ta_system, chart_contexts_relative, "i64", "relative"),
    MEMBER(ta_system, chart_stats_relative, "i64", "relative"),
    MEMBER(ta_system, actor_links_relative, "i64", "relative"),
    MEMBER(ta_system, names_relative, "i64", "relative"),
    MEMBER(ta_system, name_capacity, "u32", 0),
    MEMBER(ta_system, name_count, "u32", 0),
    MEMBER(ta_system, invocations_relative, "i64", "relative"),
    MEMBER(ta_system, invocation_capacity, "u32", 0),
    MEMBER(ta_system, invocation_count, "u32", 0),
    MEMBER(ta_system, effects_head, "u32", 0),
    MEMBER(ta_system, effects_length, "u32", 0),
    MEMBER(ta_system, effects_capacity, "u32", 0),
    MEMBER(ta_system, session_capacity, "u32", 0),
    MEMBER(ta_system, session_count, "u32", 0),
    MEMBER(ta_system, chart_count, "u32", 0),
    MEMBER(ta_system, datamodel_count, "u32", 0),
    MEMBER(ta_system, in_call, "u32", 0),
    MEMBER(ta_system, done_event, "u32", 0),
    MEMBER(ta_system, serials_exported, "u32", 0),
    MEMBER(ta_system, now, "u64", 0),
    MEMBER(ta_system, wall_ns, "u64", 0),
    MEMBER(ta_system, sequence, "u64", 0),
    MEMBER(ta_system, next_serial, "u64", 0),
    MEMBER(ta_system, serial_high, "u64", 0),
    MEMBER(ta_system, next_invocation, "u64", 0),
    MEMBER(ta_system, stats_delivered, "u64", 0),
    MEMBER(ta_system, stats_fuel, "u64", 0),
    MEMBER(ta_system, stats_turns, "u64", 0),
    MEMBER(ta_system, stats_timers, "u64", 0),
    MEMBER(ta_system, stats_spawned, "u64", 0),
    MEMBER(ta_system, stats_traces_started, "u64", 0),
    MEMBER(ta_system, stats_traces_sampled, "u64", 0),
    MEMBER(ta_system, journal, "ta_journal", 0),
    MEMBER(ta_system, contexts, "ta_pool", 0),
    MEMBER(ta_system, generator, "ta_trace_generator", 0),
    MEMBER(ta_system, trace_threshold, "u64", 0),
    MEMBER(ta_system, policy, "ta_trace_policy", 0),
    MEMBER(ta_system, rules_relative, "i64", "relative"),
    MEMBER(ta_system, chart_points_relative, "i64", "relative"),
    MEMBER(ta_system, rule_count, "u32", 0),
    MEMBER(ta_system, rule_points, "u32", 0),
    MEMBER(ta_system, fuel_clock, "u64", 0),
    ARRAY(ta_system, datamodels, "ta_datamodel_binding", 0),
};
static const layout_member actor_members[] = {
    MEMBER(ta_actor, next, "u32", "index:actors"),
    MEMBER(ta_actor, state, "u8", 0),
    MEMBER(ta_actor, flags, "u8", 0),
    MEMBER(ta_actor, trace_names, "u16", 0),
    MEMBER_WHEN(ta_actor, instance, "u32", "slab", &actor_resident),
    MEMBER_WHEN(ta_actor, instance, "u32", "global:charts", &actor_hibernated),
    MEMBER(ta_actor, mailbox_head, "u32", "index:events"),
    MEMBER(ta_actor, mailbox_tail, "u32", "index:events"),
    MEMBER(ta_actor, mailbox_length, "u32", 0),
    MEMBER(ta_actor, parent, "u32", "handle:actors"),
    MEMBER(ta_actor, parent_slot, "u32", 0),
    MEMBER(ta_actor, timers, "u32", "index:timers"),
    MEMBER(ta_actor, name, "u32", "string"),
    MEMBER(ta_actor, serial, "u64", 0),
};
static const layout_member actor_links_members[] = {
    MEMBER(ta_actor_links, name_prev, "u32", "index:actors"),
    MEMBER(ta_actor_links, name_next, "u32", "index:actors"),
    MEMBER(ta_actor_links, requests, "u32", 0),
    MEMBER(ta_actor_links, wake_prev, "u32", "index:actors"),
    MEMBER(ta_actor_links, hibernation, "u32", 0),
};
static const layout_member event_members[] = {
    MEMBER(ta_event, next, "u32", "index:events"),
    MEMBER(ta_event, refs, "u16", 0),
    MEMBER(ta_event, type, "u8", 0),
    MEMBER(ta_event, datamodel, "u8", 0),
    MEMBER(ta_event, name_id, "u32", "event-name"),
    MEMBER(ta_event, name, "u32", "string"),
    MEMBER(ta_event, sendid, "u32", "string"),
    MEMBER(ta_event, origin, "u32", "handle:actors"),
    MEMBER(ta_event, origin_string, "u32", "string"),
    MEMBER(ta_event, origintype, "u32", "string"),
    MEMBER(ta_event, invokeid, "u32", "string"),
    MEMBER(ta_event, invoke_slot, "u16", 0),
    MEMBER(ta_event, flags, "u16", 0),
    MEMBER(ta_event, raw, "u32", "string"),
    MEMBER(ta_event, trace, "u32", "index:contexts"),
    MEMBER(ta_event, payload, "u64", 0),
};
static const layout_member trace_entry_members[] = {
    MEMBER(ta_trace_entry, trace, "ta_trace_id", 0),
    MEMBER(ta_trace_entry, session, "u64", 0),
    MEMBER(ta_trace_entry, macrostep, "u64", 0),
    MEMBER(ta_trace_entry, microstep, "u32", 0),
    MEMBER(ta_trace_entry, flags, "u32", 0),
    MEMBER(ta_trace_entry, refs, "u32", 0),
    MEMBER(ta_trace_entry, pad, "u32", 0),
};
static const layout_member timer_members[] = {
    MEMBER(ta_timer, owner_next, "u32", "index:timers"),
    MEMBER(ta_timer, owner, "u32", "handle:actors"),
    MEMBER(ta_timer, event, "u32", "index:events"),
    MEMBER(ta_timer, route, "u32", 0),
    MEMBER(ta_timer, arg, "u32", 0),
    MEMBER(ta_timer, target, "u32", "string"),
};
static const layout_member heap_node_members[] = {
    MEMBER(ta_heap_node, due, "u64", 0),
    MEMBER(ta_heap_node, sequence, "u32", 0),
    MEMBER(ta_heap_node, item, "u32", "index:timers"),
};
static const layout_member request_members[] = {
    MEMBER(ta_request, kind, "u32", 0),
    MEMBER(ta_request, actor, "u32", 0),
    MEMBER(ta_request, slot, "u32", 0),
    MEMBER(ta_request, invokeid, "u32", "string"),
    MEMBER_WHEN(ta_request, source, "u32", "string", &request_named),
    MEMBER(ta_request, pad, "u32", 0),
    MEMBER(ta_request, payload, "u64", 0),
    MEMBER_WHEN(ta_request, type, "u32", "string", &request_external),
    MEMBER_WHEN(ta_request, next, "u32", "index:requests", &request_spawn),
    MEMBER(ta_request, ended, "u32", 0),
    MEMBER(ta_request, invocation, "u64", 0),
};
static const layout_member effect_record_members[] = {
    MEMBER(ta_effect_record, kind, "u32", 0),
    MEMBER(ta_effect_record, actor, "u32", 0),
    MEMBER(ta_effect_record, token, "u32", 0),
    MEMBER(ta_effect_record, io_processor, "u32", 0),
    MEMBER(ta_effect_record, flags, "u32", 0),
    MEMBER(ta_effect_record, target, "u32", "string"),
    MEMBER(ta_effect_record, event_name, "u32", "string"),
    MEMBER(ta_effect_record, sendid, "u32", "string"),
    MEMBER(ta_effect_record, auxiliary, "u32", "string"),
    MEMBER(ta_effect_record, event, "u32", "index:events"),
    MEMBER(ta_effect_record, datamodel, "u32", 0),
    MEMBER(ta_effect_record, pad, "u32", 0),
    MEMBER(ta_effect_record, payload, "u64", 0),
    MEMBER(ta_effect_record, delay, "u64", 0),
    MEMBER(ta_effect_record, serial, "u64", 0),
    MEMBER(ta_effect_record, invocation, "u64", 0),
    MEMBER(ta_effect_record, trace, "ta_trace_context", 0),
};
static const layout_member effect_members[] = {
    MEMBER(ta_effect, size, "u32", 0),
    MEMBER(ta_effect, kind, "u32", 0),
    MEMBER(ta_effect, actor, "u32", 0),
    MEMBER(ta_effect, token, "u32", 0),
    MEMBER(ta_effect, io_processor, "u32", 0),
    MEMBER(ta_effect, flags, "u32", 0),
    MEMBER(ta_effect, target, "ta_string_view", 0),
    MEMBER(ta_effect, event, "ta_string_view", 0),
    MEMBER(ta_effect, sendid, "ta_string_view", 0),
    MEMBER(ta_effect, auxiliary, "ta_string_view", 0),
    MEMBER(ta_effect, payload, "u64", 0),
    MEMBER(ta_effect, delay_hint, "u64", 0),
    MEMBER(ta_effect, session, "u64", 0),
    MEMBER(ta_effect, datamodel, "u32", 0),
    MEMBER(ta_effect, pad, "u32", 0),
    MEMBER(ta_effect, trace, "ta_trace_context", 0),
    MEMBER(ta_effect, invocation, "u64", 0),
};
static const layout_member trace_rule_compiled_members[] = {
    MEMBER(ta_trace_rule_compiled, at, "u8", 0),
    MEMBER(ta_trace_rule_compiled, sample, "u8", 0),
    MEMBER(ta_trace_rule_compiled, detail, "u8", 0),
    MEMBER(ta_trace_rule_compiled, name_bit, "u8", 0),
    MEMBER(ta_trace_rule_compiled, from, "u32", 0),
    MEMBER(ta_trace_rule_compiled, chart, "u32", 0),
    MEMBER(ta_trace_rule_compiled, event_first, "u32", 0),
    MEMBER(ta_trace_rule_compiled, event_end, "u32", 0),
    MEMBER(ta_trace_rule_compiled, event_text, "u32", "string"),
    MEMBER(ta_trace_rule_compiled, actor, "u32", "string"),
    MEMBER(ta_trace_rule_compiled, actor_prefix, "u32", 0),
    MEMBER(ta_trace_rule_compiled, per_second, "u32", 0),
    MEMBER(ta_trace_rule_compiled, burst, "u32", 0),
    MEMBER(ta_trace_rule_compiled, session, "u64", 0),
    MEMBER(ta_trace_rule_compiled, threshold, "u64", 0),
    MEMBER(ta_trace_rule_compiled, tokens, "u64", 0),
    MEMBER(ta_trace_rule_compiled, refilled, "u64", 0),
};
static const layout_member chart_stats_members[] = {
    MEMBER(ta_chart_stats, size, "u32", 0),
    MEMBER(ta_chart_stats, chart, "u32", 0),
    MEMBER(ta_chart_stats, name, "ta_string_view", 0),
    MEMBER(ta_chart_stats, actors, "u32", 0),
    MEMBER(ta_chart_stats, hibernated, "u32", 0),
    MEMBER(ta_chart_stats, spawned, "u64", 0),
    MEMBER(ta_chart_stats, ended, "u64", 0),
    MEMBER(ta_chart_stats, turns, "u64", 0),
    MEMBER(ta_chart_stats, macrosteps, "u64", 0),
    MEMBER(ta_chart_stats, fuel, "u64", 0),
    MEMBER(ta_chart_stats, errors_execution, "u64", 0),
    MEMBER(ta_chart_stats, errors_communication, "u64", 0),
    MEMBER(ta_chart_stats, errors_platform, "u64", 0),
    MEMBER(ta_chart_stats, faults, "u64", 0),
};
static const layout_member slab_page_members[] = {
    MEMBER(ta_slab_page, next, "u32", 0),
    MEMBER(ta_slab_page, previous, "u32", 0),
    MEMBER(ta_slab_page, free_slot, "u16", 0),
    MEMBER(ta_slab_page, used, "u16", 0),
    MEMBER(ta_slab_page, carved, "u16", 0),
    MEMBER(ta_slab_page, size_class, "u8", 0),
    MEMBER(ta_slab_page, pad, "u8", 0),
};
static const layout_member system_config_members[] = {
    MEMBER(ta_system_config, size, "u32", 0),
    MEMBER(ta_system_config, max_actors, "u32", 0),
    MEMBER(ta_system_config, max_events, "u32", 0),
    MEMBER(ta_system_config, max_timers, "u32", 0),
    MEMBER(ta_system_config, workspaces, "u32", 0),
    MEMBER(ta_system_config, max_requests, "u32", 0),
    MEMBER(ta_system_config, outbox, "u32", 0),
    MEMBER(ta_system_config, quantum, "u32", 0),
    MEMBER(ta_system_config, instance_bytes, "u32", 0),
    MEMBER(ta_system_config, string_bytes, "u32", 0),
    MEMBER(ta_system_config, io_processor_count, "u32", 0),
    MEMBER(ta_system_config, flags, "u32", 0),
    MEMBER(ta_system_config, ticks_per_second, "u64", 0),
    MEMBER(ta_system_config, io_processors, "pointer", "pointer"),
    MEMBER(ta_system_config, first_serial, "u64", 0),
    MEMBER(ta_system_config, tracing, "u32", 0),
    MEMBER(ta_system_config, trace_bytes, "u32", 0),
    MEMBER(ta_system_config, trace_contexts, "u32", 0),
    MEMBER(ta_system_config, trace_rules, "u32", 0),
    MEMBER(ta_system_config, trace_policy, "pointer", "pointer"),
    MEMBER(ta_system_config, trace_seed, "u64", 0),
    MEMBER(ta_system_config, identity, "u32", 0),
    MEMBER(ta_system_config, identity_pad, "u32", 0),
    MEMBER(ta_system_config, base, "ta_string_view", 0),
    MEMBER(ta_system_config, last_serial, "u64", 0),
};
static const layout_member core_members[] = {
    MEMBER(ta_core, image_view, "ta_image_view", 0),
    MEMBER(ta_core, image, "pointer", "pointer"),
    MEMBER(ta_core, events, "pointer", "pointer"),
    MEMBER(ta_core, timers, "pointer", "pointer"),
    MEMBER(ta_core, requests, "pointer", "pointer"),
    MEMBER(ta_core, workspaces, "pointer", "pointer"),
    MEMBER(ta_core, slab, "pointer", "pointer"),
    MEMBER(ta_core, datamodels, "pointer", "pointer"),
    MEMBER(ta_core, datamodel_count, "u32", 0),
    MEMBER(ta_core, reserve, "u32", 0),
    MEMBER(ta_core, chart_datamodel, "pointer", "pointer"),
    MEMBER(ta_core, chart_context, "pointer", "pointer"),
    MEMBER(ta_core, chart_stats, "pointer", "pointer"),
    MEMBER(ta_core, io_processors, "pointer", "pointer"),
    MEMBER(ta_core, base, "ta_string_view", 0),
    MEMBER(ta_core, io_processor_count, "u32", 0),
    MEMBER(ta_core, dropped_errors, "u32", 0),
    MEMBER(ta_core, overrun_max, "u32", 0),
    MEMBER(ta_core, overrun, "u32", 0),
    MEMBER(ta_core, serial_of, "pointer", "pointer"),
    MEMBER(ta_core, name_of, "pointer", "pointer"),
    MEMBER(ta_core, done_event, "pointer", "pointer"),
    MEMBER(ta_core, system, "pointer", "pointer"),
    MEMBER(ta_core, journal, "pointer", "pointer"),
    MEMBER(ta_core, contexts, "pointer", "pointer"),
    MEMBER(ta_core, trace_kinds, "u32", 0),
    ARRAY(ta_core, class_kinds, "u32", 0),
    MEMBER(ta_core, trace_step_fuel, "u32", 0),
    MEMBER(ta_core, trace_clock, "u64", 0),
    MEMBER(ta_core, trace_now, "u64", 0),
    MEMBER(ta_core, wall_ns, "u64", 0),
    MEMBER(ta_core, ticks_per_second, "u64", 0),
    MEMBER(ta_core, contexts_dropped, "u64", 0),
    MEMBER(ta_core, span_actor, "u32", 0),
    MEMBER(ta_core, span_pad, "u32", 0),
    MEMBER(ta_core, span_session, "u64", 0),
    MEMBER(ta_core, span_macrostep, "u64", 0),
    MEMBER(ta_core, span_prefix, "u64", 0),
    MEMBER(ta_core, span_macro, "u64", 0),
    MEMBER(ta_core, span_cause, "u64", 0),
    MEMBER(ta_core, out, "ta_core_out", 0),
    MEMBER(ta_core, scratch, "bytes", 0),
};
static const layout_member pool_members[] = {
    MEMBER(ta_pool, slots_relative, "i64", "relative"),
    MEMBER(ta_pool, generations_relative, "i64", "relative"),
    MEMBER(ta_pool, stride, "u32", 0),
    MEMBER(ta_pool, capacity, "u32", 0),
    MEMBER(ta_pool, free_head, "u32", 0),
    MEMBER(ta_pool, high_water_mark, "u32", 0),
    MEMBER(ta_pool, used, "u32", 0),
    MEMBER(ta_pool, refused, "u32", 0),
};
static const layout_member slab_members[] = {
    MEMBER(ta_slab, pages_relative, "i64", "relative"),
    MEMBER(ta_slab, metadata_relative, "i64", "relative"),
    MEMBER(ta_slab, page_count, "u32", 0),
    MEMBER(ta_slab, free_head, "u32", 0),
    MEMBER(ta_slab, high_water_mark, "u32", 0),
    MEMBER(ta_slab, used_pages, "u32", 0),
    ARRAY(ta_slab, class_head, "u32", 0),
    MEMBER(ta_slab, refused, "u32", 0),
};
static const layout_member heap_members[] = {
    MEMBER(ta_heap, nodes_relative, "i64", "relative"),
    MEMBER(ta_heap, positions_relative, "i64", "relative"),
    MEMBER(ta_heap, capacity, "u32", 0),
    MEMBER(ta_heap, count, "u32", 0),
};
static const layout_member index_queue_members[] = {
    MEMBER(ta_index_queue, head, "u32", 0),
    MEMBER(ta_index_queue, tail, "u32", 0),
};
static const layout_member journal_members[] = {
    MEMBER(ta_journal, storage_relative, "i64", "relative"),
    MEMBER(ta_journal, capacity, "u32", 0),
    MEMBER(ta_journal, head, "u32", 0),
    MEMBER(ta_journal, read, "u32", 0),
    MEMBER(ta_journal, tail, "u32", 0),
    MEMBER(ta_journal, end, "u32", 0),
    MEMBER(ta_journal, wrapped, "u32", 0),
    MEMBER(ta_journal, reserved, "u32", 0),
    MEMBER(ta_journal, at, "u32", 0),
    MEMBER(ta_journal, used, "u32", 0),
    MEMBER(ta_journal, retained, "u32", 0),
    MEMBER(ta_journal, lost, "u32", 0),
    MEMBER(ta_journal, size_offset, "u32", 0),
    MEMBER(ta_journal, lost_total, "u64", 0),
    MEMBER(ta_journal, written_total, "u64", 0),
};
static const layout_member trace_generator_members[] = {
    ARRAY(ta_trace_generator, state, "u64", 0),
};
static const layout_member trace_policy_members[] = {
    MEMBER(ta_trace_policy, size, "u32", 0),
    MEMBER(ta_trace_policy, sample_parts, "u32", 0),
    MEMBER(ta_trace_policy, sample_whole, "u32", 0),
    ARRAY(ta_trace_policy, kinds, "u32", 0),
    MEMBER(ta_trace_policy, rule_count, "u32", 0),
    MEMBER(ta_trace_policy, rules, "pointer", "pointer"),
};
static const layout_member datamodel_binding_members[] = {
    MEMBER(ta_datamodel_binding, vtable, "pointer", "pointer"),
    MEMBER(ta_datamodel_binding, datamodel, "pointer", "pointer"),
};
static const layout_member trace_id_members[] = {
    MEMBER(ta_trace_id, high, "u64", 0),
    MEMBER(ta_trace_id, low, "u64", 0),
};
static const layout_member trace_context_members[] = {
    MEMBER(ta_trace_context, trace, "ta_trace_id", 0),
    MEMBER(ta_trace_context, parent, "u64", 0),
    MEMBER(ta_trace_context, flags, "u32", 0),
    MEMBER(ta_trace_context, pad, "u32", 0),
};
static const layout_member string_view_members[] = {
    MEMBER(ta_string_view, pointer, "pointer", "pointer"),
    MEMBER(ta_string_view, length, "u32", 0),
};
static const layout_member image_view_members[] = {
    MEMBER(ta_image_view, base, "pointer", "pointer"),
    MEMBER(ta_image_view, strings, "pointer", "pointer"),
    MEMBER(ta_image_view, charts, "pointer", "pointer"),
    MEMBER(ta_image_view, states, "pointer", "pointer"),
    MEMBER(ta_image_view, transitions, "pointer", "pointer"),
    MEMBER(ta_image_view, u32, "pointer", "pointer"),
    MEMBER(ta_image_view, code, "pointer", "pointer"),
    MEMBER(ta_image_view, expressions, "pointer", "pointer"),
    MEMBER(ta_image_view, captures, "pointer", "pointer"),
    MEMBER(ta_image_view, sends, "pointer", "pointer"),
    MEMBER(ta_image_view, invokes, "pointer", "pointer"),
    MEMBER(ta_image_view, data, "pointer", "pointer"),
    MEMBER(ta_image_view, event_nodes, "pointer", "pointer"),
    MEMBER(ta_image_view, actors, "pointer", "pointer"),
    MEMBER(ta_image_view, hash, "u64", 0),
    MEMBER(ta_image_view, size, "u32", 0),
    MEMBER(ta_image_view, strings_size, "u32", 0),
    MEMBER(ta_image_view, chart_count, "u32", 0),
    MEMBER(ta_image_view, state_count, "u32", 0),
    MEMBER(ta_image_view, transition_count, "u32", 0),
    MEMBER(ta_image_view, u32_count, "u32", 0),
    MEMBER(ta_image_view, code_count, "u32", 0),
    MEMBER(ta_image_view, expression_count, "u32", 0),
    MEMBER(ta_image_view, capture_count, "u32", 0),
    MEMBER(ta_image_view, send_count, "u32", 0),
    MEMBER(ta_image_view, invoke_count, "u32", 0),
    MEMBER(ta_image_view, data_count, "u32", 0),
    MEMBER(ta_image_view, event_node_count, "u32", 0),
    MEMBER(ta_image_view, actor_count, "u32", 0),
};
static const layout_member core_out_members[] = {
    MEMBER(ta_core_out, event, "u32", 0),
    MEMBER(ta_core_out, route, "u32", 0),
    MEMBER(ta_core_out, arg, "u32", 0),
    MEMBER(ta_core_out, target, "u32", 0),
    MEMBER(ta_core_out, sendid, "u32", 0),
    MEMBER(ta_core_out, slot, "u32", 0),
    MEMBER(ta_core_out, chart, "u32", 0),
    MEMBER(ta_core_out, source, "u32", 0),
    MEMBER(ta_core_out, type, "u32", 0),
    MEMBER(ta_core_out, invokeid, "u32", 0),
    MEMBER(ta_core_out, autoforward, "u32", 0),
    MEMBER(ta_core_out, child, "u32", 0),
    MEMBER(ta_core_out, label, "u32", 0),
    MEMBER(ta_core_out, text, "ta_string_view", 0),
    MEMBER(ta_core_out, delay_ns, "u64", 0),
    MEMBER(ta_core_out, payload, "u64", 0),
    MEMBER(ta_core_out, final_state, "u32", 0),
    MEMBER(ta_core_out, fault, "u32", 0),
};
static const layout_member shared_string_members[] = {
    MEMBER(ta_shared_string, refs, "u32", 0),
    MEMBER(ta_shared_string, length, "u32", 0),
};
static const layout_member header_record_members[] = {
    MEMBER(ta_header_record, magic, "u32", 0),
    MEMBER(ta_header_record, version, "u32", 0),
    MEMBER(ta_header_record, total_size, "u32", 0),
    MEMBER(ta_header_record, section_count, "u32", 0),
    MEMBER(ta_header_record, hash, "u64", 0),
    MEMBER(ta_header_record, flags, "u32", 0),
    MEMBER(ta_header_record, chart_count, "u32", 0),
    ARRAY(ta_header_record, reserved, "u32", 0),
};
static const layout_member section_record_members[] = {
    MEMBER(ta_section_record, kind, "u32", 0),
    MEMBER(ta_section_record, offset, "u32", 0),
    MEMBER(ta_section_record, size, "u32", 0),
    MEMBER(ta_section_record, count, "u32", 0),
};
static const layout_member event_node_record_members[] = {
    MEMBER(ta_event_node_record, end, "u32", 0),
    MEMBER(ta_event_node_record, parent, "u32", 0),
    MEMBER(ta_event_node_record, token, "u32", 0),
    MEMBER(ta_event_node_record, name, "u32", 0),
};

/* ---- the image's records (image/records.h): a "chart:<section>" member is
 * an index local to its chart's records of that section, "global:<section>"
 * an index of the whole section ---- */
static const layout_member chart_record_members[] = {
    MEMBER(ta_chart_record, name, "u32", "string"),
    MEMBER(ta_chart_record, datamodel, "u32", "string"),
    MEMBER(ta_chart_record, binding, "u32", 0),
    MEMBER(ta_chart_record, flags, "u32", 0),
    MEMBER(ta_chart_record, first_state, "u32", "global:states"),
    MEMBER(ta_chart_record, state_count, "u32", 0),
    MEMBER(ta_chart_record, first_transition, "u32", "global:transitions"),
    MEMBER(ta_chart_record, transition_count, "u32", 0),
    MEMBER(ta_chart_record, first_expression, "u32", "global:expressions"),
    MEMBER(ta_chart_record, expression_count, "u32", 0),
    MEMBER(ta_chart_record, first_send, "u32", "global:sends"),
    MEMBER(ta_chart_record, send_count, "u32", 0),
    MEMBER(ta_chart_record, first_invoke, "u32", "global:invokes"),
    MEMBER(ta_chart_record, invoke_count, "u32", 0),
    MEMBER(ta_chart_record, first_data, "u32", "global:data"),
    MEMBER(ta_chart_record, data_count, "u32", 0),
    MEMBER(ta_chart_record, first_code, "u32", "global:code"),
    MEMBER(ta_chart_record, code_count, "u32", 0),
    MEMBER(ta_chart_record, script, "u32", "global:code"),
    MEMBER(ta_chart_record, eventless_mask, "u32", "u32-pool"),
    MEMBER(ta_chart_record, max_depth, "u32", 0),
    MEMBER(ta_chart_record, max_foreach, "u32", 0),
    MEMBER(ta_chart_record, history_word_count, "u32", 0),
    MEMBER(ta_chart_record, source, "u32", "string"),
};
static const layout_member state_record_members[] = {
    MEMBER(ta_state_record, parent, "u32", "chart:states"),
    MEMBER(ta_state_record, end, "u32", "chart:states"),
    MEMBER(ta_state_record, kind, "u8", 0),
    MEMBER(ta_state_record, depth, "u8", 0),
    MEMBER(ta_state_record, flags, "u16", 0),
    MEMBER(ta_state_record, id, "u32", "string"),
    MEMBER(ta_state_record, first_transition, "u32", "chart:transitions"),
    MEMBER(ta_state_record, transition_count, "u32", 0),
    MEMBER(ta_state_record, initial, "u32", "chart:transitions"),
    MEMBER(ta_state_record, onentry, "u32", "global:code"),
    MEMBER(ta_state_record, onexit, "u32", "global:code"),
    MEMBER(ta_state_record, first_invoke, "u32", "chart:invokes"),
    MEMBER(ta_state_record, invoke_count, "u32", 0),
    MEMBER(ta_state_record, first_data, "u32", "chart:data"),
    MEMBER(ta_state_record, data_count, "u32", 0),
    MEMBER(ta_state_record, donedata, "u32", "global:captures"),
    MEMBER(ta_state_record, done_event, "u32", "event-name"),
    MEMBER(ta_state_record, history_offset, "u32", 0),
};
static const layout_member transition_record_members[] = {
    MEMBER(ta_transition_record, source, "u32", "chart:states"),
    MEMBER(ta_transition_record, domain, "u32", "chart:states"),
    MEMBER(ta_transition_record, condition, "u32", "chart:expressions"),
    MEMBER_LIST(ta_transition_record, events_offset, "u32", &transition_events),
    MEMBER(ta_transition_record, event_count, "u16", 0),
    MEMBER(ta_transition_record, flags, "u16", 0),
    MEMBER_LIST(ta_transition_record, targets_offset, "u32", &transition_targets),
    MEMBER(ta_transition_record, target_count, "u16", 0),
    MEMBER(ta_transition_record, pad, "u16", 0),
    MEMBER(ta_transition_record, action, "u32", "global:code"),
};
static const layout_member expression_record_members[] = {
    MEMBER(ta_expression_record, kind, "u32", 0),
    MEMBER(ta_expression_record, source, "u32", "string"),
    MEMBER(ta_expression_record, line, "u32", 0),
    MEMBER(ta_expression_record, column, "u32", 0),
};
static const layout_member capture_record_members[] = {
    MEMBER_LIST(ta_capture_record, namelist_offset, "u32", &capture_namelist),
    MEMBER(ta_capture_record, namelist_count, "u32", 0),
    MEMBER_LIST(ta_capture_record, params_offset, "u32", &capture_params),
    MEMBER(ta_capture_record, param_count, "u32", 0),
    MEMBER(ta_capture_record, content, "u32", "chart:expressions"),
};
static const layout_member send_record_members[] = {
    MEMBER(ta_send_record, event, "u32", "event-name"),
    MEMBER(ta_send_record, eventexpr, "u32", "chart:expressions"),
    MEMBER(ta_send_record, target_kind, "u32", 0),
    MEMBER_WHEN(ta_send_record, target, "u32", "string", &send_string_target),
    MEMBER_WHEN(ta_send_record, target, "u32", "chart:invokes", &send_invoke_target),
    MEMBER(ta_send_record, targetexpr, "u32", "chart:expressions"),
    MEMBER(ta_send_record, type, "u32", "string"),
    MEMBER(ta_send_record, typeexpr, "u32", "chart:expressions"),
    MEMBER(ta_send_record, id, "u32", "string"),
    MEMBER(ta_send_record, idlocation, "u32", "chart:expressions"),
    MEMBER(ta_send_record, delayexpr, "u32", "chart:expressions"),
    MEMBER(ta_send_record, capture, "u32", "global:captures"),
    MEMBER(ta_send_record, pad, "u32", 0),
    MEMBER(ta_send_record, delay_ns, "u64", 0),
};
static const layout_member invoke_record_members[] = {
    MEMBER(ta_invoke_record, state, "u32", "chart:states"),
    MEMBER(ta_invoke_record, type, "u32", "string"),
    MEMBER(ta_invoke_record, typeexpr, "u32", "chart:expressions"),
    MEMBER(ta_invoke_record, source_chart, "u32", "global:charts"),
    MEMBER(ta_invoke_record, source, "u32", "string"),
    MEMBER(ta_invoke_record, srcexpr, "u32", "chart:expressions"),
    MEMBER(ta_invoke_record, id, "u32", "string"),
    MEMBER(ta_invoke_record, idlocation, "u32", "chart:expressions"),
    MEMBER(ta_invoke_record, capture, "u32", "global:captures"),
    MEMBER(ta_invoke_record, autoforward, "u32", 0),
    MEMBER(ta_invoke_record, finalize, "u32", "global:code"),
    MEMBER(ta_invoke_record, docexpr, "u32", "chart:expressions"),
};
static const layout_member data_record_members[] = {
    MEMBER(ta_data_record, id, "u32", "string"),
    MEMBER(ta_data_record, expression, "u32", "chart:expressions"),
    MEMBER(ta_data_record, state, "u32", "chart:states"),
};
static const layout_member actor_record_members[] = {
    MEMBER(ta_actor_record, name, "u32", "string"),
    MEMBER(ta_actor_record, chart, "u32", "global:charts"),
};
/* An actor's instance in the slab: this header, then the areas of
 * ta_instance_layout_of (the description's "instance"). */
static const layout_member instance_members[] = {
    MEMBER(ta_instance, chart, "u32", "global:charts"),
    MEMBER(ta_instance, phase, "u8", 0),
    MEMBER(ta_instance, flags, "u8", 0),
    MEMBER(ta_instance, subphase, "u16", 0),
    MEMBER(ta_instance, cursor, "u32", 0),
    MEMBER(ta_instance, cursor2, "u32", 0),
    MEMBER(ta_instance, current_event, "u32", "index:events"),
    MEMBER(ta_instance, internal_queue_head, "u32", "index:events"),
    MEMBER(ta_instance, internal_queue_tail, "u32", "index:events"),
    MEMBER(ta_instance, workspace, "u32", "index:workspaces"),
    MEMBER(ta_instance, actor, "u32", "handle:actors"),
    MEMBER(ta_instance, ids, "u32", 0),
    MEMBER(ta_instance, datamodel, "u64", 0),
    MEMBER(ta_instance, macrostep, "u64", 0),
    MEMBER(ta_instance, microstep, "u32", 0),
    MEMBER(ta_instance, trace, "u32", "index:contexts"),
};

/* ---- the trace records in the journal (<tinyactors/trace.h>): a string is
 * a string reference, as the description's "strings" reads it ---- */
static const layout_member trace_record_members[] = {
    MEMBER(ta_trace_record, kind, "u16", 0),
    MEMBER(ta_trace_record, flags, "u16", 0),
    MEMBER(ta_trace_record, size, "u32", 0),
    MEMBER(ta_trace_record, actor, "u32", "handle:actors"),
    MEMBER(ta_trace_record, chart, "u32", "global:charts"),
    MEMBER(ta_trace_record, key, "ta_step_key", 0),
    MEMBER(ta_trace_record, trace, "ta_trace_id", 0),
    MEMBER(ta_trace_record, span, "u64", 0),
    MEMBER(ta_trace_record, parent, "u64", 0),
    MEMBER(ta_trace_record, fuel, "u64", 0),
    MEMBER(ta_trace_record, now, "u64", 0),
};
static const layout_member step_key_members[] = {
    MEMBER(ta_step_key, session, "u64", 0),
    MEMBER(ta_step_key, macrostep, "u64", 0),
    MEMBER(ta_step_key, microstep, "u32", 0),
    MEMBER(ta_step_key, pad, "u32", 0),
};
static const layout_member trace_dropped_members[] = {
    MEMBER(ta_trace_dropped, count, "u64", 0),
};
static const layout_member trace_spawned_members[] = {
    MEMBER(ta_trace_spawned, name, "u32", "string"),
    MEMBER(ta_trace_spawned, parent, "u32", "handle:actors"),
};
static const layout_member trace_finished_members[] = {
    MEMBER(ta_trace_finished, final_state, "u32", "chart:states"),
    MEMBER(ta_trace_finished, name, "u32", "string"),
};
static const layout_member trace_lifecycle_members[] = {
    MEMBER(ta_trace_lifecycle, name, "u32", "string"),
    MEMBER(ta_trace_lifecycle, pad, "u32", 0),
};
static const layout_member trace_macrostep_begin_members[] = {
    MEMBER(ta_trace_macrostep_begin, name, "u32", "string"),
    MEMBER(ta_trace_macrostep_begin, name_id, "u32", "event-name"),
    MEMBER(ta_trace_macrostep_begin, type, "u32", 0),
    MEMBER(ta_trace_macrostep_begin, sendid, "u32", "string"),
    MEMBER(ta_trace_macrostep_begin, origin, "u32", "string"),
    MEMBER(ta_trace_macrostep_begin, datamodel, "u32", 0),
    MEMBER(ta_trace_macrostep_begin, origin_session, "u64", 0),
    MEMBER(ta_trace_macrostep_begin, cause, "ta_step_key", 0),
    MEMBER(ta_trace_macrostep_begin, payload, "u64", 0),
    MEMBER(ta_trace_macrostep_begin, actor_name, "u32", "string"),
    MEMBER(ta_trace_macrostep_begin, pad, "u32", 0),
};
static const layout_member trace_macrostep_end_members[] = {
    MEMBER(ta_trace_macrostep_end, state_count, "u32", 0),
    MEMBER(ta_trace_macrostep_end, pad, "u32", 0),
};
static const layout_member trace_microstep_members[] = {
    MEMBER(ta_trace_microstep, begin_fuel, "u64", 0),
    MEMBER(ta_trace_microstep, transition_count, "u32", 0),
    MEMBER(ta_trace_microstep, exited_count, "u32", 0),
    MEMBER(ta_trace_microstep, entered_count, "u32", 0),
    MEMBER(ta_trace_microstep, name, "u32", "string"),
};
static const layout_member trace_condition_members[] = {
    MEMBER(ta_trace_condition, transition, "u32", "chart:transitions"),
    MEMBER(ta_trace_condition, result, "u32", 0),
};
static const layout_member trace_raised_members[] = {
    MEMBER(ta_trace_raised, name, "u32", "string"),
    MEMBER(ta_trace_raised, name_id, "u32", "event-name"),
};
static const layout_member trace_sent_members[] = {
    MEMBER(ta_trace_sent, target, "u32", "string"),
    MEMBER(ta_trace_sent, name, "u32", "string"),
    MEMBER(ta_trace_sent, name_id, "u32", "event-name"),
    MEMBER(ta_trace_sent, sendid, "u32", "string"),
    MEMBER(ta_trace_sent, delay, "u64", 0),
    MEMBER(ta_trace_sent, context, "ta_trace_context", 0),
};
static const layout_member trace_invoked_members[] = {
    MEMBER(ta_trace_invoked, type, "u32", "string"),
    MEMBER(ta_trace_invoked, source, "u32", "string"),
    MEMBER(ta_trace_invoked, invokeid, "u32", "string"),
    MEMBER(ta_trace_invoked, child, "u32", "handle:actors"),
    MEMBER(ta_trace_invoked, child_session, "u64", 0),
};
static const layout_member trace_cancelled_members[] = {
    MEMBER(ta_trace_cancelled, sendid, "u32", "string"),
    MEMBER(ta_trace_cancelled, pad, "u32", 0),
};
static const layout_member trace_assign_members[] = {
    MEMBER(ta_trace_assign, location, "u32", "chart:expressions"),
    MEMBER(ta_trace_assign, datamodel, "u32", 0),
    MEMBER(ta_trace_assign, payload, "u64", 0),
};
static const layout_member trace_log_members[] = {
    MEMBER(ta_trace_log, label, "u32", "string"),
    MEMBER(ta_trace_log, text, "u32", "string"),
};
static const layout_member trace_action_failed_members[] = {
    MEMBER(ta_trace_action_failed, opcode, "u32", 0),
    MEMBER(ta_trace_action_failed, expression, "u32", "chart:expressions"),
};

#define TYPE(name, members) {#name, sizeof(name), members, sizeof members / sizeof members[0]}
static const layout_type types[] = {
    {"ta_system", sizeof(ta_system), system_members, sizeof system_members / sizeof system_members[0]},
    {"ta_actor", sizeof(ta_actor), actor_members, sizeof actor_members / sizeof actor_members[0]},
    {"ta_actor_links", sizeof(ta_actor_links), actor_links_members, sizeof actor_links_members / sizeof actor_links_members[0]},
    {"ta_event", sizeof(ta_event), event_members, sizeof event_members / sizeof event_members[0]},
    {"ta_trace_entry", sizeof(ta_trace_entry), trace_entry_members, sizeof trace_entry_members / sizeof trace_entry_members[0]},
    {"ta_timer", sizeof(ta_timer), timer_members, sizeof timer_members / sizeof timer_members[0]},
    {"ta_heap_node", sizeof(ta_heap_node), heap_node_members, sizeof heap_node_members / sizeof heap_node_members[0]},
    {"ta_request", sizeof(ta_request), request_members, sizeof request_members / sizeof request_members[0]},
    {"ta_effect_record", sizeof(ta_effect_record), effect_record_members, sizeof effect_record_members / sizeof effect_record_members[0]},
    {"ta_effect", sizeof(ta_effect), effect_members, sizeof effect_members / sizeof effect_members[0]},
    {"ta_trace_rule_compiled", sizeof(ta_trace_rule_compiled), trace_rule_compiled_members, sizeof trace_rule_compiled_members / sizeof trace_rule_compiled_members[0]},
    {"ta_chart_stats", sizeof(ta_chart_stats), chart_stats_members, sizeof chart_stats_members / sizeof chart_stats_members[0]},
    {"ta_slab_page", sizeof(ta_slab_page), slab_page_members, sizeof slab_page_members / sizeof slab_page_members[0]},
    {"ta_system_config", sizeof(ta_system_config), system_config_members, sizeof system_config_members / sizeof system_config_members[0]},
    {"ta_core", sizeof(ta_core), core_members, sizeof core_members / sizeof core_members[0]},
    {"ta_pool", sizeof(ta_pool), pool_members, sizeof pool_members / sizeof pool_members[0]},
    {"ta_slab", sizeof(ta_slab), slab_members, sizeof slab_members / sizeof slab_members[0]},
    {"ta_heap", sizeof(ta_heap), heap_members, sizeof heap_members / sizeof heap_members[0]},
    {"ta_index_queue", sizeof(ta_index_queue), index_queue_members, sizeof index_queue_members / sizeof index_queue_members[0]},
    {"ta_journal", sizeof(ta_journal), journal_members, sizeof journal_members / sizeof journal_members[0]},
    {"ta_trace_generator", sizeof(ta_trace_generator), trace_generator_members, sizeof trace_generator_members / sizeof trace_generator_members[0]},
    {"ta_trace_policy", sizeof(ta_trace_policy), trace_policy_members, sizeof trace_policy_members / sizeof trace_policy_members[0]},
    {"ta_datamodel_binding", sizeof(ta_datamodel_binding), datamodel_binding_members, sizeof datamodel_binding_members / sizeof datamodel_binding_members[0]},
    {"ta_trace_id", sizeof(ta_trace_id), trace_id_members, sizeof trace_id_members / sizeof trace_id_members[0]},
    {"ta_trace_context", sizeof(ta_trace_context), trace_context_members, sizeof trace_context_members / sizeof trace_context_members[0]},
    {"ta_string_view", sizeof(ta_string_view), string_view_members, sizeof string_view_members / sizeof string_view_members[0]},
    {"ta_image_view", sizeof(ta_image_view), image_view_members, sizeof image_view_members / sizeof image_view_members[0]},
    {"ta_core_out", sizeof(ta_core_out), core_out_members, sizeof core_out_members / sizeof core_out_members[0]},
    {"ta_shared_string", sizeof(ta_shared_string), shared_string_members, sizeof shared_string_members / sizeof shared_string_members[0]},
    {"ta_header_record", sizeof(ta_header_record), header_record_members, sizeof header_record_members / sizeof header_record_members[0]},
    {"ta_section_record", sizeof(ta_section_record), section_record_members, sizeof section_record_members / sizeof section_record_members[0]},
    {"ta_event_node_record", sizeof(ta_event_node_record), event_node_record_members, sizeof event_node_record_members / sizeof event_node_record_members[0]},
    TYPE(ta_chart_record, chart_record_members),
    TYPE(ta_state_record, state_record_members),
    TYPE(ta_transition_record, transition_record_members),
    TYPE(ta_expression_record, expression_record_members),
    TYPE(ta_capture_record, capture_record_members),
    TYPE(ta_send_record, send_record_members),
    TYPE(ta_invoke_record, invoke_record_members),
    TYPE(ta_data_record, data_record_members),
    TYPE(ta_actor_record, actor_record_members),
    TYPE(ta_instance, instance_members),
    TYPE(ta_trace_record, trace_record_members),
    TYPE(ta_step_key, step_key_members),
    TYPE(ta_trace_dropped, trace_dropped_members),
    TYPE(ta_trace_spawned, trace_spawned_members),
    TYPE(ta_trace_finished, trace_finished_members),
    TYPE(ta_trace_lifecycle, trace_lifecycle_members),
    TYPE(ta_trace_macrostep_begin, trace_macrostep_begin_members),
    TYPE(ta_trace_macrostep_end, trace_macrostep_end_members),
    TYPE(ta_trace_microstep, trace_microstep_members),
    TYPE(ta_trace_condition, trace_condition_members),
    TYPE(ta_trace_raised, trace_raised_members),
    TYPE(ta_trace_sent, trace_sent_members),
    TYPE(ta_trace_invoked, trace_invoked_members),
    TYPE(ta_trace_cancelled, trace_cancelled_members),
    TYPE(ta_trace_assign, trace_assign_members),
    TYPE(ta_trace_log, trace_log_members),
    TYPE(ta_trace_action_failed, trace_action_failed_members),
};

#undef MEMBER
#undef MEMBER_WHEN
#undef MEMBER_LIST
#undef ARRAY
#undef TYPE

/* Each pool of ta_system (its member) and the type of its slots. */
typedef struct layout_pool {
    const char *name, *element;
} layout_pool;
static const layout_pool pools[] = {
    {"actors", "ta_actor"}, {"events", "ta_event"},     {"timers", "ta_timer"},
    {"workspaces", "bytes"}, {"requests", "ta_request"}, {"contexts", "ta_trace_entry"},
};

/* Each array a *_relative member of ta_system points to: its elements' type
 * and the member of ta_system (a dotted path) that holds their count. */
typedef struct layout_array {
    const char *name, *offset, *element, *count;
} layout_array;
static const layout_array arrays[] = {
    {"effects", "effects_relative", "ta_effect_record", "effects_capacity"},
    {"effect_views", "effect_views_relative", "ta_effect", "effects_capacity"},
    {"sessions", "sessions_relative", "u32", "session_capacity"},
    {"chart_datamodels", "chart_datamodels_relative", "u8", "chart_count"},
    {"chart_contexts", "chart_contexts_relative", "pointer", "chart_count"},
    {"chart_stats", "chart_stats_relative", "ta_chart_stats", "chart_count"},
    {"actor_links", "actor_links_relative", "ta_actor_links", "actors.capacity"},
    {"names", "names_relative", "u32", "name_capacity"},
    {"invocations", "invocations_relative", "u32", "invocation_capacity"},
    {"rules", "rules_relative", "ta_trace_rule_compiled", "config.trace_rules"},
    /* chart_count of them when config.trace_rules is not 0, else none */
    {"chart_points", "chart_points_relative", "u8", "chart_count"},
};

static ta_string_view text_of(const char *text) {
    uint32_t length = 0;
    while (text[length])
        length++;
    return (ta_string_view){text, length};
}

static void write_pair(ta_cbor_writer *writer, const char *key, const char *value) {
    ta_cbor_write_text(writer, text_of(key));
    ta_cbor_write_text(writer, text_of(value));
}

static void write_number(ta_cbor_writer *writer, const char *key, uint64_t value) {
    ta_cbor_write_text(writer, text_of(key));
    ta_cbor_write_uint(writer, value);
}

/* "major.minor.patch" into `text` (room for three u32s and two dots). */
static ta_string_view library_text(char text[40]) {
    ta_version version = ta_library_version();
    const uint32_t parts[3] = {version.major, version.minor, version.patch};
    uint32_t length = 0;
    for (uint32_t i = 0; i < 3; i++) {
        char digits[10];
        uint32_t count = 0, value = parts[i];
        do {
            digits[count++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value);
        while (count)
            text[length++] = digits[--count];
        if (i < 2) text[length++] = '.';
    }
    return (ta_string_view){text, length};
}

static void write_type(ta_cbor_writer *writer, const layout_type *type) {
    ta_cbor_write_map(writer, 3);
    write_pair(writer, "name", type->name);
    write_number(writer, "size", type->size);
    ta_cbor_write_text(writer, text_of("members"));
    ta_cbor_write_array(writer, type->count);
    for (uint32_t i = 0; i < type->count; i++) {
        const layout_member *member = &type->members[i];
        ta_cbor_write_map(writer, 5u + (member->kind ? 1u : 0u) + (member->when ? 1u : 0u) +
                                      (member->list ? 1u : 0u));
        write_pair(writer, "name", member->name);
        write_number(writer, "offset", member->offset);
        write_number(writer, "size", member->size);
        write_number(writer, "count", member->count);
        write_pair(writer, "type", member->type);
        if (member->kind) write_pair(writer, "kind", member->kind);
        if (member->when) {
            ta_cbor_write_text(writer, text_of("when"));
            ta_cbor_write_map(writer, 2);
            write_pair(writer, "member", member->when->member);
            ta_cbor_write_text(writer, text_of("values"));
            ta_cbor_write_array(writer, member->when->count);
            for (uint32_t k = 0; k < member->when->count; k++)
                ta_cbor_write_uint(writer, member->when->values[k]);
        }
        if (member->list) {
            ta_cbor_write_text(writer, text_of("list"));
            ta_cbor_write_map(writer, 2);
            write_pair(writer, "count", member->list->count);
            ta_cbor_write_text(writer, text_of("elements"));
            ta_cbor_write_array(writer, member->list->width);
            for (uint32_t k = 0; k < member->list->width; k++)
                ta_cbor_write_text(writer, text_of(member->list->elements[k]));
        }
    }
}

/* How a "string" or "event-name" member reads as text (core_internal.h,
 * "tagged string references"; image/records.h). A string reference other
 * than `none` with `slabBit` set is a slab string: (reference & ~slabBit) <<
 * slabShift is its offset in the pages of ta_system's `slab` (whose
 * `pages` member is relative to it), where a `header` lies and its `length`
 * bytes follow it. Any other is a string id of the segment's image: the
 * offset in the image's strings section (`image.strings`, a section kind)
 * of a u32 length and the bytes. An event name is an index of the image's
 * event names section (`image.eventNames`): an `eventName` record whose
 * `name` is a string id. The image's integers are little-endian whatever
 * the region's are; its `header` is followed by its `sectionCount`
 * `section` records, each of a `kind`, an `offset` from the image's start,
 * a `size` in bytes and a `count` of records. */
static void write_strings(ta_cbor_writer *writer) {
    ta_cbor_write_text(writer, text_of("strings"));
    ta_cbor_write_map(writer, 8);
    write_number(writer, "none", TA_NIL);
    write_number(writer, "slabBit", TA_STRING_REF_SLAB);
    write_number(writer, "slabShift", TA_STRING_REF_SHIFT);
    write_pair(writer, "slab", "slab");
    write_pair(writer, "pages", "pages_relative");
    write_pair(writer, "header", "ta_shared_string");
    write_pair(writer, "length", "length");
    ta_cbor_write_text(writer, text_of("image"));
    ta_cbor_write_map(writer, 13);
    write_pair(writer, "byteOrder", "little");
    write_pair(writer, "header", "ta_header_record");
    write_pair(writer, "sectionCount", "section_count");
    write_pair(writer, "section", "ta_section_record");
    write_pair(writer, "kind", "kind");
    write_pair(writer, "offset", "offset");
    write_pair(writer, "size", "size");
    write_pair(writer, "count", "count");
    write_number(writer, "strings", TA_SECTION_STRINGS);
    write_pair(writer, "length", "u32");
    write_number(writer, "eventNames", TA_SECTION_EVENT_NODES);
    write_pair(writer, "eventName", "ta_event_node_record");
    write_pair(writer, "name", "name");
}


/* ---- the image: its sections and how a chart's records are found ---- */
typedef struct layout_section {
    uint32_t kind;
    const char *name, *element;
} layout_section;
static const layout_section sections[] = {
    {TA_SECTION_STRINGS, "strings", "bytes"},
    {TA_SECTION_CHARTS, "charts", "ta_chart_record"},
    {TA_SECTION_STATES, "states", "ta_state_record"},
    {TA_SECTION_TRANSITIONS, "transitions", "ta_transition_record"},
    {TA_SECTION_U32, "u32", "u32"},
    {TA_SECTION_CODE, "code", "u32"},
    {TA_SECTION_EXPRESSIONS, "expressions", "ta_expression_record"},
    {TA_SECTION_CAPTURES, "captures", "ta_capture_record"},
    {TA_SECTION_SENDS, "sends", "ta_send_record"},
    {TA_SECTION_INVOKES, "invokes", "ta_invoke_record"},
    {TA_SECTION_DATA, "data", "ta_data_record"},
    {TA_SECTION_EVENT_NODES, "eventNames", "ta_event_node_record"},
    {TA_SECTION_ACTORS, "actors", "ta_actor_record"},
};
/* A chart's records of a section: `count` of them from `first` (members of
 * ta_chart_record); its "chart:<section>" indices count from `first`. */
typedef struct layout_range {
    const char *section, *first, *count;
} layout_range;
static const layout_range ranges[] = {
    {"states", "first_state", "state_count"},
    {"transitions", "first_transition", "transition_count"},
    {"expressions", "first_expression", "expression_count"},
    {"sends", "first_send", "send_count"},
    {"invokes", "first_invoke", "invoke_count"},
    {"data", "first_data", "data_count"},
    {"code", "first_code", "code_count"},
};
/* Names of the values of a member, by value (index); "" for none. */
static const char *const state_kinds[] = {"atomic", "compound", "parallel",
                                          "final",  "history-shallow", "history-deep"};
static const char *const expression_kinds[] = {"",       "condition", "value",   "location",
                                               "script", "string",    "content", "item",
                                               "global-script"};
static const char *const send_targets[] = {"none",   "internal", "parent",
                                           "invoke", "string",   "expression"};
static const char *const bindings[] = {"early", "late"};
typedef struct layout_names {
    const char *type, *member;
    const char *const *names;
    uint32_t count;
} layout_names;
static const layout_names value_names[] = {
    {"ta_state_record", "kind", state_kinds, sizeof state_kinds / sizeof state_kinds[0]},
    {"ta_expression_record", "kind", expression_kinds,
     sizeof expression_kinds / sizeof expression_kinds[0]},
    {"ta_send_record", "target_kind", send_targets, sizeof send_targets / sizeof send_targets[0]},
    {"ta_chart_record", "binding", bindings, sizeof bindings / sizeof bindings[0]},
};
typedef struct layout_flag {
    const char *type, *member, *name;
    uint32_t bit;
} layout_flag;
static const layout_flag flags[] = {
    {"ta_transition_record", "flags", "internal", TA_TRANSITION_FLAG_INTERNAL},
    {"ta_transition_record", "flags", "historyTarget", TA_TRANSITION_FLAG_HISTORY_TARGET},
    {"ta_transition_record", "flags", "initial", TA_TRANSITION_FLAG_INITIAL},
    {"ta_state_record", "flags", "root", TA_STATE_FLAG_ROOT},
};

/* How a reader finds every chart of an image and its records. The image's
 * integers are little-endian whatever the region's are; its header is
 * followed by its `sectionCount` section records, each of a `kind`, an
 * `offset` from the image's start, a `size` in bytes and a `count` of
 * records of the section's `element`. A "u32-pool" member is an offset, in
 * u32s, into the "u32" section, where its `list` lies; an "event-name" and
 * the "event-name-end" after it are an event descriptor, the event names
 * lo <= e < hi. Chart-local indices
 * ("chart:<section>") count from the chart's `ranges`. State 0 of each
 * chart is its <scxml> root; states are in document order, a state's
 * descendants being the states after it up to its `end`. A compound
 * state's `initial` is a transition whose targets are its initial states.
 * The image holds no SCXML text: a chart's `source` names the document it
 * was compiled from. */
static void write_image(ta_cbor_writer *writer) {
    ta_cbor_write_text(writer, text_of("image"));
    ta_cbor_write_map(writer, 13);
    write_pair(writer, "byteOrder", "little");
    write_pair(writer, "header", "ta_header_record");
    write_pair(writer, "sectionCount", "section_count");
    write_pair(writer, "section", "ta_section_record");
    write_pair(writer, "kind", "kind");
    write_pair(writer, "offset", "offset");
    write_pair(writer, "size", "size");
    write_pair(writer, "count", "count");
    write_pair(writer, "chart", "ta_chart_record");
    ta_cbor_write_text(writer, text_of("sections"));
    ta_cbor_write_array(writer, (uint32_t)(sizeof sections / sizeof sections[0]));
    for (uint32_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        ta_cbor_write_map(writer, 3);
        write_number(writer, "kind", sections[i].kind);
        write_pair(writer, "name", sections[i].name);
        write_pair(writer, "element", sections[i].element);
    }
    ta_cbor_write_text(writer, text_of("ranges"));
    ta_cbor_write_array(writer, (uint32_t)(sizeof ranges / sizeof ranges[0]));
    for (uint32_t i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        ta_cbor_write_map(writer, 3);
        write_pair(writer, "section", ranges[i].section);
        write_pair(writer, "first", ranges[i].first);
        write_pair(writer, "count", ranges[i].count);
    }
    ta_cbor_write_text(writer, text_of("names"));
    ta_cbor_write_array(writer, (uint32_t)(sizeof value_names / sizeof value_names[0]));
    for (uint32_t i = 0; i < sizeof value_names / sizeof value_names[0]; i++) {
        ta_cbor_write_map(writer, 3);
        write_pair(writer, "type", value_names[i].type);
        write_pair(writer, "member", value_names[i].member);
        ta_cbor_write_text(writer, text_of("values"));
        ta_cbor_write_array(writer, value_names[i].count);
        for (uint32_t k = 0; k < value_names[i].count; k++)
            ta_cbor_write_text(writer, text_of(value_names[i].names[k]));
    }
    ta_cbor_write_text(writer, text_of("flags"));
    ta_cbor_write_array(writer, (uint32_t)(sizeof flags / sizeof flags[0]));
    for (uint32_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        ta_cbor_write_map(writer, 4);
        write_pair(writer, "type", flags[i].type);
        write_pair(writer, "member", flags[i].member);
        write_pair(writer, "name", flags[i].name);
        write_number(writer, "bit", flags[i].bit);
    }
}

/* An actor's instance (core_internal.h, ta_instance_layout_of): at the slab
 * offset its `instance` holds while resident, a `header`, then `areas` of
 * u32 words in order, each `words` long (a member of the chart's record, or
 * "stateWords": one bit per state of the chart, rounded up to whole words),
 * present only when its `when` holds for the chart's record. A bit set is
 * LSB first: state i is bit i % 32 of word i / 32. `config` is the active
 * configuration. A history state's recorded states are the bits of
 * `history`, from its `historyOffset`, over its parent's descendants (bit i:
 * the state parent + 1 + i), set when its bit of `history_recorded` is. */
static void write_instance(ta_cbor_writer *writer) {
    ta_cbor_write_text(writer, text_of("instance"));
    ta_cbor_write_map(writer, 6);
    write_pair(writer, "member", "instance");
    write_pair(writer, "header", "ta_instance");
    write_pair(writer, "word", "u32");
    write_pair(writer, "stateCount", "state_count");
    write_pair(writer, "historyOffset", "history_offset");
    ta_cbor_write_text(writer, text_of("areas"));
    ta_cbor_write_array(writer, 7);
    static const struct {
        const char *name, *words, *when;
        uint32_t value; /* the `when` member's value; TA_NIL: any but 0 */
    } areas[] = {
        {"config", "stateWords", 0, 0},
        {"to_invoke", "stateWords", 0, 0},
        {"data_init", "stateWords", "binding", TA_BINDING_LATE},
        {"history_recorded", "stateWords", "history_word_count", TA_NIL},
        {"history", "history_word_count", 0, 0},
        {"invoke_children", "invoke_count", 0, 0},
        {"invoke_ids", "invoke_count", 0, 0},
    };
    for (uint32_t i = 0; i < 7; i++) {
        ta_cbor_write_map(writer, areas[i].when ? 3 : 2);
        write_pair(writer, "name", areas[i].name);
        write_pair(writer, "words", areas[i].words);
        if (!areas[i].when) continue;
        ta_cbor_write_text(writer, text_of("when"));
        ta_cbor_write_map(writer, 2);
        write_pair(writer, "member", areas[i].when);
        if (areas[i].value == TA_NIL) {
            ta_cbor_write_text(writer, text_of("nonzero"));
            ta_cbor_write_simple(writer, 21); /* true */
        } else {
            ta_cbor_write_text(writer, text_of("values"));
            ta_cbor_write_array(writer, 1);
            ta_cbor_write_uint(writer, areas[i].value);
        }
    }
}

/* The trace records (<tinyactors/trace.h>) in the region's journal, a
 * `journal` member of the system: its storage is relative to it, `capacity`
 * bytes. The records kept run from `head`: the acknowledged ones (`retained`
 * bytes) up to `read`, then the pending ones (`used` bytes) up to `tail`;
 * while `wrapped`, the records before the wrap end at `end` and go on from
 * offset 0. Each record is a `record` header, its kind's `body`, then the
 * kind's `lists` of u32s (each as long as its body's `count` member), its
 * `size` (a multiple of 8) the whole. A string member is a string
 * reference, as "strings" reads it; a dynamic string of an acknowledged
 * record may have been freed and reused since, so a reader reads only
 * those of pending records. Times are the system's: `now` in ticks, `fuel`
 * its fuel clock. */
static void write_trace(ta_cbor_writer *writer) {
    static const struct {
        uint32_t kind;
        const char *body;
        const char *counts[3], *elements[3];
    } kinds[] = {
        {TA_TRACE_DROPPED, "ta_trace_dropped", {0}, {0}},
        {TA_TRACE_SPAWNED, "ta_trace_spawned", {0}, {0}},
        {TA_TRACE_FINISHED, "ta_trace_finished", {0}, {0}},
        {TA_TRACE_DESTROYED, "ta_trace_lifecycle", {0}, {0}},
        {TA_TRACE_UNLOADED, "ta_trace_lifecycle", {0}, {0}},
        {TA_TRACE_LOADED, "ta_trace_lifecycle", {0}, {0}},
        {TA_TRACE_MACROSTEP_BEGIN, "ta_trace_macrostep_begin", {0}, {0}},
        {TA_TRACE_MACROSTEP_END, "ta_trace_macrostep_end", {"state_count"}, {"chart:states"}},
        {TA_TRACE_MICROSTEP,
         "ta_trace_microstep",
         {"transition_count", "exited_count", "entered_count"},
         {"chart:transitions", "chart:states", "chart:states"}},
        {TA_TRACE_CONDITION, "ta_trace_condition", {0}, {0}},
        {TA_TRACE_RAISED, "ta_trace_raised", {0}, {0}},
        {TA_TRACE_SENT, "ta_trace_sent", {0}, {0}},
        {TA_TRACE_INVOKED, "ta_trace_invoked", {0}, {0}},
        {TA_TRACE_CANCELLED, "ta_trace_cancelled", {0}, {0}},
        {TA_TRACE_ASSIGN, "ta_trace_assign", {0}, {0}},
        {TA_TRACE_LOG, "ta_trace_log", {0}, {0}},
        {TA_TRACE_ACTION_FAILED, "ta_trace_action_failed", {0}, {0}},
    };
    ta_cbor_write_text(writer, text_of("trace"));
    ta_cbor_write_map(writer, 20);
    write_pair(writer, "journal", "journal");
    write_pair(writer, "storage", "storage_relative");
    static const char *const fields[] = {"capacity", "head", "read", "tail", "end", "wrapped",
                                         "used", "retained", "lost_total", "written_total"};
    for (uint32_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        write_pair(writer, fields[i], fields[i]);
    write_pair(writer, "record", "ta_trace_record");
    write_pair(writer, "kind", "kind");
    write_pair(writer, "size", "size");
    write_number(writer, "sampled", TA_TRACE_FLAG_SAMPLED);
    write_pair(writer, "tracing", "config.tracing"); /* `disabled`: no journal */
    write_number(writer, "disabled", TA_TRACING_DISABLED);
    /* a macrostep's event `type`, by value (TA_EVENT_TYPE_*) */
    static const char *const event_types[] = {"platform", "internal", "external"};
    ta_cbor_write_text(writer, text_of("eventTypes"));
    ta_cbor_write_array(writer, 3);
    for (uint32_t i = 0; i < 3; i++)
        ta_cbor_write_text(writer, text_of(event_types[i]));
    ta_cbor_write_text(writer, text_of("kinds"));
    ta_cbor_write_array(writer, (uint32_t)(sizeof kinds / sizeof kinds[0]));
    for (uint32_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        uint32_t lists = 0;
        while (lists < 3 && kinds[i].counts[lists])
            lists++;
        ta_cbor_write_map(writer, 4);
        write_number(writer, "kind", kinds[i].kind);
        ta_string_view name = ta_trace_kind_name(kinds[i].kind);
        ta_cbor_write_text(writer, text_of("name"));
        ta_cbor_write_text(writer, name);
        write_pair(writer, "body", kinds[i].body);
        ta_cbor_write_text(writer, text_of("lists"));
        ta_cbor_write_array(writer, lists);
        for (uint32_t k = 0; k < lists; k++) {
            ta_cbor_write_map(writer, 2);
            write_pair(writer, "count", kinds[i].counts[k]);
            write_pair(writer, "element", kinds[i].elements[k]);
        }
    }
}

ta_status ta_system_layout_description(ta_buffer *out, uint64_t *id) {
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, *out, (const ta_allocator *)0);
    char library[40];
    const uint16_t order = 1;
    ta_cbor_write_map(&writer, 12);
    ta_cbor_write_text(&writer, text_of("library"));
    ta_cbor_write_text(&writer, library_text(library));
    write_pair(&writer, "byteOrder", *(const unsigned char *)&order ? "little" : "big");
    write_number(&writer, "pointerBytes", sizeof(void *));
    write_number(&writer, "nil", TA_NIL);
    write_pair(&writer, "system", "ta_system");
    ta_cbor_write_text(&writer, text_of("types"));
    ta_cbor_write_array(&writer, (uint32_t)(sizeof types / sizeof types[0]));
    for (uint32_t i = 0; i < sizeof types / sizeof types[0]; i++)
        write_type(&writer, &types[i]);
    ta_cbor_write_text(&writer, text_of("pools"));
    ta_cbor_write_array(&writer, (uint32_t)(sizeof pools / sizeof pools[0]));
    for (uint32_t i = 0; i < sizeof pools / sizeof pools[0]; i++) {
        ta_cbor_write_map(&writer, 3);
        write_pair(&writer, "name", pools[i].name);
        write_pair(&writer, "member", pools[i].name);
        write_pair(&writer, "element", pools[i].element);
    }
    ta_cbor_write_text(&writer, text_of("arrays"));
    ta_cbor_write_array(&writer, (uint32_t)(sizeof arrays / sizeof arrays[0]));
    for (uint32_t i = 0; i < sizeof arrays / sizeof arrays[0]; i++) {
        ta_cbor_write_map(&writer, arrays[i].count ? 4 : 3);
        write_pair(&writer, "name", arrays[i].name);
        write_pair(&writer, "offset", arrays[i].offset);
        write_pair(&writer, "element", arrays[i].element);
        if (arrays[i].count) write_pair(&writer, "count", arrays[i].count);
    }
    write_strings(&writer);
    write_image(&writer);
    write_instance(&writer);
    write_trace(&writer);
    ta_bytes bytes;
    ta_status status = ta_cbor_writer_bytes(&writer, &bytes);
    out->length = writer.length; /* what the description needs, written or not */
    if (status != TA_OK) return status;
    if (id) *id = ta_fnv1a64(bytes);
    return TA_OK;
}

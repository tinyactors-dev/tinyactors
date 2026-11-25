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
 * host pointer, zero in a snapshot). An anonymous union is described by its
 * members, each at the union's offset. */
#include <stddef.h>

#include <tinyactors/cbor.h>

#include "system/system_internal.h"

typedef struct layout_member {
    const char *name;
    uint32_t offset, size, count;
    const char *type;
    const char *kind; /* NULL: none */
} layout_member;

typedef struct layout_type {
    const char *name;
    uint32_t size;
    const layout_member *members;
    uint32_t count;
} layout_type;

/* Only the preprocessor can name a member for offsetof and sizeof. */
#define MEMBER(record, member, type, kind)                                                         \
    {#member, offsetof(record, member), sizeof(((record *)0)->member), 1, type, kind}
#define ARRAY(record, member, type, kind)                                                          \
    {#member, offsetof(record, member), sizeof(((record *)0)->member),                             \
     sizeof(((record *)0)->member) / sizeof(((record *)0)->member[0]), type, kind}

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
    MEMBER(ta_actor, instance, "u32", 0),
    MEMBER(ta_actor, mailbox_head, "u32", "index:events"),
    MEMBER(ta_actor, mailbox_tail, "u32", "index:events"),
    MEMBER(ta_actor, mailbox_length, "u32", 0),
    MEMBER(ta_actor, parent, "u32", "handle:actors"),
    MEMBER(ta_actor, parent_slot, "u32", 0),
    MEMBER(ta_actor, timers, "u32", "index:timers"),
    MEMBER(ta_actor, name, "u32", 0),
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
    MEMBER(ta_event, name_id, "u32", 0),
    MEMBER(ta_event, name, "u32", 0),
    MEMBER(ta_event, sendid, "u32", 0),
    MEMBER(ta_event, origin, "u32", "handle:actors"),
    MEMBER(ta_event, origin_string, "u32", 0),
    MEMBER(ta_event, origintype, "u32", 0),
    MEMBER(ta_event, invokeid, "u32", 0),
    MEMBER(ta_event, invoke_slot, "u16", 0),
    MEMBER(ta_event, flags, "u16", 0),
    MEMBER(ta_event, raw, "u32", 0),
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
    MEMBER(ta_timer, target, "u32", 0),
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
    MEMBER(ta_request, invokeid, "u32", 0),
    MEMBER(ta_request, source, "u32", 0),
    MEMBER(ta_request, pad, "u32", 0),
    MEMBER(ta_request, payload, "u64", 0),
    MEMBER(ta_request, type, "u32", 0),
    MEMBER(ta_request, next, "u32", "index:requests"),
    MEMBER(ta_request, ended, "u32", 0),
};
static const layout_member effect_record_members[] = {
    MEMBER(ta_effect_record, kind, "u32", 0),
    MEMBER(ta_effect_record, actor, "u32", 0),
    MEMBER(ta_effect_record, token, "u32", 0),
    MEMBER(ta_effect_record, io_processor, "u32", 0),
    MEMBER(ta_effect_record, flags, "u32", 0),
    MEMBER(ta_effect_record, target, "u32", 0),
    MEMBER(ta_effect_record, event_name, "u32", 0),
    MEMBER(ta_effect_record, sendid, "u32", 0),
    MEMBER(ta_effect_record, auxiliary, "u32", 0),
    MEMBER(ta_effect_record, event, "u32", "index:events"),
    MEMBER(ta_effect_record, datamodel, "u32", 0),
    MEMBER(ta_effect_record, pad, "u32", 0),
    MEMBER(ta_effect_record, payload, "u64", 0),
    MEMBER(ta_effect_record, delay, "u64", 0),
    MEMBER(ta_effect_record, serial, "u64", 0),
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
    MEMBER(ta_trace_rule_compiled, event_text, "u32", 0),
    MEMBER(ta_trace_rule_compiled, actor, "u32", 0),
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
    MEMBER(ta_journal, tail, "u32", 0),
    MEMBER(ta_journal, end, "u32", 0),
    MEMBER(ta_journal, wrapped, "u32", 0),
    MEMBER(ta_journal, reserved, "u32", 0),
    MEMBER(ta_journal, at, "u32", 0),
    MEMBER(ta_journal, used, "u32", 0),
    MEMBER(ta_journal, lost, "u32", 0),
    MEMBER(ta_journal, pad, "u32", 0),
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
};

#undef MEMBER
#undef ARRAY

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
        ta_cbor_write_map(writer, member->kind ? 6 : 5);
        write_pair(writer, "name", member->name);
        write_number(writer, "offset", member->offset);
        write_number(writer, "size", member->size);
        write_number(writer, "count", member->count);
        write_pair(writer, "type", member->type);
        if (member->kind) write_pair(writer, "kind", member->kind);
    }
}

static uint64_t fnv1a64(ta_bytes bytes) {
    const unsigned char *in = (const unsigned char *)bytes.pointer;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < bytes.length; i++) {
        hash ^= in[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

ta_status ta_system_layout_description(ta_buffer *out, uint64_t *id) {
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, *out, (const ta_allocator *)0);
    char library[40];
    const uint16_t order = 1;
    ta_cbor_write_map(&writer, 8);
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
    ta_bytes bytes;
    ta_status status = ta_cbor_writer_bytes(&writer, &bytes);
    out->length = writer.length; /* what the description needs, written or not */
    if (status != TA_OK) return status;
    if (id) *id = fnv1a64(bytes);
    return TA_OK;
}

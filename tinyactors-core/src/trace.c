#include "trace.h"
#include "system.h"
#include "interpreter.h"
#include "../include/tinyactors.h"
#include "datamodel.h"
#include "definition.h"
#include "actor.h"
#include "atom.h"
#include "trie.h"
#include "hash.h"
#include "memory.h"
#include "str.h"
#include "ring.h"
#include "span.h"
#include "scxml_uri.h"
#include "text_writer.h"
#include "generated/ring_records.h"
#include "generated/ring_codec.h"

_Static_assert(sizeof(ta_tel_trace) == TA_TRACE_FIXED_BYTES, "TA_TRACE_FIXED_BYTES is tel.trace's fixed part");
/* ta_trace_write measures a record as ta_trace_fit's 23 texts and the two lists: a text the
 * catalog adds must be cut and measured there too. */
_Static_assert(sizeof(ta_tel_trace_texts) == 25u * sizeof(ta_str), "tel.trace has 25 texts");

#define TA_TRACE_LIT(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * Step keys as span IDs (ADR 0067 section 6)
 */

uint64_t ta_step_key_span_id(ta_step_key key) {
    if (key.session == 0) {
        return key.macrostep;
    }
    uint64_t span = ta_hash_mix64(key.session);
    span = ta_hash_mix64(span ^ key.macrostep);
    span = ta_hash_mix64(span ^ ((uint64_t)key.microstep | ((uint64_t)1u << 32)));
    return span == 0 ? 1u : span;
}

/* ------------------------------------------------------------------------
 * What is built: the masks and the clock
 */

uint32_t ta_trace_macrostep_kinds(const ta_trace_masks *masks, const ta_trace_context *context) {
    if (!ta_trace_context_empty(context) && (context->flags & TA_TRACE_FLAG_SAMPLED) == 0) {
        return masks->unsampled_kinds;
    }
    return (context->detail & TA_TRACE_DETAIL_WATCHED) != 0 ? masks->watched_kinds : masks->kinds;
}

ta_result ta_system_set_trace_masks(ta_system *system, const ta_trace_masks *masks) {
    if (system == NULL || masks == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->trace.scratch == NULL && ta_trace_any_kinds(masks) != 0) {
        return TA_ERR_INVALID; /* telBytes 0: no ring to write to (ADR 0098 section 10) */
    }
    system->trace.masks = *masks;
    if (system->trace.masks.value_bytes > system->trace.value_bytes) {
        system->trace.masks.value_bytes = system->trace.value_bytes; /* at most telemetryValueBytes */
    }
    return TA_OK;
}

ta_result ta_system_set_wall_clock(ta_system *system, ta_wall_clock clock) {
    if (system == NULL || (clock.v != NULL && clock.v->wall_ns == NULL)) {
        return TA_ERR_INVALID;
    }
    system->trace.clock = clock;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * The ring and the scratch area
 */

ta_result ta_trace_open(ta_system *system) {
    uint32_t ring_bytes = system->params[TA_PARAM_TEL_BYTES] & ~(TA_RING_ALIGNMENT - 1u);
    uint32_t record_bytes = system->params[TA_PARAM_MAX_RECORD_BYTES] & ~(TA_RING_ALIGNMENT - 1u);
    system->trace.value_bytes = system->params[TA_PARAM_TELEMETRY_VALUE_BYTES];
    if (ring_bytes == 0) {
        return TA_OK;
    }
    unsigned char *scratch = ta_mem_alloc(system->allocator, record_bytes, TA_RING_ALIGNMENT);
    if (scratch == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_ring_init(&system->control.tel, &(ta_ring_params){ .allocator = system->allocator,
                                                                             .capacity = ring_bytes,
                                                                             .max_record_bytes = record_bytes,
                                                                             .mode = TA_RING_LOSSY });
    if (result != TA_OK) {
        ta_mem_free(system->allocator, scratch,
                    &(ta_allocator_free_params){ .size = record_bytes, .alignment = TA_RING_ALIGNMENT });
        return result;
    }
    system->trace.scratch = scratch;
    system->trace.scratch_bytes = record_bytes;
    return TA_OK;
}

void ta_trace_close(ta_system *system) {
    system->trace.masks = (ta_trace_masks){ 0 };
    if (system->trace.scratch == NULL) {
        return;
    }
    ta_ring_destroy(&system->control.tel);
    ta_mem_free(system->allocator, system->trace.scratch,
                &(ta_allocator_free_params){ .size = system->trace.scratch_bytes, .alignment = TA_RING_ALIGNMENT });
    system->trace.scratch = NULL;
    system->trace.scratch_bytes = 0;
}

void ta_trace_forget_definition(ta_tracer *tracer, ta_definition_id definition) {
    if (tracer->facts.definition == definition) {
        tracer->facts = (ta_trace_facts){ 0 };
    }
    if (tracer->transition_label.key.definition == definition) {
        tracer->transition_label.key = (ta_trace_label_key){ 0 };
    }
    if (tracer->action_label.key.definition == definition) {
        tracer->action_label.key = (ta_trace_label_key){ 0 };
    }
}

/* ------------------------------------------------------------------------
 * Lifecycle records
 */

ta_step_key ta_trace_lifecycle_key(ta_actor *actor) {
    actor->trace_lifecycle += 1u;
    return (ta_step_key){ .session = actor->session_id, .microstep = actor->trace_lifecycle,
                          .family = actor->definition->family };
}

void ta_trace_lifecycle(ta_system *system, ta_actor *actor, ta_trace_kind kind, const ta_trace_parent *parent) {
    ta_step_key key = ta_trace_lifecycle_key(actor);
    /* Counters and occupancy (ADR 0072): every residency change passes here. */
    switch (kind) {
    case TA_TRACE_SPAWNED: TA_COUNT(system, actor->definition, spawned); break;
    case TA_TRACE_LOADED:
        TA_COUNT(system, actor->definition, loaded);
        ta_interpreter_occupancy(actor, true);
        break;
    case TA_TRACE_UNLOADED:
        TA_COUNT(system, actor->definition, unloaded);
        ta_interpreter_occupancy(actor, false);
        break;
    case TA_TRACE_DESTROYED:
        TA_COUNT(system, actor->definition, destroyed);
        ta_interpreter_occupancy(actor, false);
        break;
    default: break;
    }
    ta_trace_context none = { 0 };
    if (!ta_trace_lifecycle_wanted(&system->trace.masks, kind, parent != NULL ? &parent->context : &none)) {
        return;
    }
    ta_trace_entry entry = ta_trace_entry_blank(kind);
    entry.key = key;
    entry.actor = actor;
    entry.definition = actor->definition;
    entry.now = ta_actor_now(system, actor);
    if (parent != NULL) {
        entry.context = parent->context;
        entry.cause = parent->cause;
    }
    ta_trace_write(system, &entry);
}

ta_trace_entry ta_trace_entry_blank(ta_trace_kind kind) {
    return (ta_trace_entry){
        .kind = kind,
        .position = { .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE, .transition = TA_TRANSITION_NONE,
                      .action = TA_ACTION_NONE },
        .state = TA_STATE_NONE,
        .expr = TA_EXPR_ID_NONE,
        .action = TA_ACTION_NONE,
        .invoke = TA_INVOKE_ID_NONE,
    };
}

/* ------------------------------------------------------------------------
 * Composing a record's texts
 *
 * Texts core has as they are (atom texts, the definition's) are borrowed;
 * texts core builds (session IDs, labels, event names, the lists) are laid
 * out one after another in the scratch area, which holds maxRecordBytes,
 * as much as any record can carry.
 */

/* The scratch area, filled from the front. */
typedef struct {
    unsigned char *bytes;
    uint32_t capacity;
    uint32_t used;
} ta_trace_area;

/* A writer over what is left of the area. */
static ta_text_writer ta_area_writer(const ta_trace_area *area) {
    return (ta_text_writer){ .buffer = (char *)area->bytes + area->used, .capacity = area->capacity - area->used };
}

/* The longest prefix of `text`, at most `limit` bytes, that ends on a code point boundary. */
static uint32_t ta_utf8_cut(const char *text, uint32_t len, uint32_t limit) {
    if (len <= limit) {
        return len;
    }
    uint32_t at = limit;
    while (at > 0 && ((unsigned char)text[at] & 0xc0u) == 0x80u) {
        at -= 1u;
    }
    return at;
}

/* Keeps what `writer` wrote into the area as one text; `*cut` when it ran past the area. */
static ta_str ta_area_take(ta_trace_area *area, const ta_text_writer *writer, bool *cut) {
    uint32_t len = writer->length;
    if (len > writer->capacity) {
        len = ta_utf8_cut(writer->buffer, writer->capacity, writer->capacity);
        *cut = true;
    }
    area->used += len;
    return (ta_str){ .ptr = len == 0 ? NULL : writer->buffer, .len = len };
}

/* A state's name: its author ID. */
static ta_str ta_trace_state_name(const ta_system *system, const ta_definition *definition, ta_state_id state) {
    if (state >= definition->state_count) {
        return (ta_str){ 0 };
    }
    return ta_atom_text(&system->atoms, definition->states[state].name);
}

/* Where the labels below look names up: the system's atoms and one
 * definition; and the tracer, which keeps the last labels composed. */
typedef struct ta_trace_names {
    const ta_system *system;
    const ta_definition *definition;
    ta_tracer *tracer;
} ta_trace_names;

/* Puts the label `label` keeps when it is the one `key` names; false when it is not. */
static bool ta_trace_label_put(const ta_trace_label *label, ta_trace_label_key key, ta_text_writer *writer) {
    if (label->key.definition != key.definition || label->key.id != key.id || key.definition == 0) {
        return false;
    }
    ta_text_put(writer, (ta_str){ .ptr = label->text, .len = label->len });
    return true;
}

/* What `writer` stored from `start` on, when it stored all of it; false when it ran out of room. */
static bool ta_trace_written(const ta_text_writer *writer, uint32_t start, ta_str *out) {
    if (writer->buffer == NULL || writer->skip != 0 || writer->length > writer->capacity || start > writer->length) {
        return false;
    }
    *out = (ta_str){ .ptr = writer->buffer + start, .len = writer->length - start };
    return true;
}

/* Keeps `text` as the label `key` names, when it fits. */
static void ta_trace_label_keep(ta_trace_label *label, ta_trace_label_key key, ta_str text) {
    if (text.len > TA_TRACE_LABEL_TEXT) {
        return;
    }
    ta_mem_copy(label->text, text.ptr, text.len);
    label->len = text.len;
    label->key = key;
}

/* Puts a state's name, or "s<id>" when the definition has none by that ID. */
static void ta_trace_put_state(const ta_trace_names *names, ta_state_id state, ta_text_writer *writer) {
    if (state >= names->definition->state_count) {
        ta_text_put(writer, TA_TRACE_LIT("s"));
        ta_text_put_u64(writer, state);
        return;
    }
    ta_text_put(writer, ta_trace_state_name(names->system, names->definition, state));
}

/* Puts a transition's label: "idle → busy on order.placed [guarded]",
 * "initial of p → a", "initial of the document → a", "busy on poke
 * (internal)", or "t<id>" when there is none by that ID. */
static void ta_trace_compose_transition(const ta_trace_names *names, ta_transition_id transition,
                                        ta_text_writer *writer) {
    ta_transition_info info = { 0 };
    if (ta_definition_transition(names->system,
                                 &(ta_definition_transition_params){ .definition = names->definition->id,
                                                                             .transition = transition,
                                                                             .out = &info }) != TA_OK) {
        ta_text_put(writer, TA_TRACE_LIT("t"));
        ta_text_put_u64(writer, transition);
        return;
    }
    if (info.origin != TA_TRANSITION_AUTHORED) {
        ta_text_put(writer, TA_TRACE_LIT("initial of "));
        if (info.origin == TA_TRANSITION_BOOTSTRAP) {
            ta_text_put(writer, TA_TRACE_LIT("the document"));
        } else {
            ta_trace_put_state(names, info.source, writer);
        }
        ta_text_put(writer, TA_TRACE_LIT(" \xe2\x86\x92"));
        for (uint32_t at = 0; at < info.target_count; at += 1u) {
            ta_text_put(writer, TA_TRACE_LIT(" "));
            ta_trace_put_state(names, info.targets[at], writer);
        }
        return;
    }
    ta_trace_put_state(names, info.source, writer);
    if (info.target_count != 0) {
        ta_text_put(writer, TA_TRACE_LIT(" \xe2\x86\x92"));
        for (uint32_t at = 0; at < info.target_count; at += 1u) {
            ta_text_put(writer, TA_TRACE_LIT(" "));
            ta_trace_put_state(names, info.targets[at], writer);
        }
    }
    if (info.event.len != 0) {
        ta_text_put(writer, TA_TRACE_LIT(" on "));
        ta_text_put(writer, info.event);
    }
    if (info.condition != TA_EXPR_ID_NONE) {
        ta_text_put(writer, TA_TRACE_LIT(" [guarded]"));
    }
    if (info.type == TA_SCXML_TRANSITION_INTERNAL) {
        ta_text_put(writer, TA_TRACE_LIT(" (internal)"));
    }
}

/* Puts a transition's label, the one the tracer kept when it is the last composed. */
static void ta_trace_put_transition(const ta_trace_names *names, ta_transition_id transition,
                                    ta_text_writer *writer) {
    ta_trace_label *label = &names->tracer->transition_label;
    ta_trace_label_key key = { .definition = names->definition->id, .id = transition };
    if (ta_trace_label_put(label, key, writer)) {
        return;
    }
    uint32_t start = writer->length;
    ta_trace_compose_transition(names, transition, writer);
    ta_str text = { 0 };
    if (ta_trace_written(writer, start, &text)) {
        ta_trace_label_keep(label, key, text);
    }
}

static const char *const ta_trace_action_elements[] = {
    "<?>", "<raise>", "<send>", "<cancel>", "<assign>", "<script>", "<log>", "<if>", "<foreach>", "<invoke>",
};

/* Puts an action's label: "<assign> in onentry of busy", "<raise> in then
 * of <if> in onexit of idle", "<log> in finalize of <invoke> #2 in a". It
 * walks up from the action to its block's owner, one level at a time. */
static void ta_trace_compose_action(const ta_trace_names *names, ta_action_id action, ta_text_writer *writer) {
    ta_action_id id = action;
    uint32_t start = writer->length;
    for (;;) {
        ta_action_info info = { 0 };
        if (ta_definition_action(names->system, &(ta_definition_action_params){ .definition = names->definition->id,
                                                                         .action = id,
                                                                         .out = &info }) != TA_OK) {
            writer->length = start; /* nothing stored past the start is kept */
            ta_text_put(writer, TA_TRACE_LIT("a"));
            ta_text_put_u64(writer, action);
            return;
        }
        uint32_t kind = (uint32_t)info.kind < TA_ARRAY_COUNT(ta_trace_action_elements) ? (uint32_t)info.kind : 0u;
        ta_text_put(writer, ta_str_from_cstr(ta_trace_action_elements[kind]));
        switch (info.block) {
        case TA_ACTION_IN_THEN:
        case TA_ACTION_IN_ELSE:
        case TA_ACTION_IN_FOREACH:
            ta_text_put(writer, info.block == TA_ACTION_IN_THEN   ? TA_TRACE_LIT(" in then of ")
                                : info.block == TA_ACTION_IN_ELSE ? TA_TRACE_LIT(" in else of ")
                                                                  : TA_TRACE_LIT(" in the body of "));
            id = info.owner;
            continue;
        case TA_ACTION_IN_ONENTRY:
        case TA_ACTION_IN_ONEXIT:
            ta_text_put(writer, info.block == TA_ACTION_IN_ONENTRY ? TA_TRACE_LIT(" in onentry") : TA_TRACE_LIT(" in onexit"));
            if (info.block_index > 0) {
                ta_text_put(writer, TA_TRACE_LIT(" #"));
                ta_text_put_u64(writer, (uint64_t)info.block_index + 1u);
            }
            ta_text_put(writer, TA_TRACE_LIT(" of "));
            ta_trace_put_state(names, info.owner, writer);
            return;
        case TA_ACTION_IN_TRANSITION:
            ta_text_put(writer, TA_TRACE_LIT(" in "));
            ta_trace_put_transition(names, info.owner, writer);
            return;
        case TA_ACTION_IN_FINALIZE:
            ta_text_put(writer, TA_TRACE_LIT(" in finalize of <invoke>"));
            if (info.block_index > 0) {
                ta_text_put(writer, TA_TRACE_LIT(" #"));
                ta_text_put_u64(writer, (uint64_t)info.block_index + 1u);
            }
            ta_text_put(writer, TA_TRACE_LIT(" in "));
            ta_trace_put_state(names, info.owner, writer);
            return;
        default:
            ta_text_put(writer, TA_TRACE_LIT(" in the document's <script>"));
            return;
        }
    }
}

/* Puts an action's label, the one the tracer kept when it is the last composed. */
static void ta_trace_put_action(const ta_trace_names *names, ta_action_id action, ta_text_writer *writer) {
    ta_trace_label *label = &names->tracer->action_label;
    ta_trace_label_key key = { .definition = names->definition->id, .id = action };
    if (ta_trace_label_put(label, key, writer)) {
        return;
    }
    uint32_t start = writer->length;
    ta_trace_compose_action(names, action, writer);
    ta_str text = { 0 };
    if (ta_trace_written(writer, start, &text)) {
        ta_trace_label_keep(label, key, text);
    }
}

/* Puts an event name's dotted text. */
static void ta_trace_put_event_name(const ta_system *system, ta_event_name_id name, ta_text_writer *writer) {
    if (!ta_trie_valid(&system->event_names, name)) {
        return;
    }
    ta_atom segments[TA_TRIE_MAX_DEPTH];
    uint32_t count = ta_trie_path(&system->event_names, name, segments);
    for (uint32_t index = 0; index < count; index += 1u) {
        if (index != 0) {
            ta_text_put(writer, TA_TRACE_LIT("."));
        }
        ta_text_put(writer, ta_atom_text(&system->atoms, segments[index]));
    }
}

/* Puts a step key's session as text: `<incarnation>.<session>.<family>`, or
 * the decimal session when its family is unknown (a host's cause, a
 * definition record) or the text would be longer than TA_TRACE_SESSION_TEXT. */
static void ta_trace_put_session(const ta_system *system, const ta_step_key *key, ta_text_writer *writer) {
    ta_str family = key->family != 0 ? ta_atom_text(&system->atoms, key->family) : (ta_str){ 0 };
    if (key->session != 0 && family.len != 0) {
        char text[TA_TRACE_SESSION_TEXT];
        ta_text_writer session = { .buffer = text, .capacity = TA_TRACE_SESSION_TEXT };
        ta_session_text_write(system, &session, key->session, family);
        if (session.length <= TA_TRACE_SESSION_TEXT) {
            ta_text_put(writer, (ta_str){ .ptr = text, .len = session.length });
            return;
        }
    }
    ta_text_put_u64(writer, key->session);
}

/* The record's own session as text, written once for a run of its records. */
static ta_str ta_trace_session_text(ta_system *system, const ta_step_key *key) {
    ta_tracer *tracer = &system->trace;
    if (tracer->session_text_len == 0 || tracer->text_session != key->session || tracer->text_family != key->family) {
        ta_text_writer writer = { .buffer = tracer->session_text, .capacity = TA_TRACE_SESSION_TEXT };
        ta_trace_put_session(system, key, &writer);
        tracer->session_text_len = writer.length <= TA_TRACE_SESSION_TEXT ? writer.length : 0u;
        tracer->text_session = key->session;
        tracer->text_family = key->family;
    }
    return (ta_str){ .ptr = tracer->session_text, .len = tracer->session_text_len };
}

/* What rendering one value of a record needs. */
typedef struct {
    const ta_system *system;
    const ta_trace_entry *entry;
    const ta_value *value;
    /* Where the text is copied to, as soon as it is rendered. */
    ta_trace_area *area;
    /* Set when the text was cut. */
    bool *cut;
    /* The reason a failure gives (a fault's data, an error event's): kept
     * at telemetryValueBytes whatever the masks' value_bytes says. */
    bool reason;
} ta_trace_value_params;

/*
 * A value as the actor's data model renders it for a trace, cut at the
 * masks' value_bytes, or at telemetryValueBytes for a failure's reason,
 * and copied into the area at once: a data model's text stays valid only
 * until it is called again, and a record may render three. Empty when
 * there is nothing to render, or no value is wanted.
 */
static ta_str ta_trace_value_text(const ta_trace_value_params *params) {
    const ta_system *system = params->system;
    const ta_value *value = params->value;
    uint32_t limit = params->reason ? system->trace.value_bytes : system->trace.masks.value_bytes;
    const ta_actor *actor = params->entry->actor;
    if (limit == 0 || actor == NULL || value == NULL || value->v == NULL) {
        return (ta_str){ 0 };
    }
    const ta_definition *definition = actor->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v == NULL || v->value_text == NULL) {
        return (ta_str){ 0 };
    }
    ta_str text = { 0 };
    ta_fault fault = { 0 };
    ta_op_status status = v->value_text(definition->datamodel.self,
                                        &(ta_datamodel_value_text_params){
                                            .shared = definition->datamodel_shared,
                                            .instance = actor->datamodel_instance,
                                            .value = value,
                                            .host = &actor->host,
                                            .out = &text,
                                            .fault = &fault,
                                            .trace_bytes = limit,
                                        });
    if (status != TA_OP_OK || (text.ptr == NULL && text.len != 0)) {
        if (status == TA_OP_ERROR) {
            ta_value_release(&fault.data); /* a receiver of a fault owns its data */
        }
        return (ta_str){ 0 };
    }
    if (text.len > limit) {
        text.len = ta_utf8_cut(text.ptr, text.len, limit);
        *params->cut = true;
    }
    ta_trace_area *area = params->area;
    uint32_t room = area->capacity - area->used;
    if (text.len > room) {
        text.len = ta_utf8_cut(text.ptr, text.len, room);
        *params->cut = true;
    }
    if (text.len == 0) {
        return (ta_str){ 0 };
    }
    char *copy = (char *)area->bytes + area->used;
    ta_mem_copy(copy, text.ptr, text.len);
    area->used += text.len;
    return (ta_str){ .ptr = copy, .len = text.len };
}

/* Cuts `*text` by up to `excess` bytes, on a code point boundary unless it
 * is bytes; answers the bytes it took off. */
static uint32_t ta_trace_shorten(ta_str *text, uint32_t excess, bool bytes) {
    if (excess == 0 || text->len == 0) {
        return 0;
    }
    uint32_t keep = excess >= text->len ? 0u : text->len - excess;
    if (!bytes && keep != 0) {
        keep = ta_utf8_cut(text->ptr, text->len, keep);
    }
    uint32_t taken = text->len - keep;
    text->len = keep;
    return taken;
}

/* Appends a u32 in this machine's byte order. */
static void ta_area_put_u32(ta_trace_area *area, uint32_t value) {
    ta_mem_copy(area->bytes + area->used, &value, sizeof value);
    area->used += (uint32_t)sizeof value;
}

/* The lists of an entry, in the order a record carries them. */
typedef enum {
    TA_TRACE_LIST_TRANSITIONS = 0,
    TA_TRACE_LIST_EXITED,
    TA_TRACE_LIST_ENTERED,
    TA_TRACE_LIST_HISTORY,
    TA_TRACE_LIST_STATES,
    TA_TRACE_LIST_COUNT
} ta_trace_list;

/* What the texts of one record are, and the flags composing them set. */
typedef struct {
    ta_tel_trace_texts texts;
    uint16_t flags;
    uint32_t counts[TA_TRACE_LIST_COUNT];
} ta_trace_build;

/* What composing the lists needs. */
typedef struct {
    ta_system *system;
    const ta_trace_entry *entry;
    /* Bytes the lists may take, IDs and names together. */
    uint32_t budget;
} ta_trace_lists_params;

/*
 * Lays the lists out in the area: the IDs first, back to back, then each
 * entry's name as a u32 length and its bytes. Whole from the first list,
 * stopping at the first entry that does not fit, so the last list is cut
 * first. Stores in the build's counts how many of each there are and in its
 * texts where the IDs and names lie, and answers whether any was cut.
 */
static bool ta_trace_lists(ta_trace_area *area, const ta_trace_lists_params *params, ta_trace_build *build) {
    const ta_trace_entry *entry = params->entry;
    const ta_trace_names names = { .system = params->system, .definition = entry->definition,
                                   .tracer = &params->system->trace };
    uint32_t *counts = build->counts;
    ta_bytes *ids = &build->texts.list_ids;
    ta_bytes *list_names = &build->texts.list_names;
    const uint32_t *lists[TA_TRACE_LIST_COUNT] = { entry->transitions, entry->exited, entry->entered, entry->history,
                                                   entry->states };
    const uint32_t sizes[TA_TRACE_LIST_COUNT] = { entry->transition_count, entry->exited_count, entry->entered_count,
                                                  entry->history_count, entry->state_count };
    uint32_t total = 0;
    for (uint32_t list = 0; list < TA_TRACE_LIST_COUNT; list += 1u) {
        counts[list] = 0;
        total += lists[list] == NULL ? 0u : sizes[list];
    }
    *ids = (ta_bytes){ 0 };
    *list_names = (ta_bytes){ 0 };
    uint32_t budget = params->budget;
    if (total == 0) {
        return false;
    }
    /* Every entry takes at least 8 bytes (its ID and its length), so the IDs of
     * all that fit take at most half the budget; the names go after them. */
    uint32_t id_room = budget / 2u;
    if (id_room > total * 4u) {
        id_room = total * 4u;
    }
    if (id_room > area->capacity - area->used) {
        id_room = area->capacity - area->used;
    }
    id_room &= ~3u;
    unsigned char *id_bytes = area->bytes + area->used;
    area->used += id_room;
    uint32_t names_start = area->used;
    uint32_t used = 0;
    uint32_t taken = 0;
    bool cut = false;
    for (uint32_t list = 0; list < TA_TRACE_LIST_COUNT && !cut; list += 1u) {
        for (uint32_t index = 0; lists[list] != NULL && index < sizes[list]; index += 1u) {
            uint32_t id = lists[list][index];
            uint32_t mark = area->used;
            if (area->capacity - area->used < (uint32_t)sizeof(uint32_t)) {
                cut = true;
                break;
            }
            ta_area_put_u32(area, 0u);
            ta_text_writer writer = ta_area_writer(area);
            if (list == TA_TRACE_LIST_TRANSITIONS) {
                ta_trace_put_transition(&names, id, &writer);
            } else {
                ta_trace_put_state(&names, id, &writer);
            }
            uint32_t need = 8u + writer.length;
            if ((taken + 1u) * 4u > id_room || used + need > budget || writer.length > writer.capacity) {
                area->used = mark;
                cut = true;
                break;
            }
            ta_mem_copy(area->bytes + mark, &writer.length, sizeof writer.length);
            area->used += writer.length;
            ta_mem_copy(id_bytes + taken * 4u, &id, sizeof id);
            taken += 1u;
            counts[list] += 1u;
            used += need;
        }
    }
    *ids = (ta_bytes){ .ptr = taken == 0 ? NULL : id_bytes, .len = taken * 4u };
    *list_names = (ta_bytes){ .ptr = area->used == names_start ? NULL : area->bytes + names_start,
                         .len = area->used - names_start };
    return cut;
}

/* The event type as the catalog numbers it. */
static uint8_t ta_trace_event_type(ta_event_type type) {
    switch (type) {
    case TA_EVENT_INTERNAL: return TA_RECORD_EVENT_TYPE_INTERNAL;
    case TA_EVENT_PLATFORM: return TA_RECORD_EVENT_TYPE_PLATFORM;
    default: return TA_RECORD_EVENT_TYPE_EXTERNAL;
    }
}

/* Whether a kind opens a run, and so carries the definition's facts. */
static bool ta_trace_opens_run(ta_trace_kind kind) {
    return kind == TA_TRACE_SPAWNED || kind == TA_TRACE_LOADED || kind == TA_TRACE_MACROSTEP_BEGIN ||
           kind == TA_TRACE_DEFINITION_REGISTERED || kind == TA_TRACE_DEFINITION_RETIRED ||
           kind == TA_TRACE_DEFINITION_FREED;
}

/* Composes one text with `put` into the area. */
#define TA_TRACE_COMPOSE(area, build, field, statement)           \
    do {                                                          \
        ta_text_writer writer = ta_area_writer(area);             \
        ta_text_writer *out = &writer;                            \
        statement;                                                \
        bool cut = false;                                         \
        (build)->texts.field = ta_area_take((area), out, &cut);   \
        if (cut) {                                                \
            (build)->flags |= TA_TEL_TRACE_FLAG_TRUNCATED;         \
        }                                                         \
    } while (0)

/* What composing a record's texts reads. */
typedef struct {
    ta_system *system;
    const ta_trace_entry *entry;
} ta_trace_compose_params;

/* The record's texts but its lists. */
static void ta_trace_compose(const ta_trace_compose_params *params, ta_trace_area *area, ta_trace_build *build) {
    ta_system *system = params->system;
    const ta_trace_entry *entry = params->entry;
    const ta_definition *definition = entry->definition;
    const ta_trace_names names = { .system = system, .definition = definition, .tracer = &system->trace };
    const ta_actor *actor = entry->actor;
    build->texts.session_id = ta_trace_session_text(system, &entry->key);
    if (entry->cause.session != 0 || entry->cause.macrostep != 0 || entry->cause.microstep != 0) {
        TA_TRACE_COMPOSE(area, build, cause_session_id, ta_trace_put_session(system, &entry->cause, out));
    }
    if (actor != NULL && actor->record.self.kind == TA_ACTOR_REF_NAMED && actor->record.self.as.named != NULL) {
        ta_actor_address address = ta_address_view(actor->record.self.as.named);
        TA_TRACE_COMPOSE(area, build, address, {
            ta_text_put(out, address.family.name);
            ta_text_put(out, TA_TRACE_LIT("/"));
            ta_text_put(out, address.name);
        });
    }
    if (ta_trace_opens_run(entry->kind)) {
        build->texts.family_name = definition->identity.family.name;
        build->texts.revision = definition->identity.revision;
        ta_trace_facts *facts = &system->trace.facts;
        if (facts->definition != definition->id) {
            facts->document_name = definition->document_name != TA_ATOM_NONE
                                       ? ta_atom_text(&system->atoms, definition->document_name)
                                       : (ta_str){ 0 };
            ta_source_location place;
            facts->source = ta_definition_span(system, &(ta_definition_span_params){ .definition = definition->id,
                                                                                      .kind = TA_ELEMENT_STATE,
                                                                                      .id = 0,
                                                                                      .out = &place }) == TA_OK
                                ? place.source
                                : (ta_str){ 0 };
            facts->definition = definition->id;
        }
        build->texts.document_name = facts->document_name;
        build->texts.source = facts->source;
    }
    if (entry->position.state != TA_STATE_NONE) {
        build->texts.state_name = ta_trace_state_name(system, definition, entry->position.state);
    }
    if (entry->position.transition != TA_TRANSITION_NONE) {
        TA_TRACE_COMPOSE(area, build, transition_name,
                         ta_trace_put_transition(&names, entry->position.transition, out));
    }
    if (entry->position.action != TA_ACTION_NONE) {
        TA_TRACE_COMPOSE(area, build, action_name, ta_trace_put_action(&names, entry->position.action, out));
    }
    if (entry->state != TA_STATE_NONE) {
        build->texts.subject_state_name = ta_trace_state_name(system, definition, entry->state);
    }
    if (entry->action != TA_ACTION_NONE) {
        TA_TRACE_COMPOSE(area, build, subject_action_name, ta_trace_put_action(&names, entry->action, out));
    }
    build->texts.name = entry->name;
    if (entry->event != NULL) {
        const ta_event *event = entry->event;
        build->flags |= TA_TEL_TRACE_FLAG_EVENT;
        if (event->name == 0 && entry->event_text.len != 0) {
            TA_TRACE_COMPOSE(area, build, event_name, ta_text_put(out, entry->event_text));
        } else {
            TA_TRACE_COMPOSE(area, build, event_name, (void)ta_event_name_put(system, event, out));
        }
        if (event->send_id != 0) {
            TA_TRACE_COMPOSE(area, build, event_send_id, ta_send_id_write(system, event->send_id, out));
        }
        if (event->invoke_id != TA_INVOKE_ID_NONE) {
            TA_TRACE_COMPOSE(area, build, event_invoke_id, (void)ta_invoke_id_write(system, event->invoke_id, out));
        }
        bool cut = false;
        build->texts.event_data = ta_trace_value_text(&(ta_trace_value_params){
            .system = system, .entry = entry, .value = &event->data, .area = area, .cut = &cut,
            .reason = event->type == TA_EVENT_PLATFORM /* an error event: its data says why */ });
        if (cut) {
            build->flags |= TA_TEL_TRACE_FLAG_EVENT_DATA_CUT | TA_TEL_TRACE_FLAG_TRUNCATED;
        }
    }
    if (entry->send != 0) {
        TA_TRACE_COMPOSE(area, build, send_id, ta_send_id_write(system, entry->send, out));
    }
    if (entry->invoke != TA_INVOKE_ID_NONE) {
        TA_TRACE_COMPOSE(area, build, invoke_id, (void)ta_invoke_id_write(system, entry->invoke, out));
    }
    if (entry->has_value) {
        build->flags |= TA_TEL_TRACE_FLAG_VALUE;
        bool cut = false;
        build->texts.value_text = ta_trace_value_text(&(ta_trace_value_params){
            .system = system, .entry = entry, .value = &entry->value, .area = area, .cut = &cut });
        if (cut) {
            build->flags |= TA_TEL_TRACE_FLAG_VALUE_CUT | TA_TEL_TRACE_FLAG_TRUNCATED;
        }
    }
    if (entry->has_fault) {
        build->flags |= TA_TEL_TRACE_FLAG_FAULT;
        if (entry->fault.detail != 0) {
            TA_TRACE_COMPOSE(area, build, fault_detail, ta_trace_put_event_name(system, entry->fault.detail, out));
        }
        build->texts.fault_message = entry->fault.message;
        bool cut = false;
        build->texts.fault_data = ta_trace_value_text(&(ta_trace_value_params){
            .system = system, .entry = entry, .value = &entry->fault.data, .area = area, .cut = &cut,
            .reason = true });
        if (cut) {
            build->flags |= TA_TEL_TRACE_FLAG_TRUNCATED;
        }
    }
}

/* The texts but the lists, in the order they are cut when a record does not fit. */
static uint32_t ta_trace_fit(ta_trace_build *build, uint32_t budget) {
    ta_tel_trace_texts *texts = &build->texts;
    ta_str *order[] = {
        &texts->value_text, &texts->event_data, &texts->fault_data, &texts->fault_message, &texts->source,
        &texts->document_name, &texts->name, &texts->invoke_id, &texts->send_id, &texts->event_invoke_id,
        &texts->event_send_id, &texts->event_name, &texts->fault_detail, &texts->subject_action_name,
        &texts->subject_state_name, &texts->action_name, &texts->transition_name, &texts->state_name,
        &texts->family_name, &texts->address, &texts->cause_session_id, &texts->session_id,
    };
    uint32_t sum = texts->revision.len;
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(order); index += 1u) {
        sum += order[index]->len;
    }
    if (sum > budget && texts->revision.len != 0) {
        /* The revision is bytes, cut with the definition's other facts. */
        uint32_t excess = sum - budget;
        uint32_t taken = excess >= texts->revision.len ? texts->revision.len : excess;
        texts->revision.len -= taken;
        sum -= taken;
        build->flags |= TA_TEL_TRACE_FLAG_TRUNCATED;
    }
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(order) && sum > budget; index += 1u) {
        uint32_t taken = ta_trace_shorten(order[index], sum - budget, false);
        if (taken != 0) {
            sum -= taken;
            build->flags |= TA_TEL_TRACE_FLAG_TRUNCATED;
            if (order[index] == &texts->value_text) {
                build->flags |= TA_TEL_TRACE_FLAG_VALUE_CUT;
            } else if (order[index] == &texts->event_data) {
                build->flags |= TA_TEL_TRACE_FLAG_EVENT_DATA_CUT;
            }
        }
    }
    return sum;
}

/* Reports the records dropped since the last report, ahead of the next record. */
static void ta_trace_report_drops(ta_system *system, ta_time now) {
    ta_ring *ring = &system->control.tel;
    ta_ring_record reserved;
    /* Reserve first, then take the drops: a refused report is counted with them. */
    if (ta_ring_reserve(ring, (uint32_t)sizeof(ta_tel_dropped), &reserved) != TA_OK) {
        return;
    }
    ta_ring_drops drops = { 0 };
    (void)ta_ring_take_drops(ring, &drops);
    ta_tel_dropped fixed = { .header = { .at = now }, .count = drops.count, .first_seq = drops.first_seq };
    (void)ta_tel_dropped_write((ta_record_buffer){ .bytes = reserved.bytes, .size = reserved.size }, &fixed);
    ta_ring_commit(ring);
}

void ta_trace_write(ta_system *system, const ta_trace_entry *entry) {
    ta_tracer *tracer = &system->trace;
    ta_ring *ring = &system->control.tel;
    if (tracer->scratch == NULL) {
        return;
    }
    if (ring->unreported.count != 0) {
        ta_trace_report_drops(system, entry->now);
    }
    ta_ring_record reserved;
    if (ta_ring_available(ring) < TA_TRACE_FIXED_BYTES) {
        /* Full whatever the texts: drop it, counted, before rendering anything. */
        (void)ta_ring_reserve(ring, TA_TRACE_FIXED_BYTES, &reserved);
        return;
    }
    ta_trace_area area = { .bytes = tracer->scratch, .capacity = tracer->scratch_bytes };
    ta_trace_build build = { 0 };
    ta_trace_compose(&(ta_trace_compose_params){ .system = system, .entry = entry }, &area, &build);
    uint32_t budget = ring->max_record_bytes - TA_TRACE_FIXED_BYTES;
    uint32_t used = ta_trace_fit(&build, budget);
    if (entry->definition != NULL) {
        bool cut = ta_trace_lists(&area, &(ta_trace_lists_params){ .system = system, .entry = entry,
                                                                   .budget = budget - used },
                                  &build);
        if (cut) {
            build.flags |= TA_TEL_TRACE_FLAG_LISTS_CUT | TA_TEL_TRACE_FLAG_TRUNCATED;
        }
    }
    /* Measured once: the fixed part, the texts ta_trace_fit summed, and the lists. */
    uint32_t size = TA_ALIGN_UP(TA_TRACE_FIXED_BYTES + used + build.texts.list_ids.len + build.texts.list_names.len, 8u);
    if (ta_ring_reserve(ring, size, &reserved) != TA_OK) {
        return; /* dropped and counted (ADR 0098) */
    }
    /* The fixed part is built in the reservation, around the seq it stamped. */
    ta_tel_trace *fixed = (ta_tel_trace *)(void *)reserved.bytes;
    ta_mem_zero(reserved.bytes + offsetof(ta_record_header, session),
                TA_TRACE_FIXED_BYTES - offsetof(ta_record_header, session));
    fixed->header.flags = build.flags;
    fixed->header.session = entry->key.session;
    fixed->header.at = entry->now;
    fixed->wall_ns = tracer->clock.v != NULL ? tracer->clock.v->wall_ns(tracer->clock.self) : 0u;
    fixed->macrostep = entry->key.macrostep;
    fixed->trace_id_high = entry->context.trace_id_high;
    fixed->trace_id_low = entry->context.trace_id_low;
    fixed->cause_session = entry->cause.session;
    fixed->cause_macrostep = entry->cause.macrostep;
    fixed->actor = entry->actor != NULL ? entry->actor->record.id : 0u;
    fixed->definition = entry->definition != NULL ? entry->definition->id : 0u;
    fixed->send = entry->send;
    fixed->invoke = entry->invoke;
    fixed->microstep = entry->key.microstep;
    fixed->cause_microstep = entry->cause.microstep;
    fixed->state = entry->position.state;
    fixed->transition = entry->position.transition;
    fixed->action = entry->position.action;
    fixed->iteration = entry->position.iteration;
    fixed->subject_state = entry->state;
    fixed->subject_action = entry->action;
    fixed->expr = entry->expr;
    fixed->dropped = entry->dropped;
    fixed->fault_code = entry->has_fault ? entry->fault.code : 0;
    fixed->transition_count = build.counts[TA_TRACE_LIST_TRANSITIONS];
    fixed->exited_count = build.counts[TA_TRACE_LIST_EXITED];
    fixed->entered_count = build.counts[TA_TRACE_LIST_ENTERED];
    fixed->history_count = build.counts[TA_TRACE_LIST_HISTORY];
    fixed->state_count = build.counts[TA_TRACE_LIST_STATES];
    fixed->trace_kind = (uint16_t)entry->kind;
    fixed->position = (uint8_t)entry->position.at;
    fixed->result = entry->result ? 1u : 0u;
    fixed->trace_flags = entry->context.flags;
    fixed->trace_detail = entry->context.detail;
    fixed->runaway_cause = entry->kind == TA_TRACE_RUNAWAY ? (uint8_t)TA_RECORD_RUNAWAY_CAUSE_CHAIN_LIMIT : 0u;
    fixed->event_type = entry->event != NULL ? ta_trace_event_type(entry->event->type) : 0u;
    (void)ta_tel_trace_write((ta_record_buffer){ .bytes = reserved.bytes, .size = reserved.size }, fixed,
                             &build.texts);
    ta_ring_commit(ring);
}

/* ------------------------------------------------------------------------
 * Reading tel (ADR 0098, ADR 0104): the host's side of the ring
 */

static ta_event_type ta_trace_public_event_type(uint8_t type) {
    switch (type) {
    case TA_RECORD_EVENT_TYPE_INTERNAL: return TA_EVENT_INTERNAL;
    case TA_RECORD_EVENT_TYPE_PLATFORM: return TA_EVENT_PLATFORM;
    default: return TA_EVENT_EXTERNAL;
    }
}

/* A trace record's IDs, numbers, flags and lists, read from its checked view
 * into `out`; its texts stay in the record, for ta_trace_record_text. */
static ta_result ta_trace_decode(const ta_record_view *view, ta_trace_record *out) {
    const ta_tel_trace *fixed = ta_tel_trace_from_view(view);
    uint16_t flags = fixed->header.flags;
    uint64_t total = (uint64_t)fixed->transition_count + fixed->exited_count + fixed->entered_count +
                     fixed->history_count + fixed->state_count;
    ta_bytes ids = ta_record_view_bytes(view, fixed->list_ids);
    if (total * 4u != ids.len || ((uintptr_t)ids.ptr & 3u) != 0) {
        return TA_ERR_CORRUPT;
    }
    const uint32_t *list = (const uint32_t *)(const void *)ids.ptr;
    out->kind = (ta_trace_kind)fixed->trace_kind;
    out->key = (ta_step_key){ .session = fixed->header.session, .macrostep = fixed->macrostep,
                              .microstep = fixed->microstep };
    out->position = (ta_trace_position){ .at = (ta_trace_at)fixed->position, .state = fixed->state,
                                         .transition = fixed->transition, .action = fixed->action,
                                         .iteration = fixed->iteration };
    out->context = (ta_trace_context){ .trace_id_high = fixed->trace_id_high, .trace_id_low = fixed->trace_id_low,
                                       .flags = fixed->trace_flags, .detail = fixed->trace_detail };
    out->cause = (ta_step_key){ .session = fixed->cause_session, .macrostep = fixed->cause_macrostep,
                                .microstep = fixed->cause_microstep };
    out->actor = fixed->actor;
    out->definition = fixed->definition;
    out->now = fixed->header.at;
    out->wall_ns = fixed->wall_ns;
    out->has_event = (flags & TA_TEL_TRACE_FLAG_EVENT) != 0;
    out->event_type = ta_trace_public_event_type(fixed->event_type);
    out->event_data_cut = (flags & TA_TEL_TRACE_FLAG_EVENT_DATA_CUT) != 0;
    out->state = fixed->subject_state;
    out->expr = fixed->expr;
    out->action = fixed->subject_action;
    out->iteration = fixed->trace_kind == (uint16_t)TA_TRACE_FOREACH ? fixed->iteration : 0u;
    out->send = fixed->send;
    out->invoke = fixed->invoke;
    out->result = fixed->result != 0;
    out->has_value = (flags & TA_TEL_TRACE_FLAG_VALUE) != 0;
    out->value_cut = (flags & TA_TEL_TRACE_FLAG_VALUE_CUT) != 0;
    out->has_fault = (flags & TA_TEL_TRACE_FLAG_FAULT) != 0;
    out->fault_code = fixed->fault_code;
    out->dropped = fixed->dropped;
    out->runaway_cause = fixed->runaway_cause;
    out->suppressed = (flags & TA_TEL_TRACE_FLAG_SUPPRESSED) != 0;
    out->truncated = (flags & TA_TEL_TRACE_FLAG_TRUNCATED) != 0;
    out->lists_cut = (flags & TA_TEL_TRACE_FLAG_LISTS_CUT) != 0;
    out->transition_count = fixed->transition_count;
    out->transitions = fixed->transition_count != 0 ? list : NULL;
    list += fixed->transition_count;
    out->exited_count = fixed->exited_count;
    out->exited = fixed->exited_count != 0 ? list : NULL;
    list += fixed->exited_count;
    out->entered_count = fixed->entered_count;
    out->entered = fixed->entered_count != 0 ? list : NULL;
    list += fixed->entered_count;
    out->history_count = fixed->history_count;
    out->history = fixed->history_count != 0 ? list : NULL;
    list += fixed->history_count;
    out->state_count = fixed->state_count;
    out->states = fixed->state_count != 0 ? list : NULL;
    out->list_names = ta_record_view_bytes(view, fixed->list_names);
    out->revision = ta_record_view_bytes(view, fixed->revision);
    out->bytes = (ta_bytes){ .ptr = view->bytes, .len = view->size };
    return TA_OK;
}

/* Where each ta_trace_text's span lies in a tel.trace record, by its value. */
static const uint16_t ta_trace_text_spans[] = {
    [TA_TRACE_TEXT_SESSION_ID] = offsetof(ta_tel_trace, session_id),
    [TA_TRACE_TEXT_CAUSE_SESSION_ID] = offsetof(ta_tel_trace, cause_session_id),
    [TA_TRACE_TEXT_ADDRESS] = offsetof(ta_tel_trace, address),
    [TA_TRACE_TEXT_FAMILY] = offsetof(ta_tel_trace, family_name),
    [TA_TRACE_TEXT_DOCUMENT_NAME] = offsetof(ta_tel_trace, document_name),
    [TA_TRACE_TEXT_SOURCE] = offsetof(ta_tel_trace, source),
    [TA_TRACE_TEXT_POSITION_STATE] = offsetof(ta_tel_trace, state_name),
    [TA_TRACE_TEXT_POSITION_TRANSITION] = offsetof(ta_tel_trace, transition_name),
    [TA_TRACE_TEXT_POSITION_ACTION] = offsetof(ta_tel_trace, action_name),
    [TA_TRACE_TEXT_STATE] = offsetof(ta_tel_trace, subject_state_name),
    [TA_TRACE_TEXT_ACTION] = offsetof(ta_tel_trace, subject_action_name),
    [TA_TRACE_TEXT_NAME] = offsetof(ta_tel_trace, name),
    [TA_TRACE_TEXT_EVENT_NAME] = offsetof(ta_tel_trace, event_name),
    [TA_TRACE_TEXT_EVENT_SEND_ID] = offsetof(ta_tel_trace, event_send_id),
    [TA_TRACE_TEXT_EVENT_INVOKE_ID] = offsetof(ta_tel_trace, event_invoke_id),
    [TA_TRACE_TEXT_EVENT_DATA] = offsetof(ta_tel_trace, event_data),
    [TA_TRACE_TEXT_SEND_ID] = offsetof(ta_tel_trace, send_id),
    [TA_TRACE_TEXT_INVOKE_ID] = offsetof(ta_tel_trace, invoke_id),
    [TA_TRACE_TEXT_VALUE] = offsetof(ta_tel_trace, value_text),
    [TA_TRACE_TEXT_FAULT_DETAIL] = offsetof(ta_tel_trace, fault_detail),
    [TA_TRACE_TEXT_FAULT_MESSAGE] = offsetof(ta_tel_trace, fault_message),
    [TA_TRACE_TEXT_FAULT_DATA] = offsetof(ta_tel_trace, fault_data),
};

_Static_assert(TA_ARRAY_COUNT(ta_trace_text_spans) == (uint32_t)TA_TRACE_TEXT_FAULT_DATA + 1u,
               "every ta_trace_text has its span");

ta_str ta_trace_record_text(const ta_trace_record *record, ta_trace_text text) {
    if (record == NULL || (uint32_t)text >= TA_ARRAY_COUNT(ta_trace_text_spans) ||
        record->bytes.ptr == NULL || record->bytes.len < TA_TRACE_FIXED_BYTES) {
        return (ta_str){ 0 };
    }
    ta_span32 span = { 0 };
    ta_mem_copy(&span, record->bytes.ptr + ta_trace_text_spans[text], sizeof span);
    ta_record_view view = { .bytes = record->bytes.ptr, .size = record->bytes.len, .kind = TA_RECORD_KIND_TEL_TRACE };
    return ta_record_view_text(&view, span);
}

ta_result ta_system_tel_peek(ta_system *system, ta_tel_record *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->trace.scratch == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_ring_record record;
    ta_result result = ta_ring_peek(&system->control.tel, &record);
    if (result != TA_OK) {
        return result;
    }
    ta_record_view view;
    if (ta_record_read(&(ta_record_read_params){ .bytes = record.bytes, .size = record.size,
                                                 .ring = TA_RECORD_RING_TEL },
                       &view) != TA_OK) {
        return TA_ERR_CORRUPT;
    }
    const ta_tel_dropped *dropped = ta_tel_dropped_from_view(&view);
    if (dropped != NULL) {
        *out = (ta_tel_record){ .kind = TA_TEL_DROPPED, .seq = record.seq, .dropped = dropped->count,
                                .first_dropped = dropped->first_seq };
        return TA_OK;
    }
    if (ta_trace_decode(&view, &out->trace) != TA_OK) {
        return TA_ERR_CORRUPT;
    }
    out->kind = TA_TEL_TRACE;
    out->seq = record.seq;
    out->dropped = 0;
    out->first_dropped = 0;
    return TA_OK;
}

ta_result ta_system_tel_release(ta_system *system) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->trace.scratch == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_ring_release(&system->control.tel);
}

ta_result ta_system_tel_status(const ta_system *system, ta_tel_status *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_ring *ring = &system->control.tel;
    if (system->trace.scratch == NULL) {
        *out = (ta_tel_status){ 0 };
        return TA_OK;
    }
    *out = (ta_tel_status){ .capacity = ta_ring_capacity(ring), .used = ta_ring_used(ring),
                            .next_seq = ring->next_seq, .dropped = ring->dropped };
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * traceparent text (ADR 0073 section 1)
 */

#define TA_TRACEPARENT_LEN 55u

/*
 * A lowercase hex digit's value plus one, and 0 for anything else, so the
 * table's default (zero) is "not a digit". A traceparent has 52 digits,
 * read on every post that carries one: a lookup and an AND per digit, and
 * one test per field, keep that cheap (ADR 0073, napi ADR 0017 section 2).
 */
static const uint8_t ta_hex_value[256] = {
    ['0'] = 1, ['1'] = 2, ['2'] = 3, ['3'] = 4, ['4'] = 5, ['5'] = 6, ['6'] = 7, ['7'] = 8, ['8'] = 9, ['9'] = 10,
    ['a'] = 11, ['b'] = 12, ['c'] = 13, ['d'] = 14, ['e'] = 15, ['f'] = 16,
};

/* Reads `count` lowercase hex digits (at most 16) into `*out`, one at a time: the 2-digit fields. */
static bool ta_hex_read(const char *text, uint32_t count, uint64_t *out) {
    uint64_t value = 0;
    uint32_t valid = 1;
    for (uint32_t index = 0; index < count; index += 1) {
        uint32_t digit = ta_hex_value[(uint8_t)text[index]];
        valid &= digit != 0;
        value = (value << 4) | (uint64_t)((digit - 1u) & 0xfu);
    }
    *out = value;
    return valid != 0;
}

/*
 * Eight lowercase hex digits at once (SWAR): loaded with shifts, so the
 * result does not depend on byte order, validated with byte-wise range
 * tests, and packed into 32 bits. On 16-digit fields this is several times
 * faster than a digit at a time.
 */
static bool ta_hex_read8(const char *text, uint32_t *out) {
    const unsigned char *bytes = (const unsigned char *)text;
    uint64_t x = 0;
    for (uint32_t index = 0; index < 8u; index += 1u) {
        x |= (uint64_t)bytes[index] << (8u * index);
    }
    const uint64_t ones = 0x0101010101010101u;
    const uint64_t highs = 0x8080808080808080u;
    if ((x & highs) != 0) {
        return false; /* not ASCII; also keeps the additions below inside each byte */
    }
    /* Bit 7 of each byte: b >= lo is (b + 0x80 - lo), b > hi is (b + 0x7f - hi). */
    uint64_t digit = (x + ones * 0x50u) & ~(x + ones * 0x46u);  /* '0'..'9' */
    uint64_t alpha = (x + ones * 0x1fu) & ~(x + ones * 0x19u);  /* 'a'..'f' */
    if (((digit | alpha) & highs) != highs) {
        return false;
    }
    /* A digit's value is its low nibble, plus 9 for a letter (bit 6 set). */
    uint64_t v = (x & (ones * 0x0fu)) + ((x >> 6) & ones) * 9u;
    /* The first character is in the lowest byte and is the most significant nibble. */
    v = ((v << 4) | (v >> 8)) & 0x00ff00ff00ff00ffu;
    v = ((v << 8) | (v >> 16)) & 0x0000ffff0000ffffu;
    v = ((v << 16) | (v >> 32)) & 0x00000000ffffffffu;
    *out = (uint32_t)v;
    return true;
}

/* Reads 16 lowercase hex digits into `*out`. */
static bool ta_hex_read16(const char *text, uint64_t *out) {
    uint32_t high = 0;
    uint32_t low = 0;
    bool high_valid = ta_hex_read8(text, &high);
    bool low_valid = ta_hex_read8(text + 8, &low);
    *out = ((uint64_t)high << 32) | low;
    return high_valid && low_valid;
}

static bool ta_trace_space(char c) {
    return c == ' ' || c == '\t';
}

ta_result ta_traceparent_parse(const ta_traceparent_parse_params *params) {
    if (params == NULL || params->out == NULL || (params->text.len != 0 && params->text.ptr == NULL)) {
        return TA_ERR_INVALID;
    }
    const char *text = params->text.ptr;
    uint32_t len = params->text.len;
    while (len != 0 && ta_trace_space(text[0])) {
        text += 1;
        len -= 1;
    }
    while (len != 0 && ta_trace_space(text[len - 1u])) {
        len -= 1;
    }
    uint64_t version = 0;
    uint64_t high = 0;
    uint64_t low = 0;
    uint64_t span = 0;
    uint64_t flags = 0;
    if (len < TA_TRACEPARENT_LEN || !ta_hex_read(text, 2u, &version) || version == 0xffu || text[2] != '-' ||
        text[35] != '-' || text[52] != '-' || !ta_hex_read16(text + 3, &high) ||
        !ta_hex_read16(text + 19, &low) || !ta_hex_read16(text + 36, &span) ||
        !ta_hex_read(text + 53, 2u, &flags)) {
        return TA_ERR_INVALID;
    }
    /* Version 00 is exactly 55 characters; a later one may carry more after a dash. */
    if (len != TA_TRACEPARENT_LEN && (version == 0 || text[TA_TRACEPARENT_LEN] != '-')) {
        return TA_ERR_INVALID;
    }
    if ((high == 0 && low == 0) || span == 0) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_trace_parent){
        .context = { .trace_id_high = high, .trace_id_low = low, .flags = (uint8_t)flags },
        .cause = { .session = 0, .macrostep = span, .microstep = 0 },
    };
    return TA_OK;
}

/* Writes `len` characters and a NUL through the buffer convention. */
static ta_result ta_trace_text_begin(char *buffer, uint32_t capacity, uint32_t *needed, uint32_t len) {
    if (needed == NULL) {
        return TA_ERR_INVALID;
    }
    *needed = len + 1u;
    if (buffer == NULL || capacity < len + 1u) {
        return TA_ERR_NOMEM;
    }
    buffer[len] = '\0';
    return TA_OK;
}

ta_result ta_traceparent_format(const ta_traceparent_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t span = ta_step_key_span_id(params->parent.cause);
    if (ta_trace_context_empty(&params->parent.context) || span == 0) {
        if (params->needed != NULL) {
            *params->needed = 0;
        }
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, TA_TRACEPARENT_LEN);
    if (result != TA_OK) {
        return result;
    }
    char *out = params->buffer;
    out[0] = '0';
    out[1] = '0';
    out[2] = '-';
    ta_text_format_hex(out + 3, params->parent.context.trace_id_high, 16u);
    ta_text_format_hex(out + 19, params->parent.context.trace_id_low, 16u);
    out[35] = '-';
    ta_text_format_hex(out + 36, span, 16u);
    out[52] = '-';
    ta_text_format_hex(out + 53, params->parent.context.flags, 2u);
    return TA_OK;
}

ta_result ta_trace_id_format(const ta_trace_id_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, 32u);
    if (result == TA_OK) {
        ta_text_format_hex(params->buffer, params->context.trace_id_high, 16u);
        ta_text_format_hex(params->buffer + 16, params->context.trace_id_low, 16u);
    }
    return result;
}

ta_result ta_span_id_format(const ta_span_id_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, 16u);
    if (result == TA_OK) {
        ta_text_format_hex(params->buffer, params->span_id, 16u);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Sampling (ADR 0073 section 3)
 */

#define TA_TRACE_RANDOM_BITS ((uint64_t)1u << 56)

uint64_t ta_trace_ratio_threshold(uint32_t parts, uint32_t whole) {
    if (whole == 0 || parts == 0) {
        return TA_TRACE_RANDOM_BITS; /* nothing is sampled */
    }
    if (parts >= whole) {
        return 0;
    }
    /* round(rejected * 2^56 / whole), in 64 bits: 2^56 = q * whole + r. */
    uint64_t rejected = (uint64_t)(whole - parts);
    uint64_t q = TA_TRACE_RANDOM_BITS / whole;
    uint64_t r = TA_TRACE_RANDOM_BITS % whole;
    return rejected * q + (rejected * r + whole / 2u) / whole;
}

bool ta_trace_sampled_at(const ta_trace_context *context, uint64_t threshold) {
    return context != NULL && (context->trace_id_low & (TA_TRACE_RANDOM_BITS - 1u)) >= threshold;
}

void ta_system_set_trace_ratio(ta_system *system, uint32_t parts, uint32_t whole) {
    if (system != NULL) {
        system->trace_threshold = ta_trace_ratio_threshold(parts, whole);
    }
}

/* ------------------------------------------------------------------------
 * The generator (ADR 0073 section 2): xoshiro256**, seeded through SplitMix64
 */

static uint64_t ta_splitmix64(uint64_t *state) {
    *state += 0x9e3779b97f4a7c15ull;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t ta_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t ta_trace_next(ta_system *system) {
    uint64_t *s = system->trace_rng;
    uint64_t result = ta_rotl(s[1] * 5u, 7) * 9u;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = ta_rotl(s[3], 45);
    return result;
}

void ta_system_seed_trace_ids(ta_system *system, uint64_t seed) {
    if (system == NULL) {
        return;
    }
    uint64_t state = seed;
    for (uint32_t index = 0; index < 4u; index += 1) {
        system->trace_rng[index] = ta_splitmix64(&state);
    }
}

uint64_t ta_system_new_span_id(ta_system *system) {
    if (system == NULL) {
        return 0;
    }
    uint64_t span = 0;
    while (span == 0) {
        span = ta_trace_next(system);
    }
    return span;
}

ta_trace_context ta_system_new_trace(ta_system *system) {
    if (system == NULL) {
        return (ta_trace_context){0};
    }
    ta_trace_context context = { .flags = TA_TRACE_FLAG_RANDOM };
    while (context.trace_id_high == 0 && context.trace_id_low == 0) {
        context.trace_id_high = ta_trace_next(system);
        context.trace_id_low = ta_trace_next(system);
    }
    if (ta_trace_sampled_at(&context, system->trace_threshold)) {
        context.flags |= TA_TRACE_FLAG_SAMPLED;
    }
    return context;
}

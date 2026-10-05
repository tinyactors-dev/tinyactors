#include "internal.h"

/*
 * The implementation of package tel (see tel.h for the contract, the
 * lifetimes, the drop protocol and the failure rules). Three parts: the
 * staging of the texts a record needs rendered, the writer, and the public
 * reading side (masks, ta_tel_u32_at, ta_tel_decode). Nothing here
 * allocates: every buffer is the staging area system.c made at creation, a
 * stack value, or the ring.
 */

/* ------------------------------------------------------------------------
 * Staging
 */

/*
 * A cursor over the system's staging area, for one record (lifetime: call).
 * Texts are appended and never freed individually; the next record starts a
 * new cursor at the beginning. `used` never passes `capacity`.
 */
typedef struct tel_stage {
    unsigned char *base;
    uint32_t capacity;
    uint32_t used;
} tel_stage;

static uint32_t tel_stage_room(const tel_stage *stage) { return stage->capacity - stage->used; }

/*
 * tel_stage_event_name formats an event name's dotted text straight into the
 * staging area. False when it does not fit (the record is then lost: a name
 * is never cut); an unknown name is empty text, not a failure.
 */
static bool tel_stage_event_name(const ta_system *system, ta_event_name_id name, tel_stage *stage, ta_bytes *out) {
    uint32_t needed = 0;
    ta_result result = ta_event_name_format(system, &(ta_event_name_format_params){
                                                        .name = name,
                                                        .buffer = (char *)(stage->base + stage->used),
                                                        .capacity = tel_stage_room(stage),
                                                        .needed = &needed });
    *out = (ta_bytes){ 0 };
    if (result == TA_OK) {
        *out = (ta_bytes){ stage->base + stage->used, needed - 1u }; /* the terminating NUL is not kept */
        stage->used += needed - 1u;
    }
    return result != TA_ERR_NOMEM;
}

/* tel_stage_send_id formats a send ID's text into the staging area; empty for no ID, false when it does not fit. */
static bool tel_stage_send_id(const ta_system *system, ta_send_id send, tel_stage *stage, ta_bytes *out) {
    *out = (ta_bytes){ 0 };
    if (send == 0) {
        return true;
    }
    uint32_t needed = 0;
    ta_result result = ta_send_id_format(system, &(ta_send_id_format_params){
                                                     .send_id = send,
                                                     .buffer = (char *)(stage->base + stage->used),
                                                     .capacity = tel_stage_room(stage),
                                                     .needed = &needed });
    if (result == TA_OK) {
        *out = (ta_bytes){ stage->base + stage->used, needed - 1u };
        stage->used += needed - 1u;
    }
    return result != TA_ERR_NOMEM;
}

/* tel_stage_value_params are the arguments of tel_stage_value; they only live for the call. */
typedef struct {
    const ta_tel_values *values; /* the data model to ask; may be NULL */
    const ta_value *value;       /* the value to render */
    tel_stage *stage;            /* where the text is copied */
    uint32_t share;              /* how many value texts, this one included, still need staging */
    ta_bytes *out;               /* receives the staged text */
    uint16_t *flags;             /* receives `cut_flag` when the text was cut */
    uint16_t cut_flag;
} tel_stage_value_params;

/*
 * tel_stage_value asks the data model for `value`'s text and copies it into
 * the staging area at once, since the data model's memory is only good until
 * its next call. The limit is telemetryValueBytes or `1/share` of the room
 * staging has left, whichever is smaller (`share` is how many value texts,
 * this one included, still have to be staged, so the first cannot take the
 * room the others need); a text the data model returns longer than the
 * limit is cut here, too. `cut_flag` is set in *flags when anything was cut. False, with *out
 * empty, when the value cannot be rendered: no data model running, a data
 * model without the op, a null value, a zero limit, or the op failing. A
 * failing op's fault is released and forgotten: telemetry never fails a step.
 */
static bool tel_stage_value(const ta_system *system, const tel_stage_value_params *params) {
    const ta_tel_values *values = params->values;
    const ta_value *value = params->value;
    tel_stage *stage = params->stage;
    uint32_t share = params->share;
    ta_bytes *out = params->out;
    uint16_t *flags = params->flags;
    uint16_t cut_flag = params->cut_flag;
    *out = (ta_bytes){ 0 };
    if (values == NULL || values->datamodel == NULL || values->datamodel->v == NULL ||
        values->datamodel->v->value_trace_text == NULL || value->v == NULL) {
        return false;
    }
    uint32_t limit = system->params[TA_PARAM_TELEMETRY_VALUE_BYTES];
    uint32_t room = tel_stage_room(stage) / (share == 0 ? 1u : share);
    if (limit > room) {
        limit = room;
    }
    if (limit == 0) {
        if (system->params[TA_PARAM_TELEMETRY_VALUE_BYTES] != 0) {
            *flags |= cut_flag; /* there was a value and no room left for any of it */
        }
        return false;
    }
    ta_str text = { 0 };
    bool text_cut = false;
    ta_fault fault = { 0 };
    ta_op_status status = values->datamodel->v->value_trace_text(
        values->datamodel->self, &(ta_datamodel_value_trace_text_params){ .shared = values->shared,
                                                                          .instance = values->instance,
                                                                          .value = value,
                                                                          .host = values->host,
                                                                          .limit = limit,
                                                                          .out = &text,
                                                                          .cut = &text_cut,
                                                                          .fault = &fault });
    if (status != TA_OP_OK || (text.ptr == NULL && text.len != 0)) {
        ta_value_release(&fault.data);
        return false;
    }
    uint32_t length = text.len;
    if (length > limit) {
        length = limit; /* the data model must keep to the limit; if it did not, cut it here */
        text_cut = true;
    }
    ta_mem_copy(stage->base + stage->used, text.ptr, length);
    *out = (ta_bytes){ stage->base + stage->used, length };
    stage->used += length;
    if (text_cut) {
        *flags |= cut_flag;
    }
    return true;
}

/* tel_starts_with says whether `text` begins with the NUL-terminated `prefix`. */
static bool tel_starts_with(ta_bytes text, const char *prefix) {
    uint32_t at = 0;
    for (; prefix[at] != '\0'; at += 1u) {
        if (at >= text.len || text.ptr[at] != (uint8_t)prefix[at]) {
            return false;
        }
    }
    return true;
}

/* tel_u32s views a list of 32-bit IDs as a span of bytes; false when its byte length overflows a u32. */
static bool tel_u32s(const uint32_t *list, uint32_t count, ta_bytes *out) {
    *out = (ta_bytes){ 0 };
    if (count == 0 || list == NULL) {
        return true;
    }
    if (count > UINT32_MAX / (uint32_t)sizeof(uint32_t)) {
        return false;
    }
    *out = (ta_bytes){ (const uint8_t *)list, count * (uint32_t)sizeof(uint32_t) };
    return true;
}

/* tel_ta_bytes views a ta_str as bytes. */
static ta_bytes tel_ta_bytes(ta_str text) { return (ta_bytes){ (const uint8_t *)text.ptr, text.len }; }

/* ------------------------------------------------------------------------
 * Building a record
 */

/* tel_fit_params are the arguments of tel_fit; they only live for the call. */
typedef struct {
    uint32_t max;    /* maxRecordBytes */
    uint32_t *size;  /* receives the final size */
    uint16_t *flags; /* receives the truncated bits */
} tel_fit_params;

/*
 * tel_fit makes the record fit maxRecordBytes by shortening the texts that
 * are values, in the order value, event data, fault data, each cut on a
 * UTF-8 character boundary, and sets the matching truncated bit in `*flags`
 * for each it cut. The names and
 * IDs are never cut. *size is the final size either way; false when the
 * record is still over `max` after every value is gone, or cannot be
 * measured.
 */
static bool tel_fit(ta_msg_tel_trace *data, const tel_fit_params *params) {
    uint32_t max = params->max;
    uint32_t *size = params->size;
    uint16_t *flags = params->flags;
    ta_bytes *values[] = { &data->value, &data->event_data, &data->fault_data };
    static const uint16_t cut_flags[] = { TA_MSG_TEL_TRACE_TRUNCATED, TA_MSG_TEL_TRACE_EVENT_DATA_TRUNCATED,
                                          TA_MSG_TEL_TRACE_FAULT_DATA_TRUNCATED };
    if (ta_msg_tel_trace_size(data, size) != TA_OK) {
        return false;
    }
    for (uint32_t index = 0; index < 3u && *size > max; index += 1u) {
        ta_bytes *text = values[index];
        if (text->len == 0) {
            continue;
        }
        uint32_t excess = *size - max;
        uint32_t keep = text->len > excess ? text->len - excess : 0;
        while (keep > 0 && (text->ptr[keep] & 0xC0u) == 0x80u) {
            keep -= 1u; /* not in the middle of a character */
        }
        text->len = keep;
        if (keep == 0) {
            text->ptr = NULL;
        }
        *flags |= cut_flags[index];
        if (ta_msg_tel_trace_size(data, size) != TA_OK) {
            return false;
        }
    }
    return *size <= max;
}

/* tel_build_params are the arguments of tel_build; they only live for the call. */
typedef struct {
    const ta_trace_record *record;
    const ta_tel_values *values; /* may be NULL */
    tel_stage *stage;
    ta_msg_tel_trace *data; /* receives the built record */
    uint32_t *size;         /* receives its size */
} tel_build_params;

/*
 * tel_build fills `data` for `record`: the scalars, the borrowed texts and
 * lists, and the texts it renders into `stage`, and measures it. False when
 * the record cannot be written: a name that does not fit staging, a list too
 * long to count, or a record that stays over maxRecordBytes with its values
 * cut away. `data` borrows from `record` and `stage` for the call.
 */
static bool tel_build(const ta_system *system, const tel_build_params *params) {
    const ta_trace_record *record = params->record;
    const ta_tel_values *values = params->values;
    tel_stage *stage = params->stage;
    ta_msg_tel_trace *data = params->data;
    uint32_t *size = params->size;
    uint16_t flags = 0;
    ta_msg_tel_trace built = { 0 };
    const ta_event *event = record->event;
    ta_bytes event_name = { 0 };
    /*
     * First the texts that are never cut (names and IDs), so that the values,
     * which are cut, only get what they leave of the staging area.
     */
    if (event != NULL) {
        flags |= TA_MSG_TEL_TRACE_HAS_EVENT;
        built.event_type = (uint8_t)event->type;
        if (!tel_stage_event_name(system, event->name, stage, &event_name) ||
            !tel_stage_send_id(system, event->send_id, stage, &built.event_send_id)) {
            return false;
        }
        built.event_name = event_name;
        ta_str invoke = { 0 };
        if (event->invoke_id != TA_INVOKE_ID_NONE &&
            ta_system_invoke_id_text(system, event->invoke_id, &invoke) == TA_OK) {
            built.event_invoke_id = tel_ta_bytes(invoke); /* borrowed from the system's atoms: system lifetime */
        }
    }
    if (record->has_fault && record->fault.detail != 0 &&
        !tel_stage_event_name(system, record->fault.detail, stage, &built.fault_detail)) {
        return false;
    }
    if (!tel_stage_send_id(system, record->send, stage, &built.send)) {
        return false;
    }
    /* Then the values, each with an equal share of what is left. */
    bool event_data = event != NULL && (system->trace_masks.values ||
                                        (record->kind == TA_TRACE_RAISED && tel_starts_with(event_name, "error.")));
    bool record_value = record->has_value && system->trace_masks.values;
    uint32_t share = (event_data ? 1u : 0u) + (record_value ? 1u : 0u) + (record->has_fault ? 1u : 0u);
    /* An event's data is rendered when values are asked for, and always for an error: the reason is what an operator needs. */
    if (event_data) {
        (void)tel_stage_value(system, &(tel_stage_value_params){ .values = values,
                                                                 .value = &event->data,
                                                                 .stage = stage,
                                                                 .share = share,
                                                                 .out = &built.event_data,
                                                                 .flags = &flags,
                                                                 .cut_flag = TA_MSG_TEL_TRACE_EVENT_DATA_TRUNCATED });
        share -= 1u;
    }
    if (record_value) {
        if (tel_stage_value(system, &(tel_stage_value_params){ .values = values,
                                                               .value = &record->value,
                                                               .stage = stage,
                                                               .share = share,
                                                               .out = &built.value,
                                                               .flags = &flags,
                                                               .cut_flag = TA_MSG_TEL_TRACE_TRUNCATED })) {
            flags |= TA_MSG_TEL_TRACE_HAS_VALUE;
        }
        share -= 1u;
    }
    if (record->has_fault) {
        flags |= TA_MSG_TEL_TRACE_HAS_FAULT;
        built.fault_code = (uint32_t)record->fault.code;
        built.fault_message = tel_ta_bytes(record->fault.message);
        (void)tel_stage_value(system, &(tel_stage_value_params){ .values = values,
                                                                 .value = &record->fault.data,
                                                                 .stage = stage,
                                                                 .share = share,
                                                                 .out = &built.fault_data,
                                                                 .flags = &flags,
                                                                 .cut_flag = TA_MSG_TEL_TRACE_FAULT_DATA_TRUNCATED });
    }
    ta_str invoke = { 0 };
    if (record->invoke != TA_INVOKE_ID_NONE && ta_system_invoke_id_text(system, record->invoke, &invoke) == TA_OK) {
        built.invoke = tel_ta_bytes(invoke);
    }
    if (!tel_u32s(record->transitions, record->transition_count, &built.transitions) ||
        !tel_u32s(record->exited, record->exited_count, &built.exited) ||
        !tel_u32s(record->entered, record->entered_count, &built.entered) ||
        !tel_u32s(record->history, record->history_count, &built.history) ||
        !tel_u32s(record->states, record->state_count, &built.states)) {
        return false;
    }
    built.name = tel_ta_bytes(record->name);
    built.identity_family = tel_ta_bytes(record->identity.family.name);
    built.identity_revision = record->identity.revision;

    built.macrostep = record->key.macrostep;
    built.trace_id_high = record->context.trace_id_high;
    built.trace_id_low = record->context.trace_id_low;
    built.cause_session = record->cause.session;
    built.cause_macrostep = record->cause.macrostep;
    built.actor = record->actor;
    built.definition = record->definition;
    built.microstep = record->key.microstep;
    built.family = record->key.family;
    built.cause_microstep = record->cause.microstep;
    built.cause_family = record->cause.family;
    built.at_state = record->position.state;
    built.at_transition = record->position.transition;
    built.at_action = record->position.action;
    built.at_iteration = record->position.iteration;
    built.state = record->state;
    built.expr = record->expr;
    built.action = record->action;
    built.iteration = record->iteration;
    built.dropped = record->dropped;
    built.trace_kind = (uint16_t)record->kind;
    built.position = (uint8_t)record->position.at;
    built.result = record->result ? 1u : 0u;
    built.context_flags = record->context.flags;
    built.context_detail = record->context.detail;

    if (!tel_fit(&built, &(tel_fit_params){ .max = system->rings.ring[TA_RING_TEL].max_record_bytes,
                                            .size = size,
                                            .flags = &flags })) {
        return false;
    }
    built.head = (ta_msg_head){ .flags = flags, .session = record->key.session, .at = record->now };
    *data = built;
    return true;
}

/* ------------------------------------------------------------------------
 * The writer
 */

/*
 * tel_lose counts a record the producer decided not to write: the ring
 * consumes its seq and counts the drop (ta_ring_drop), and the pending count
 * grows, remembering the seq of the oldest loss (invariant T2).
 */
static void tel_lose(ta_system *system, ta_ring *tel) {
    uint64_t seq = ta_ring_drop(tel);
    if (system->tel.pending == 0) {
        system->tel.first_seq = seq;
    }
    system->tel.pending += 1u;
}

/*
 * tel_report_dropped writes the tel.dropped that is due (pending > 0) if the
 * ring has room for it, and clears the pending count; false, with nothing
 * changed, when it does not fit. Whether it fits is asked first
 * (ta_ring_would_fit), so that a report that cannot be written costs no seq
 * and no drop of its own.
 */
static bool tel_report_dropped(ta_system *system, ta_ring *tel) {
    ta_msg_tel_dropped report = { .head = { .at = ta_clock_now(&system->clock) },
                                  .count = system->tel.pending,
                                  .first_seq = system->tel.first_seq };
    uint32_t size = 0;
    ta_ring_reservation reservation;
    if (ta_msg_tel_dropped_size(&report, &size) != TA_OK || !ta_ring_would_fit(tel, size) ||
        ta_ring_reserve(tel, size, &reservation) != TA_RING_OK) {
        return false;
    }
    if (ta_msg_tel_dropped_write(&report, &reservation) != TA_OK) {
        ta_ring_abandon(tel);
        return false;
    }
    ta_ring_commit(tel);
    system->tel.pending = 0;
    return true;
}

void ta_tel_write_trace(ta_system *system, const ta_trace_record *record, const ta_tel_values *values) {
    if (system->rings.control == NULL) {
        return; /* a system that is being torn down after a failed creation has no ring */
    }
    ta_ring *tel = &system->rings.ring[TA_RING_TEL];
    if (system->tel.pending != 0 && !tel_report_dropped(system, tel)) {
        tel_lose(system, tel); /* the report does not fit, so neither does anything after it */
        return;
    }
    tel_stage stage = { .base = system->tel_staging, .capacity = system->tel_staging_bytes };
    ta_msg_tel_trace data;
    uint32_t size = 0;
    if (!tel_build(system, &(tel_build_params){ .record = record,
                                                .values = values,
                                                .stage = &stage,
                                                .data = &data,
                                                .size = &size })) {
        tel_lose(system, tel);
        return;
    }
    ta_ring_reservation reservation;
    switch (ta_ring_reserve(tel, size, &reservation)) {
    case TA_RING_OK:
        if (ta_msg_tel_trace_write(&data, &reservation) == TA_OK) {
            ta_ring_commit(tel);
        } else {
            ta_ring_abandon(tel); /* nothing was stored */
            tel_lose(system, tel);
        }
        return;
    case TA_RING_DROPPED:
        /* The ring consumed the seq and counted the drop; remember it as ours. */
        if (system->tel.pending == 0) {
            system->tel.first_seq = reservation.seq;
        }
        system->tel.pending += 1u;
        return;
    default:
        tel_lose(system, tel); /* too big for the ring, or the consumer's tail is corrupt: lost either way */
        return;
    }
}

/* ------------------------------------------------------------------------
 * Masks
 */

ta_result ta_system_set_trace_masks(ta_system *system, const ta_trace_masks *masks) {
    if (system == NULL || masks == NULL) {
        return TA_ERR_INVALID;
    }
    system->trace_masks = *masks;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Reading
 */

uint32_t ta_tel_u32_at(const ta_tel_u32s *list, uint32_t index) {
    uint32_t value = 0;
    if (list == NULL || list->bytes == NULL || index >= list->count) {
        return 0;
    }
    ta_mem_copy(&value, list->bytes + (size_t)index * sizeof value, sizeof value);
    return value;
}

/* tel_str views a span of a checked record as text. */
static ta_str tel_str(ta_bytes bytes) { return (ta_str){ (const char *)bytes.ptr, bytes.len }; }

/* tel_list views a span of a checked record as a list of u32; false when its length is not a multiple of 4. */
static bool tel_list(ta_bytes bytes, ta_tel_u32s *out) {
    if (bytes.len % (uint32_t)sizeof(uint32_t) != 0) {
        return false;
    }
    *out = (ta_tel_u32s){ bytes.ptr, bytes.len / (uint32_t)sizeof(uint32_t) };
    return true;
}

/* tel_trace_entry converts a checked tel.trace into the public view; false when a field is out of range. */
static bool tel_trace_entry(const ta_msg_tel_trace *msg, ta_tel_trace *out) {
    if (msg->trace_kind > (uint16_t)TA_TRACE_WITHDRAWN || msg->position > (uint8_t)TA_TRACE_AT_INVOKE ||
        msg->event_type > (uint8_t)TA_EVENT_PLATFORM) {
        return false;
    }
    ta_tel_trace trace = { 0 };
    trace.kind = (ta_trace_kind)msg->trace_kind;
    trace.key = (ta_step_key){ .session = msg->head.session, .macrostep = msg->macrostep,
                               .microstep = msg->microstep, .family = msg->family };
    trace.position = (ta_trace_position){ .at = (ta_trace_at)msg->position, .state = msg->at_state,
                                          .transition = msg->at_transition, .action = msg->at_action,
                                          .iteration = msg->at_iteration };
    trace.context = (ta_trace_context){ .trace_id_high = msg->trace_id_high, .trace_id_low = msg->trace_id_low,
                                        .flags = msg->context_flags, .detail = msg->context_detail };
    trace.cause = (ta_step_key){ .session = msg->cause_session, .macrostep = msg->cause_macrostep,
                                 .microstep = msg->cause_microstep, .family = msg->cause_family };
    trace.actor = msg->actor;
    trace.definition = msg->definition;
    trace.now = msg->head.at;
    trace.has_event = (msg->head.flags & TA_MSG_TEL_TRACE_HAS_EVENT) != 0;
    trace.event_type = (ta_event_type)msg->event_type;
    trace.event_name = tel_str(msg->event_name);
    trace.event_send_id = tel_str(msg->event_send_id);
    trace.event_invoke_id = tel_str(msg->event_invoke_id);
    trace.event_data = tel_str(msg->event_data);
    trace.state = msg->state;
    trace.expr = msg->expr;
    trace.name = tel_str(msg->name);
    trace.action = msg->action;
    trace.iteration = msg->iteration;
    trace.send = tel_str(msg->send);
    trace.invoke = tel_str(msg->invoke);
    trace.result = msg->result != 0;
    trace.has_value = (msg->head.flags & TA_MSG_TEL_TRACE_HAS_VALUE) != 0;
    trace.value = tel_str(msg->value);
    trace.truncated = (msg->head.flags & TA_MSG_TEL_TRACE_TRUNCATED) != 0;
    trace.event_data_truncated = (msg->head.flags & TA_MSG_TEL_TRACE_EVENT_DATA_TRUNCATED) != 0;
    trace.fault_data_truncated = (msg->head.flags & TA_MSG_TEL_TRACE_FAULT_DATA_TRUNCATED) != 0;
    trace.suppressed = (msg->head.flags & TA_MSG_TEL_TRACE_SUPPRESSED) != 0;
    trace.has_fault = (msg->head.flags & TA_MSG_TEL_TRACE_HAS_FAULT) != 0;
    trace.fault_code = (int32_t)msg->fault_code;
    trace.fault_detail = tel_str(msg->fault_detail);
    trace.fault_message = tel_str(msg->fault_message);
    trace.fault_data = tel_str(msg->fault_data);
    trace.dropped = msg->dropped;
    trace.identity = (ta_definition_identity){ .family = { .name = tel_str(msg->identity_family) },
                                               .revision = msg->identity_revision };
    if (!tel_list(msg->transitions, &trace.transitions) || !tel_list(msg->exited, &trace.exited) ||
        !tel_list(msg->entered, &trace.entered) || !tel_list(msg->history, &trace.history) ||
        !tel_list(msg->states, &trace.states)) {
        return false;
    }
    *out = trace;
    return true;
}

ta_result ta_tel_decode(const ta_ring_record *record, ta_tel_entry *out) {
    if (record == NULL || out == NULL || record->bytes == NULL || record->seq == 0) {
        return TA_ERR_INVALID;
    }
    ta_record_reader reader;
    if (ta_record_reader_init(&reader, &(ta_record_reader_params){ .table = ta_msg_table(),
                                                                    .ring = TA_RING_TEL,
                                                                    .mode = TA_RING_LOSSY,
                                                                    .max_record_bytes = record->size }) != TA_OK) {
        return TA_ERR_INVALID;
    }
    reader.next_seq = record->seq; /* the record is checked on its own: it is the next one, whatever came before */
    ta_record checked;
    if (ta_record_check(&reader, &(ta_record_input){ .bytes = record->bytes, .length = record->size }, &checked) !=
        TA_RECORD_OK ||
        checked.seq != record->seq) {
        return TA_ERR_INVALID; /* the seq the ring reported is the seq the record carries */
    }
    ta_tel_entry entry = { 0 };
    entry.seq = record->seq;
    entry.gap = record->gap;
    if (checked.kind == TA_MSG_TEL_TRACE) {
        ta_msg_tel_trace msg;
        if (ta_msg_tel_trace_get(&checked, &msg) != TA_OK || !tel_trace_entry(&msg, &entry.trace)) {
            return TA_ERR_INVALID;
        }
        entry.type = TA_TEL_TRACE;
        entry.at = msg.head.at;
    } else if (checked.kind == TA_MSG_TEL_DROPPED) {
        ta_msg_tel_dropped msg;
        if (ta_msg_tel_dropped_get(&checked, &msg) != TA_OK) {
            return TA_ERR_INVALID;
        }
        entry.type = TA_TEL_DROPPED;
        entry.at = msg.head.at;
        entry.dropped = (ta_tel_dropped){ .count = msg.count, .first_seq = msg.first_seq };
    } else {
        return TA_ERR_INVALID;
    }
    *out = entry;
    return TA_OK;
}

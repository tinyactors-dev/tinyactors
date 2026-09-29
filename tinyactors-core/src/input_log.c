#include "input_log.h"
#include "interpreter.h"
#include "scxml_uri.h"

/*
 * The input log (ADR 0090). Writing stages small pieces through the
 * system's scratch buffer and hands larger ones to the sink as they are;
 * reading takes exact byte counts from a source, so a source handed to a
 * replay is left at the next record.
 */

#define TA_RECORD_HEADER_BYTES 32u
#define TA_RECORD_TEXT_ABSENT UINT32_MAX
#define TA_RECORD_WINDOW 128u

static uint32_t ta_input_pad(uint32_t length) {
    return (8u - length % 8u) % 8u;
}

/* ------------------------------------------------------------------------
 * Which actors are logged
 */

bool ta_input_family_logged(const ta_system *system, ta_atom family) {
    for (uint32_t index = 0; index < system->logged_families.count; index += 1u) {
        if (*(const ta_atom *)ta_vec_at(&system->logged_families, index) == family) {
            return true;
        }
    }
    return false;
}

bool ta_input_logged(const ta_system *system, const ta_actor *actor) {
    return system->input_sink.v != NULL && actor != NULL && actor->published &&
           actor->record.self.kind == TA_ACTOR_REF_NAMED && actor->definition->input_logged;
}

void ta_system_set_input_sink(ta_system *system, ta_byte_sink sink) {
    if (system != NULL && !system->input_open) {
        system->input_sink = sink;
    }
}

ta_result ta_system_log_family(ta_system *system, ta_str family, bool logged) {
    if (system == NULL || family.ptr == NULL || family.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_atom atom = 0;
    ta_result result = logged ? ta_atom_intern(&system->atoms, family, &atom)
                              : ta_atom_find(&system->atoms, family, &atom);
    if (result != TA_OK) {
        return logged ? result : TA_OK;
    }
    ta_vec *families = &system->logged_families;
    uint32_t found = UINT32_MAX;
    for (uint32_t index = 0; index < families->count && found == UINT32_MAX; index += 1u) {
        found = *(ta_atom *)ta_vec_at(families, index) == atom ? index : UINT32_MAX;
    }
    if (logged && found == UINT32_MAX) {
        result = ta_vec_push(families, &atom);
        if (result != TA_OK) {
            return result;
        }
    } else if (!logged && found != UINT32_MAX) {
        *(ta_atom *)ta_vec_at(families, found) = *(ta_atom *)ta_vec_at(families, families->count - 1u);
        families->count -= 1u;
    }
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1u) {
        ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        if (entry->definition != NULL && entry->definition->family == atom) {
            entry->definition->input_logged = logged;
        }
    }
    return TA_OK;
}

ta_result ta_input_prepare_actor(ta_system *system, ta_actor *actor) {
    return ta_interpreter_sparse(system, actor) != NULL ? TA_OK : TA_ERR_NOMEM;
}

/* ------------------------------------------------------------------------
 * Writing: staged output
 */

static void ta_input_flush(ta_system *system) {
    if (system->input_used == 0 || system->input_status != TA_OP_OK) {
        system->input_used = 0;
        return;
    }
    system->input_status = system->input_sink.v->write(
        system->input_sink.self, (ta_bytes){ .ptr = system->input_scratch, .len = system->input_used });
    system->input_used = 0;
}

static void ta_input_put(ta_system *system, const void *bytes, uint32_t length) {
    if (system->input_status != TA_OP_OK || length == 0) {
        return;
    }
    if (system->input_used + length <= TA_INPUT_SCRATCH_BYTES) {
        ta_mem_copy(system->input_scratch + system->input_used, bytes, length);
        system->input_used += length;
        return;
    }
    ta_input_flush(system);
    if (length <= TA_INPUT_SCRATCH_BYTES) {
        if (system->input_status == TA_OP_OK) {
            ta_mem_copy(system->input_scratch, bytes, length);
            system->input_used = length;
        }
        return;
    }
    if (system->input_status == TA_OP_OK) {
        system->input_status = system->input_sink.v->write(system->input_sink.self,
                                                           (ta_bytes){ .ptr = bytes, .len = length });
    }
}

static void ta_input_u16(ta_system *system, uint16_t value) {
    uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    ta_input_put(system, bytes, 2);
}

static void ta_input_u32(ta_system *system, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t index = 0; index < 4u; index += 1u) {
        bytes[index] = (uint8_t)(value >> (8u * index));
    }
    ta_input_put(system, bytes, 4);
}

static void ta_input_u64(ta_system *system, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t index = 0; index < 8u; index += 1u) {
        bytes[index] = (uint8_t)(value >> (8u * index));
    }
    ta_input_put(system, bytes, 8);
}

static void ta_input_zeros(ta_system *system, uint32_t count) {
    static const uint8_t zeros[8] = { 0 };
    ta_input_put(system, zeros, count);
}

static uint32_t ta_input_text_length(const ta_record_text *text) {
    if (!text->present) {
        return 0;
    }
    if (text->write == NULL) {
        return text->text.len;
    }
    ta_text_writer measure = { 0 };
    (void)text->write(text->context, &measure);
    return measure.length;
}

/* A rendered text goes through a small window, rendered again for each. */
static void ta_input_text(ta_system *system, const ta_record_text *text, uint32_t length) {
    if (!text->present || length == 0) {
        return;
    }
    if (text->write == NULL) {
        ta_input_put(system, text->text.ptr, text->text.len);
        return;
    }
    char window[TA_RECORD_WINDOW];
    for (uint32_t skip = 0; skip < length; skip += TA_RECORD_WINDOW) {
        ta_text_writer writer = { .buffer = window, .capacity = TA_RECORD_WINDOW, .skip = skip };
        (void)text->write(text->context, &writer);
        uint32_t count = length - skip < TA_RECORD_WINDOW ? length - skip : TA_RECORD_WINDOW;
        ta_input_put(system, window, count);
    }
}

/* A data model's pieces become chunks: u32 length, the bytes, zeros to 8. */
static ta_op_status ta_input_frame_begin(void *self) {
    (void)self;
    return TA_OP_OK;
}

static ta_op_status ta_input_frame_write(void *self, ta_bytes bytes) {
    ta_system *system = self;
    if (bytes.len == 0) {
        return system->input_status;
    }
    ta_input_u32(system, bytes.len);
    ta_input_put(system, bytes.ptr, bytes.len);
    ta_input_zeros(system, ta_input_pad(4u + bytes.len));
    return system->input_status;
}

static void ta_input_frame_end(void *self, bool keep) {
    (void)self;
    (void)keep;
}

static const ta_byte_sink_vtable ta_input_frame_vtable = {
    .begin = ta_input_frame_begin,
    .write = ta_input_frame_write,
    .end = ta_input_frame_end,
};

static bool ta_input_value_present(const ta_record_desc *record, const ta_value *value) {
    return value != NULL && value->v != NULL && record->definition != NULL;
}

static uint16_t ta_input_value_count(const ta_record_desc *record) {
    return (uint16_t)((ta_input_value_present(record, record->value) ? 1u : 0u) +
                      (ta_input_value_present(record, record->value2) ? 1u : 0u));
}

static ta_result ta_input_value(ta_system *system, const ta_record_desc *record, const ta_value *value) {
    const ta_definition *definition = record->definition;
    if (definition->datamodel.v->value_write == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_fault fault = { 0 };
    ta_op_status status = definition->datamodel.v->value_write(
        definition->datamodel.self, &(ta_datamodel_value_write_params){
                                        .shared = definition->datamodel_shared,
                                        .value = value,
                                        .sink = { .self = system, .v = &ta_input_frame_vtable },
                                        .durable = true,
                                        .fault = &fault,
                                    });
    if (status == TA_OP_ERROR) {
        ta_value_release(&fault.data);
    }
    if (status != TA_OP_OK) {
        return status == TA_OP_NOMEM || system->input_status != TA_OP_OK ? TA_ERR_NOMEM : TA_ERR_UNSUPPORTED;
    }
    ta_input_u32(system, 0);
    ta_input_zeros(system, 4);
    return TA_OK;
}

static ta_record_text ta_input_target_text(const ta_record_desc *record, uint32_t index) {
    ta_state_id state = record->target_at != NULL ? record->target_at(record->target_context, index)
                                                  : record->targets[index];
    ta_atom name = record->definition->states[state].name;
    return (ta_record_text){ .present = true, .text = ta_atom_text(&record->definition->system->atoms, name) };
}

/* Writes a whole record into the open unit, under `seq`. */
static ta_result ta_input_emit(ta_system *system, uint64_t seq, const ta_record_desc *record) {
    uint32_t text_bytes = 0;
    for (uint32_t index = 0; index < record->field_count; index += 1u) {
        text_bytes += record->fields[index].is_text ? ta_input_text_length(&record->fields[index].text) : 0u;
    }
    for (uint32_t index = 0; index < record->target_count; index += 1u) {
        text_bytes += ta_input_target_text(record, index).text.len;
    }
    uint32_t fixed_bytes = TA_RECORD_HEADER_BYTES + 8u * (record->field_count + record->target_count);
    ta_input_u16(system, TA_RECORD_VERSION);
    ta_input_u16(system, (uint16_t)record->kind);
    ta_input_u32(system, fixed_bytes);
    ta_input_u64(system, seq);
    ta_input_u64(system, record->time);
    ta_input_u32(system, text_bytes);
    ta_input_u16(system, ta_input_value_count(record));
    ta_input_u16(system, 0);
    uint32_t offset = 0;
    for (uint32_t index = 0; index < record->field_count; index += 1u) {
        const ta_record_field *field = &record->fields[index];
        if (!field->is_text) {
            ta_input_u64(system, field->number);
            continue;
        }
        uint32_t length = ta_input_text_length(&field->text);
        ta_input_u32(system, field->text.present ? offset : TA_RECORD_TEXT_ABSENT);
        ta_input_u32(system, length);
        offset += length;
    }
    for (uint32_t index = 0; index < record->target_count; index += 1u) {
        uint32_t length = ta_input_target_text(record, index).text.len;
        ta_input_u32(system, offset);
        ta_input_u32(system, length);
        offset += length;
    }
    for (uint32_t index = 0; index < record->field_count; index += 1u) {
        const ta_record_field *field = &record->fields[index];
        if (field->is_text) {
            ta_input_text(system, &field->text, ta_input_text_length(&field->text));
        }
    }
    for (uint32_t index = 0; index < record->target_count; index += 1u) {
        ta_record_text text = ta_input_target_text(record, index);
        ta_input_text(system, &text, text.text.len);
    }
    ta_input_zeros(system, ta_input_pad(text_bytes));
    ta_result result = TA_OK;
    if (ta_input_value_present(record, record->value)) {
        result = ta_input_value(system, record, record->value);
    }
    if (result == TA_OK && ta_input_value_present(record, record->value2)) {
        result = ta_input_value(system, record, record->value2);
    }
    ta_input_flush(system);
    if (result == TA_OK && system->input_status != TA_OP_OK) {
        result = TA_ERR_NOMEM;
    }
    return result;
}

ta_result ta_input_open(ta_system *system, ta_actor *actor, const ta_record_desc *record) {
    if (system->input_sink.v == NULL) {
        return TA_OK;
    }
    if (system->input_open) {
        return TA_ERR_BUSY;
    }
    uint64_t seq = 0;
    if (actor != NULL) {
        ta_result result = ta_input_prepare_actor(system, actor);
        if (result != TA_OK) {
            return result;
        }
        seq = actor->sparse->log_seq + 1u;
    }
    if (system->input_sink.v->begin(system->input_sink.self) != TA_OP_OK) {
        return TA_ERR_NOMEM;
    }
    system->input_open = true;
    system->input_status = TA_OP_OK;
    system->input_used = 0;
    system->input_actor = actor;
    system->input_seq = seq;
    ta_result result = ta_input_emit(system, seq, record);
    if (result != TA_OK) {
        ta_input_close(system, false);
    }
    return result;
}

void ta_input_close(ta_system *system, bool keep) {
    if (!system->input_open) {
        return;
    }
    system->input_sink.v->end(system->input_sink.self, keep);
    if (keep && system->input_actor != NULL) {
        system->input_actor->sparse->log_seq = system->input_seq;
    }
    system->input_open = false;
    system->input_actor = NULL;
}

ta_result ta_input_write(ta_system *system, ta_actor *actor, const ta_record_desc *record) {
    ta_result result = ta_input_open(system, actor, record);
    if (result == TA_OK) {
        ta_input_close(system, true);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Record builders
 */

static bool ta_input_event_name_write(const void *context, ta_text_writer *writer) {
    const ta_record_event_texts *texts = context;
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&texts->system->event_names, texts->name, segments);
    for (uint32_t index = 0; index < count; index += 1u) {
        if (index != 0) {
            ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
        }
        ta_text_put(writer, ta_atom_text(&texts->system->atoms, segments[index]));
    }
    return true;
}

static bool ta_input_send_id_write(const void *context, ta_text_writer *writer) {
    const ta_record_event_texts *texts = context;
    ta_send_id_write(texts->system, texts->send_id, writer);
    return true;
}

static bool ta_input_origin_write(const void *context, ta_text_writer *writer) {
    const ta_record_event_texts *texts = context;
    return ta_scxml_uri_write(texts->system, writer, &texts->origin);
}

static ta_record_field ta_input_number(uint64_t number) {
    return (ta_record_field){ .number = number };
}

static ta_record_field ta_input_str(bool present, ta_str text) {
    return (ta_record_field){ .is_text = true, .text = { .present = present, .text = text } };
}

void ta_input_event_fields(const ta_record_event_params *params, ta_record_event_texts *texts, ta_record_desc *record) {
    ta_system *system = params->system;
    const ta_event *event = params->event;
    *texts = (ta_record_event_texts){ .system = system, .name = event->name, .send_id = event->send_id };
    uint32_t at = record->field_count;
    record->fields[at++] = ta_input_number((uint64_t)event->type);
    record->fields[at++] = ta_input_number(params->host_seq);
    record->fields[at++] = (ta_record_field){
        .is_text = true, .text = { .present = true, .write = ta_input_event_name_write, .context = texts }
    };
    record->fields[at++] = (ta_record_field){
        .is_text = true,
        .text = { .present = event->send_id != 0, .write = ta_input_send_id_write, .context = texts },
    };
    ta_record_field origin = ta_input_str(false, (ta_str){ 0 });
    if (event->origin.kind == TA_ORIGIN_ACTOR) {
        texts->origin = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR, .as.actor = event->origin.as.actor };
        origin.text = (ta_record_text){ .present = true, .write = ta_input_origin_write, .context = texts };
    } else if (event->origin.kind == TA_ORIGIN_INVOKE) {
        texts->origin = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE, .as.invoke = event->origin.as.invoke.invoke };
        origin.text = (ta_record_text){ .present = true, .write = ta_input_origin_write, .context = texts };
    } else if (event->origin.kind == TA_ORIGIN_IO) {
        ta_str uri = { 0 };
        const ta_actor *actor = params->actor;
        ta_result rendered = ta_io_origin_uri(
            system, &(ta_io_origin_render_params){
                        .origin = &event->origin,
                        .session_id = actor != NULL ? actor->session_id : 0,
                        .address = actor != NULL && actor->record.self.kind == TA_ACTOR_REF_NAMED
                                       ? actor->record.self.as.named
                                       : NULL,
                        .arena = ta_arena_allocator(&system->scratch),
                        .out = &uri,
                    });
        origin = ta_input_str(rendered == TA_OK, uri);
    }
    record->fields[at++] = origin;
    ta_str type = { 0 };
    record->fields[at++] = ta_input_str(ta_io_origin_type(system, &event->origin, &type) == TA_OK, type);
    bool invoked = event->invoke_id != TA_INVOKE_ID_NONE && event->invoke_id != 0;
    record->fields[at++] = ta_input_str(invoked, invoked ? ta_invoke_id_text(system, event->invoke_id) : (ta_str){ 0 });
    record->field_count = at;
}

void ta_input_send_field(ta_record_event_texts *texts, ta_send_id send, ta_record_desc *record) {
    texts->send_id = send;
    record->fields[record->field_count++] = (ta_record_field){
        .is_text = true, .text = { .present = send != 0, .write = ta_input_send_id_write, .context = texts }
    };
}

static ta_str ta_input_state_text(const ta_system *system, const ta_definition *definition, ta_state_id state) {
    if (state == TA_STATE_NONE || state >= definition->state_count) {
        return (ta_str){ 0 };
    }
    return ta_atom_text(&system->atoms, definition->states[state].name);
}

bool ta_input_runaway_stopped(const ta_system *system, const ta_actor *actor) {
    uint32_t limit = actor->definition->microstep_limit == 0 ? system->microstep_limit
                                                            : actor->definition->microstep_limit;
    return actor->macrostep && actor->trace_begun && limit != 0 && limit != TA_MICROSTEP_LIMIT_NONE &&
           actor->trace_microstep >= limit;
}

ta_result ta_input_finished(ta_system *system, ta_actor *actor) {
    if (!ta_input_logged(system, actor) || !actor->finished ||
        (actor->sparse != NULL && actor->sparse->log_finished)) {
        return TA_OK;
    }
    ta_state_id final_state = actor->sparse != NULL ? actor->sparse->final_state : TA_STATE_NONE;
    ta_str text = ta_input_state_text(system, actor->definition, final_state);
    ta_record_desc record = { .kind = TA_RECORD_FINISHED, .time = ta_actor_now(system, actor), .field_count = 1 };
    record.fields[0] = ta_input_str(text.ptr != NULL, text);
    ta_result result = ta_input_write(system, actor, &record);
    if (result == TA_OK) {
        actor->sparse->log_finished = true;
    }
    return result;
}

ta_result ta_input_released(ta_system *system, ta_actor *actor, bool no_microstep_limit) {
    if (!no_microstep_limit || !ta_input_logged(system, actor) || !ta_input_runaway_stopped(system, actor) ||
        (actor->sparse != NULL && actor->sparse->log_released)) {
        return TA_OK;
    }
    ta_record_desc record = { .kind = TA_RECORD_RELEASED, .time = ta_actor_now(system, actor) };
    ta_result result = ta_input_write(system, actor, &record);
    if (result == TA_OK) {
        actor->sparse->log_released = true;
    }
    return result;
}

ta_result ta_system_write_host_record(ta_system *system, const ta_host_record_params *params) {
    if (system == NULL || params == NULL || params->address == NULL ||
        (params->kind != TA_RECORD_QUEUED && params->kind != TA_RECORD_DROPPED &&
         params->kind != TA_RECORD_EVICTED) ||
        (params->kind == TA_RECORD_QUEUED && params->event == NULL)) {
        return TA_ERR_INVALID;
    }
    ta_atom family = 0;
    if (system->input_sink.v == NULL ||
        ta_atom_find(&system->atoms, params->address->family.name, &family) != TA_OK ||
        !ta_input_family_logged(system, family)) {
        return TA_OK;
    }
    ta_actor *actor = NULL;
    if (params->actor != 0) {
        actor = ta_actor_lookup(system, params->actor);
        if (actor == NULL || !actor->published || actor->record.self.kind != TA_ACTOR_REF_NAMED ||
            !ta_str_equal(ta_address_view(actor->record.self.as.named).name, params->address->name) ||
            actor->definition->family != family) {
            return TA_ERR_INVALID;
        }
    }
    const ta_definition *definition = actor != NULL ? actor->definition
                                                    : ta_system_definition(system, params->definition);
    ta_record_desc record = { .kind = params->kind, .time = ta_clock_now(&system->clock) };
    ta_record_event_texts texts;
    if (params->kind == TA_RECORD_QUEUED) {
        if (params->event->data.v != NULL && definition == NULL) {
            return TA_ERR_INVALID;
        }
        ta_input_event_fields(&(ta_record_event_params){ .system = system,
                                                         .actor = actor,
                                                         .event = params->event,
                                                         .host_seq = params->host_seq },
                              &texts, &record);
        record.value = &params->event->data;
        record.definition = definition;
    } else if (params->kind == TA_RECORD_DROPPED) {
        record.fields[record.field_count++] = ta_input_number(params->host_seq);
    }
    ta_result result = ta_input_write(system, actor, &record);
    if (result == TA_OK && actor != NULL && params->kind == TA_RECORD_EVICTED) {
        actor->sparse->log_evicted = true;
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Reading
 */

static ta_result ta_input_read_exact(const ta_byte_source *source, uint8_t *buffer, uint32_t length) {
    uint32_t done = 0;
    while (done < length) {
        uint32_t got = 0;
        ta_op_status status = source->v->read(
            source->self, &(ta_byte_read_params){ .buffer = buffer + done, .capacity = length - done, .got = &got });
        if (status == TA_OP_NOMEM) {
            return TA_ERR_NOMEM;
        }
        if (status != TA_OP_OK || got == 0 || got > length - done) {
            return TA_ERR_CORRUPT;
        }
        done += got;
    }
    return TA_OK;
}

static ta_result ta_input_skip(const ta_byte_source *source, uint32_t length) {
    uint8_t scrap[64];
    while (length > 0) {
        uint32_t count = length < sizeof scrap ? length : (uint32_t)sizeof scrap;
        ta_result result = ta_input_read_exact(source, scrap, count);
        if (result != TA_OK) {
            return result;
        }
        length -= count;
    }
    return TA_OK;
}

static uint64_t ta_input_le(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        value |= (uint64_t)bytes[index] << (8u * index);
    }
    return value;
}

/* The fields a kind has at least, before any JUMP targets. */
static uint32_t ta_input_known_fields(ta_record_kind kind) {
    switch (kind) {
        case TA_RECORD_SPAWNED:
            return 3;
        case TA_RECORD_EVENT:
        case TA_RECORD_QUEUED:
            return 7;
        case TA_RECORD_JUMP:
            return 9;
        case TA_RECORD_REPORT:
            return 4;
        case TA_RECORD_FIRED:
        case TA_RECORD_WITHDRAWN:
        case TA_RECORD_FINISHED:
        case TA_RECORD_DROPPED:
            return 1;
        case TA_RECORD_ABANDONED:
        case TA_RECORD_RELEASED:
        case TA_RECORD_DESTROYED:
        case TA_RECORD_EVICTED:
            return 0;
        case TA_RECORD_CHECKPOINT:
            return 11;
        case TA_RECORD_CHECKPOINT_HISTORY:
            return 2;
        case TA_RECORD_CHECKPOINT_SEND:
            return 8;
        case TA_RECORD_CHECKPOINT_INVOKE:
            return 3;
        default:
            return UINT32_MAX;
    }
}

static void ta_input_record_free(ta_system *system, ta_input_record *record) {
    if (record->block != NULL) {
        ta_mem_free(system->allocator, record->block,
                    &(ta_allocator_free_params){ .size = record->block_bytes, .alignment = 8 });
        record->block = NULL;
    }
}

static uint64_t ta_input_field(const ta_input_record *record, uint32_t index) {
    return ta_input_le(record->block + 8u * index, 8);
}

/* A text field: false when absent; an out-of-range reference is corrupt. */
static bool ta_input_field_text(const ta_input_record *record, uint32_t index, ta_str *out) {
    const uint8_t *field = record->block + 8u * index;
    uint32_t offset = (uint32_t)ta_input_le(field, 4);
    uint32_t length = (uint32_t)ta_input_le(field + 4, 4);
    if (offset == TA_RECORD_TEXT_ABSENT) {
        *out = (ta_str){ 0 };
        return false;
    }
    *out = (ta_str){ .ptr = (const char *)record->text + offset, .len = length };
    return true;
}

static bool ta_input_text_refs_valid(const ta_input_record *record, uint32_t first, uint32_t count) {
    for (uint32_t index = first; index < first + count; index += 1u) {
        const uint8_t *field = record->block + 8u * index;
        uint64_t offset = ta_input_le(field, 4);
        uint64_t length = ta_input_le(field + 4, 4);
        if (offset != TA_RECORD_TEXT_ABSENT && offset + length > record->text_bytes) {
            return false;
        }
    }
    return true;
}

/* Which fields of a kind are texts, as a bit per field. */
static uint32_t ta_input_text_mask(ta_record_kind kind) {
    switch (kind) {
        case TA_RECORD_SPAWNED:
            return 0x7u;
        case TA_RECORD_EVENT:
        case TA_RECORD_QUEUED:
            return 0x7cu;
        case TA_RECORD_JUMP:
            return 0x17cu;
        case TA_RECORD_REPORT:
            return 0xcu;
        case TA_RECORD_FIRED:
        case TA_RECORD_WITHDRAWN:
        case TA_RECORD_FINISHED:
        case TA_RECORD_CHECKPOINT_HISTORY:
            return 0x1u;
        case TA_RECORD_CHECKPOINT:
            return 0x7u;
        case TA_RECORD_CHECKPOINT_SEND:
            return 0xfu;
        case TA_RECORD_CHECKPOINT_INVOKE:
            return 0x3u;
        default:
            return 0;
    }
}

/* The texts that follow a kind's known fields: JUMP's targets, a
 * checkpoint's two state lists, a history's recorded states. */
static uint64_t ta_input_trailing(const ta_input_record *record) {
    switch (record->kind) {
        case TA_RECORD_JUMP:
            return ta_input_field(record, 8);
        case TA_RECORD_CHECKPOINT:
            return ta_input_field(record, 9) + ta_input_field(record, 10);
        case TA_RECORD_CHECKPOINT_HISTORY:
            return ta_input_field(record, 1);
        default:
            return 0;
    }
}

static ta_result ta_input_read_record(ta_system *system, const ta_byte_source *source, ta_input_record *out) {
    uint8_t header[TA_RECORD_HEADER_BYTES];
    ta_result result = ta_input_read_exact(source, header, TA_RECORD_HEADER_BYTES);
    if (result != TA_OK) {
        return result;
    }
    uint16_t version = (uint16_t)ta_input_le(header, 2);
    ta_record_kind kind = (ta_record_kind)ta_input_le(header + 2, 2);
    uint32_t fixed_bytes = (uint32_t)ta_input_le(header + 4, 4);
    uint32_t known = ta_input_known_fields(kind);
    *out = (ta_input_record){
        .kind = kind,
        .seq = ta_input_le(header + 8, 8),
        .time = ta_input_le(header + 16, 8),
        .text_bytes = (uint32_t)ta_input_le(header + 24, 4),
        .value_count = (uint16_t)ta_input_le(header + 28, 2),
    };
    if (version != TA_RECORD_VERSION || known == UINT32_MAX || fixed_bytes % 8u != 0 ||
        fixed_bytes < TA_RECORD_HEADER_BYTES + 8u * known ||
        out->value_count > (kind == TA_RECORD_CHECKPOINT_SEND ? 2u : 1u)) {
        return TA_ERR_CORRUPT;
    }
    out->fields = (fixed_bytes - TA_RECORD_HEADER_BYTES) / 8u;
    size_t text_padded = (size_t)out->text_bytes + ta_input_pad(out->text_bytes);
    out->block_bytes = (size_t)(fixed_bytes - TA_RECORD_HEADER_BYTES) + text_padded;
    if (out->block_bytes == 0) {
        return TA_OK;
    }
    out->block = ta_mem_alloc(system->allocator, out->block_bytes, 8);
    if (out->block == NULL) {
        return TA_ERR_NOMEM;
    }
    result = ta_input_read_exact(source, out->block, (uint32_t)out->block_bytes);
    out->text = out->block + (fixed_bytes - TA_RECORD_HEADER_BYTES);
    uint32_t mask = ta_input_text_mask(kind);
    for (uint32_t index = 0; result == TA_OK && index < known; index += 1u) {
        if ((mask >> index & 1u) != 0 && !ta_input_text_refs_valid(out, index, 1)) {
            result = TA_ERR_CORRUPT;
        }
    }
    if (result == TA_OK) {
        uint64_t trailing = ta_input_trailing(out);
        if (trailing > out->fields - known || !ta_input_text_refs_valid(out, known, (uint32_t)trailing)) {
            result = TA_ERR_CORRUPT;
        }
    }
    if (result != TA_OK) {
        ta_input_record_free(system, out);
    }
    return result;
}

/* A value stream as a source that ends at its chunk of length 0. */
typedef struct {
    const ta_byte_source *source;
    uint32_t left; /* bytes of the current chunk not yet read */
    uint32_t pad;  /* zeros after the current chunk */
    bool ended;
    ta_result error;
} ta_input_value_in;

static ta_op_status ta_input_value_read(void *self, const ta_byte_read_params *params) {
    ta_input_value_in *in = self;
    *params->got = 0;
    if (in->error != TA_OK) {
        return TA_OP_ERROR;
    }
    while (!in->ended && in->left == 0) {
        in->error = ta_input_skip(in->source, in->pad);
        uint8_t frame[4];
        if (in->error == TA_OK) {
            in->error = ta_input_read_exact(in->source, frame, 4);
        }
        if (in->error != TA_OK) {
            return in->error == TA_ERR_NOMEM ? TA_OP_NOMEM : TA_OP_ERROR;
        }
        uint32_t length = (uint32_t)ta_input_le(frame, 4);
        in->pad = ta_input_pad(4u + length);
        if (length == 0) {
            in->error = ta_input_skip(in->source, in->pad);
            in->pad = 0;
            in->ended = true;
            if (in->error != TA_OK) {
                return in->error == TA_ERR_NOMEM ? TA_OP_NOMEM : TA_OP_ERROR;
            }
        }
        in->left = length;
    }
    if (in->ended) {
        return TA_OP_OK;
    }
    uint32_t count = params->capacity < in->left ? params->capacity : in->left;
    in->error = ta_input_read_exact(in->source, params->buffer, count);
    if (in->error != TA_OK) {
        return in->error == TA_ERR_NOMEM ? TA_OP_NOMEM : TA_OP_ERROR;
    }
    in->left -= count;
    *params->got = count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable ta_input_value_vtable = { .read = ta_input_value_read };

/* Reads the rest of a value stream the data model left. */
static ta_result ta_input_value_drain(ta_input_value_in *in) {
    uint8_t scrap[64];
    while (!in->ended) {
        uint32_t got = 0;
        if (ta_input_value_read(in, &(ta_byte_read_params){ .buffer = scrap, .capacity = sizeof scrap, .got = &got }) !=
            TA_OP_OK) {
            return in->error != TA_OK ? in->error : TA_ERR_CORRUPT;
        }
    }
    return TA_OK;
}

typedef struct {
    const ta_input_record *record;
    const ta_byte_source *source;
    const ta_definition *definition;
    ta_value *out;
} ta_input_value_params;

/* Reads the next value stream through `definition`'s data model. */
static ta_result ta_input_read_stream(const ta_input_value_params *params) {
    ta_value *out = params->out;
    *out = (ta_value){ 0 };
    const ta_definition *definition = params->definition;
    ta_input_value_in in = { .source = params->source };
    ta_result result = TA_OK;
    if (definition->datamodel.v->value_read == NULL) {
        result = TA_ERR_UNSUPPORTED;
    } else {
        ta_fault fault = { 0 };
        ta_op_status status = definition->datamodel.v->value_read(
            definition->datamodel.self, &(ta_datamodel_value_read_params){
                                            .shared = definition->datamodel_shared,
                                            .source = { .self = &in, .v = &ta_input_value_vtable },
                                            .durable = true,
                                            .out = out,
                                            .fault = &fault,
                                        });
        if (status == TA_OP_ERROR) {
            ta_value_release(&fault.data);
        }
        if (in.error != TA_OK) {
            result = in.error;
        } else if (status != TA_OP_OK) {
            result = status == TA_OP_NOMEM ? TA_ERR_NOMEM : TA_ERR_UNSUPPORTED;
        }
    }
    ta_result drained = in.error == TA_OK ? ta_input_value_drain(&in) : in.error;
    if (result == TA_OK && drained != TA_OK) {
        result = drained;
    }
    if (result != TA_OK) {
        ta_value_release(out);
        *out = (ta_value){ 0 };
    }
    return result;
}

/* Reads the record's value, if it has one. */
static ta_result ta_input_read_value(const ta_input_value_params *params) {
    *params->out = (ta_value){ 0 };
    return params->record->value_count == 0 ? TA_OK : ta_input_read_stream(params);
}

/* Reads past a record's value streams without decoding them. */
static ta_result ta_input_skip_values(const ta_input_record *record, const ta_byte_source *source) {
    for (uint16_t index = 0; index < record->value_count; index += 1u) {
        ta_input_value_in in = { .source = source };
        ta_result result = ta_input_value_drain(&in);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Replaying
 */

static ta_result ta_input_send_id(ta_system *system, ta_str text, ta_send_id *out) {
    ta_send_id id = ta_send_id_from_text(system, text);
    if (id == 0) {
        ta_atom atom = 0;
        ta_result result = ta_atom_intern(&system->atoms, text, &atom);
        if (result != TA_OK) {
            return result;
        }
        id = ta_send_id_from_text(system, text);
    }
    *out = id;
    return id != 0 ? TA_OK : TA_ERR_CORRUPT;
}

/* The registry index of the I/O processor registered for `type`, or 0. */
static uint32_t ta_input_processor(const ta_system *system, ta_str type) {
    ta_atom atom = 0;
    if (ta_atom_find(&system->atoms, type, &atom) != TA_OK) {
        return 0;
    }
    for (uint32_t index = 0; index < system->io_processors.count; index += 1u) {
        const ta_io_registration *registration = ta_vec_at(&system->io_processors, index);
        if (registration->type == atom) {
            return index + 1u;
        }
    }
    return 0;
}

/* The origin an event record names: an actor, an I/O processor's message
 * origin, or none. An event from an invocation gets its origin from core
 * when it is delivered (ADR 0068). */
static ta_result ta_input_origin(ta_system *system, const ta_input_record *record, ta_origin *out) {
    *out = (ta_origin){ .kind = TA_ORIGIN_NONE };
    ta_str origin = { 0 };
    ta_str type = { 0 };
    ta_str invoke = { 0 };
    if (ta_input_field_text(record, 6, &invoke) || !ta_input_field_text(record, 4, &origin) ||
        !ta_input_field_text(record, 5, &type)) {
        return TA_OK;
    }
    if (ta_str_equal(type, ta_atom_text(&system->atoms, system->scxml_io_type))) {
        ta_scxml_target target = { 0 };
        ta_result result = ta_scxml_target_parse(system, origin, &target);
        if (result == TA_ERR_NOMEM) {
            return result;
        }
        if (result == TA_OK && target.kind == TA_SCXML_TARGET_ACTOR) {
            *out = (ta_origin){ .kind = TA_ORIGIN_ACTOR, .as.actor = target.as.actor };
            return TA_OK;
        }
        if (result == TA_OK) {
            ta_scxml_target_release(&target);
        }
        return TA_OK;
    }
    uint32_t processor = ta_input_processor(system, type);
    if (processor == 0) {
        return TA_OK;
    }
    return ta_io_origin_make(system, &(ta_io_origin_make_params){
                                         .processor = processor,
                                         .origin_class = TA_IO_ORIGIN_MESSAGE,
                                         .uri = origin,
                                         .out = out,
                                     });
}

typedef struct {
    const ta_input_record *record;
    const ta_byte_source *source;
    const ta_definition *definition;
} ta_input_rebuild_params;

/* The event of an EVENT, JUMP or QUEUED record, owned by the caller. */
static ta_result ta_input_rebuild_event(ta_system *system, const ta_input_rebuild_params *params, ta_event *out) {
    const ta_input_record *record = params->record;
    *out = (ta_event){ .invoke_id = TA_INVOKE_ID_NONE };
    uint64_t type = ta_input_field(record, 0);
    ta_str name = { 0 };
    ta_str text = { 0 };
    if (type > (uint64_t)TA_EVENT_PLATFORM || !ta_input_field_text(record, 2, &name) || name.len == 0) {
        return TA_ERR_CORRUPT;
    }
    out->type = (ta_event_type)type;
    ta_result result = ta_event_name_intern_dotted(&system->event_names, name, &out->name);
    if (result == TA_OK && out->name == TA_EVENT_NAME_ROOT) {
        result = TA_ERR_CORRUPT;
    }
    if (result == TA_OK && ta_input_field_text(record, 3, &text)) {
        result = ta_input_send_id(system, text, &out->send_id);
    }
    if (result == TA_OK && ta_input_field_text(record, 6, &text)) {
        result = ta_invoke_id_from_text(system, text, &out->invoke_id);
    }
    if (result == TA_OK) {
        result = ta_input_origin(system, record, &out->origin);
    }
    if (result == TA_OK) {
        result = ta_input_read_value(&(ta_input_value_params){ .record = record,
                                                               .source = params->source,
                                                               .definition = params->definition,
                                                               .out = &out->data });
    }
    if (result != TA_OK) {
        ta_event_release(out);
        *out = (ta_event){ .invoke_id = TA_INVOKE_ID_NONE };
    }
    return result;
}

static bool ta_input_invoke_active(const ta_actor *actor, ta_invoke_id invoke) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1u) {
        if (sparse->invokes[index].id == invoke) {
            return true;
        }
    }
    return false;
}

/* A state of `definition` by its ID text. */
static ta_result ta_input_state(const ta_definition *definition, ta_str id, ta_state_id *out) {
    ta_atom name = 0;
    if (ta_atom_find(&definition->system->atoms, id, &name) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        if (definition->states[state].name == name) {
            *out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

typedef struct {
    ta_actor *actor;
    const ta_input_record *record;
    const ta_byte_source *source;
    const ta_actor_replay_params *params;
} ta_input_context;

/* JUMP: its operator transition, with targets in memory the caller frees. */
typedef struct {
    ta_operator_transition transition;
    ta_state_id *targets;
    size_t bytes;
} ta_input_jump;

static ta_result ta_input_rebuild_jump(ta_system *system, const ta_input_context *apply, ta_input_jump *out) {
    const ta_input_record *record = apply->record;
    const ta_actor *actor = apply->actor;
    *out = (ta_input_jump){ .transition = { .source = TA_STATE_NONE } };
    ta_str text = { 0 };
    ta_result result = TA_OK;
    if (ta_input_field_text(record, 7, &text)) {
        result = ta_input_state(actor->definition, text, &out->transition.source);
    }
    uint32_t count = (uint32_t)ta_input_field(record, 8);
    if (result != TA_OK || count == 0) {
        return result != TA_OK ? result : TA_ERR_CORRUPT;
    }
    out->bytes = (size_t)count * sizeof(ta_state_id);
    out->targets = ta_mem_alloc(system->allocator, out->bytes, _Alignof(ta_state_id));
    if (out->targets == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < count && result == TA_OK; index += 1u) {
        if (!ta_input_field_text(record, 9u + index, &text)) {
            result = TA_ERR_CORRUPT;
        } else {
            result = ta_input_state(actor->definition, text, &out->targets[index]);
        }
    }
    out->transition.targets = out->targets;
    out->transition.target_count = count;
    return result;
}

static void ta_input_jump_free(ta_system *system, ta_input_jump *jump) {
    if (jump->targets != NULL) {
        ta_mem_free(system->allocator, jump->targets,
                    &(ta_allocator_free_params){ .size = jump->bytes, .alignment = _Alignof(ta_state_id) });
    }
}

static ta_result ta_input_apply_event(ta_system *system, const ta_input_context *apply) {
    const ta_input_record *record = apply->record;
    ta_event event;
    ta_result result = ta_input_rebuild_event(
        system, &(ta_input_rebuild_params){ .record = record, .source = apply->source, .definition = apply->actor->definition },
        &event);
    if (result != TA_OK) {
        return result;
    }
    ta_input_jump jump = { .transition = { .source = TA_STATE_NONE } };
    if (record->kind == TA_RECORD_JUMP) {
        result = ta_input_rebuild_jump(system, apply, &jump);
    }
    if (result == TA_OK && event.invoke_id != TA_INVOKE_ID_NONE && !ta_input_invoke_active(apply->actor, event.invoke_id)) {
        /* The invocation it came from is not active in the replay (it was
         * never started, or it ended): the event arrives without it. */
        event.invoke_id = TA_INVOKE_ID_NONE;
    }
    if (result == TA_OK) {
        result = ta_actor_deliver(system, &(ta_actor_deliver_params){
                                              .actor = apply->actor->record.id,
                                              .event = &event,
                                              .transition = record->kind == TA_RECORD_JUMP ? &jump.transition : NULL,
                                          });
    }
    ta_input_jump_free(system, &jump);
    ta_event_release(&event);
    uint64_t host_seq = ta_input_field(record, 1);
    if (result == TA_OK && host_seq != 0 && apply->params->host != NULL) {
        *apply->params->host = (ta_host_record){ .kind = record->kind, .host_seq = host_seq };
    }
    return result;
}

static ta_result ta_input_apply_report(ta_system *system, const ta_input_context *apply) {
    const ta_input_record *record = apply->record;
    ta_str invoke_text = { 0 };
    ta_str reason = { 0 };
    if (ta_input_field(record, 0) != (uint64_t)TA_INVOKE_REPORT_ERROR ||
        !ta_input_field_text(record, 2, &invoke_text) || !ta_input_field_text(record, 3, &reason)) {
        return TA_ERR_CORRUPT;
    }
    ta_invoke_id invoke = TA_INVOKE_ID_NONE;
    ta_result result = ta_invoke_id_from_text(system, invoke_text, &invoke);
    ta_value data = { 0 };
    if (result == TA_OK) {
        result = ta_input_read_value(&(ta_input_value_params){
            .record = record, .source = apply->source, .definition = apply->actor->definition, .out = &data });
    }
    if (result == TA_OK) {
        ta_invoke_error error = { .reason = reason, .data = data.v != NULL ? &data : NULL,
                                  .ends = ta_input_field(record, 1) != 0 };
        result = ta_actor_report_invoke(system, &(ta_invoke_report_params){
                                                    .actor = apply->actor->record.id,
                                                    .invoke_id = invoke,
                                                    .kind = TA_INVOKE_REPORT_ERROR,
                                                    .as.error = &error,
                                                });
    }
    ta_value_release(&data);
    return result;
}

static ta_result ta_input_apply_host(ta_system *system, const ta_input_context *apply, ta_replay_step *step) {
    const ta_input_record *record = apply->record;
    ta_host_record host = { .kind = record->kind };
    ta_result result = TA_OK;
    if (record->kind == TA_RECORD_QUEUED) {
        host.host_seq = ta_input_field(record, 1);
        result = ta_input_rebuild_event(
            system, &(ta_input_rebuild_params){ .record = record, .source = apply->source, .definition = apply->actor->definition },
            &host.event);
        host.has_event = result == TA_OK;
    } else {
        host.host_seq = ta_input_field(record, 0);
    }
    if (result != TA_OK) {
        return result;
    }
    if (apply->params->host != NULL) {
        *apply->params->host = host;
    } else if (host.has_event) {
        ta_event_release(&host.event);
    }
    *step = TA_REPLAY_HOST;
    return TA_OK;
}

static ta_result ta_input_apply(ta_system *system, const ta_input_context *apply, ta_replay_step *step) {
    const ta_input_record *record = apply->record;
    ta_actor_id id = apply->actor->record.id;
    ta_str text = { 0 };
    *step = TA_REPLAY_NEXT;
    switch (record->kind) {
        case TA_RECORD_ABANDONED: {
            /* The macrostep it ended is still open in the replay. */
            ta_result result = ta_actor_abandon(system, id);
            return result == TA_OK ? ta_actor_set_time(system, id, record->time) : result;
        }
        case TA_RECORD_RELEASED:
            *step = TA_REPLAY_RELEASE;
            return TA_OK;
        case TA_RECORD_FINISHED:
            *step = TA_REPLAY_FINISHED;
            return TA_OK;
        case TA_RECORD_DESTROYED:
            *step = TA_REPLAY_ENDED;
            return TA_OK;
        case TA_RECORD_QUEUED:
        case TA_RECORD_DROPPED:
            return ta_input_apply_host(system, apply, step);
        case TA_RECORD_SPAWNED:
        case TA_RECORD_CHECKPOINT:
        case TA_RECORD_CHECKPOINT_HISTORY:
        case TA_RECORD_CHECKPOINT_SEND:
        case TA_RECORD_CHECKPOINT_INVOKE:
            /* A checkpoint past what the replay applied: prepare from it. */
            return TA_ERR_INVALID;
        default:
            break;
    }
    if (ta_input_runaway_stopped(system, apply->actor)) {
        return TA_ERR_BUSY;
    }
    ta_result result = ta_actor_set_time(system, id, record->time);
    if (result != TA_OK) {
        return result;
    }
    switch (record->kind) {
        case TA_RECORD_EVENT:
        case TA_RECORD_JUMP:
            result = ta_input_apply_event(system, apply);
            *step = TA_REPLAY_RUN;
            return result;
        case TA_RECORD_REPORT:
            result = ta_input_apply_report(system, apply);
            *step = TA_REPLAY_RUN;
            return result;
        case TA_RECORD_WITHDRAWN: {
            ta_send_id send = 0;
            if (!ta_input_field_text(record, 0, &text)) {
                return TA_ERR_CORRUPT;
            }
            result = ta_input_send_id(system, text, &send);
            return result == TA_OK ? ta_actor_withdraw_send(system, id, send) : result;
        }
        default:
            /* FIRED and EVICTED move the clock: the time they record reached
             * the held sends due by then. */
            return TA_OK;
    }
}

ta_result ta_actor_replay(ta_system *system, const ta_actor_replay_params *params) {
    if (system == NULL || params == NULL || params->step == NULL || params->source.v == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!ta_actor_replaying(actor)) {
        return TA_ERR_INVALID;
    }
    if ((actor->macrostep || ta_actor_external_event(actor) != NULL) && !ta_input_runaway_stopped(system, actor)) {
        return TA_ERR_BUSY;
    }
    if (params->host != NULL) {
        *params->host = (ta_host_record){ 0 };
    }
    ta_input_record record;
    ta_result result = ta_input_read_record(system, &params->source, &record);
    if (result != TA_OK) {
        return result;
    }
    result = ta_input_prepare_actor(system, actor);
    ta_activation_effects *effects = result == TA_OK ? actor->sparse->activation_effects : NULL;
    bool covered = result == TA_OK && record.seq != 0 && record.seq <= actor->sparse->log_seq;
    bool owed = covered && record.kind == TA_RECORD_QUEUED && effects != NULL && effects->checkpoint_owed != 0;
    ta_input_context apply = { .actor = actor, .record = &record, .source = &params->source, .params = params };
    if (owed) {
        /* A message the checkpoint the replay started from owes (ADR 0091). */
        result = ta_input_apply_host(system, &apply, params->step);
        effects->checkpoint_owed -= result == TA_OK ? 1u : 0u;
    } else if (covered) {
        /* The actor has this record already (ADR 0091 section 3). */
        *params->step = TA_REPLAY_NEXT;
        result = ta_input_skip_values(&record, &params->source);
    } else if (result == TA_OK) {
        result = ta_input_apply(system, &apply, params->step);
    }
    if (result == TA_OK && record.seq != 0 && actor->sparse->log_seq < record.seq) {
        actor->sparse->log_seq = record.seq;
    }
    ta_input_record_free(system, &record);
    return result;
}

void ta_input_spawn_release(ta_system *system, ta_input_spawn *spawn) {
    ta_value_release(&spawn->binding);
    spawn->binding = (ta_value){ 0 };
    ta_input_record_free(system, &spawn->checkpoint);
}

static bool ta_input_revision_is(const ta_input_record *record, const ta_definition *definition) {
    ta_str revision = { 0 };
    return ta_input_field_text(record, 2, &revision) &&
           ta_str_equal(revision, (ta_str){ .ptr = (const char *)definition->identity.revision.ptr,
                                            .len = definition->identity.revision.len });
}

ta_result ta_input_read_spawn(ta_system *system, const ta_input_spawn_params *params) {
    ta_input_record record;
    ta_result result = ta_input_read_record(system, params->log, &record);
    if (result != TA_OK) {
        return result;
    }
    ta_str family = { 0 };
    if (record.kind != TA_RECORD_SPAWNED && record.kind != TA_RECORD_CHECKPOINT) {
        result = TA_ERR_INVALID;
    } else if (!ta_input_field_text(&record, 0, &family) ||
               !ta_str_equal(family, params->definition->identity.family.name)) {
        result = TA_ERR_INVALID;
    } else if (record.kind == TA_RECORD_CHECKPOINT && !ta_input_revision_is(&record, params->definition)) {
        /* A checkpoint holds its revision's states and data (ADR 0091). */
        result = TA_ERR_DEFINITION_MISMATCH;
    }
    ta_input_spawn *out = params->out;
    *out = (ta_input_spawn){ .kind = record.kind, .time = record.time, .seq = record.seq };
    if (result == TA_OK) {
        result = ta_input_read_value(&(ta_input_value_params){
            .record = &record, .source = params->log, .definition = params->definition, .out = &out->binding });
        out->has_binding = result == TA_OK && out->binding.v != NULL;
    }
    if (result == TA_OK && record.kind == TA_RECORD_CHECKPOINT) {
        out->checkpoint = record;
        return TA_OK;
    }
    ta_input_record_free(system, &record);
    return result;
}

/* ------------------------------------------------------------------------
 * Checkpoints (ADR 0091)
 */

typedef enum { TA_LIST_ACTIVE, TA_LIST_LATE, TA_LIST_HISTORY } ta_input_list_kind;

/* The states of one of an actor's lists, asked for in order. */
typedef struct {
    const ta_actor *actor;
    ta_input_list_kind kind;
    uint32_t slot;
    uint32_t next_index; /* the index the cursor answers next */
    ta_state_id cursor;  /* where to look for it */
} ta_input_state_list;

static bool ta_input_listed(const ta_input_state_list *list, ta_state_id state) {
    const ta_actor *actor = list->actor;
    switch (list->kind) {
        case TA_LIST_ACTIVE:
            return ta_configuration_contains_atomic(&actor->configuration, state);
        case TA_LIST_LATE:
            return ta_bits_get(actor->late_initialized, state);
        default:
            return ta_bits_get(ta_history_slot_words(&actor->history, list->slot), state);
    }
}

static uint32_t ta_input_list_count(ta_input_state_list *list) {
    uint32_t count = 0;
    for (ta_state_id state = 0; state < list->actor->definition->state_count; state += 1u) {
        count += ta_input_listed(list, state) ? 1u : 0u;
    }
    return count;
}

static ta_state_id ta_input_list_at(void *context, uint32_t index) {
    ta_input_state_list *list = context;
    if (index < list->next_index) {
        list->next_index = 0;
        list->cursor = 0;
    }
    for (ta_state_id state = list->cursor; state < list->actor->definition->state_count; state += 1u) {
        if (!ta_input_listed(list, state)) {
            continue;
        }
        if (list->next_index == index) {
            list->next_index = index + 1u;
            list->cursor = state + 1u;
            return state;
        }
        list->next_index += 1u;
    }
    return 0; /* not reached: the count was taken from the same lists */
}

/* The actor's ascent over the list texts of a checkpoint header: the
 * active states, then the late-initialized ones, through one cursor each. */
typedef struct {
    ta_input_state_list active;
    ta_input_state_list late;
    uint32_t active_count;
} ta_input_header_lists;

static ta_state_id ta_input_header_at(void *context, uint32_t index) {
    ta_input_header_lists *lists = context;
    return index < lists->active_count ? ta_input_list_at(&lists->active, index)
                                       : ta_input_list_at(&lists->late, index - lists->active_count);
}

/* A pending send, as a CHECKPOINT_SEND part. */
static ta_result ta_input_emit_send(ta_system *system, const ta_actor *actor, uint64_t seq,
                                    const ta_delayed_send_info *send) {
    const ta_definition *definition = actor->definition;
    ta_record_event_texts texts = { .system = system, .name = send->event, .send_id = send->send_id };
    bool scxml = ta_system_is_scxml_io(system, send->io_type);
    bool self = scxml && send->destination.kind == TA_ACTOR_REF_RUNTIME &&
                (send->destination.as.runtime == actor->session_id ||
                 send->destination.as.runtime == actor->record.id);
    bool destination = scxml && !self &&
                       (send->destination.kind == TA_ACTOR_REF_NAMED || send->destination.as.runtime != 0);
    if (destination) {
        texts.origin = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR, .as.actor = send->destination };
    }
    uint64_t flags = (send->data.v != NULL ? 1u : 0u) | (send->target.v != NULL ? 2u : 0u) |
                     (send->has_target_text ? 4u : 0u) | (send->has_content ? 8u : 0u) | (self ? 16u : 0u);
    ta_record_desc record = { .kind = TA_RECORD_CHECKPOINT_SEND,
                              .time = ta_actor_now(system, actor),
                              .value = &send->data,
                              .value2 = &send->target,
                              .definition = definition };
    ta_input_send_field(&texts, send->send_id, &record);
    record.fields[record.field_count++] = (ta_record_field){
        .is_text = true, .text = { .present = true, .write = ta_input_event_name_write, .context = &texts }
    };
    record.fields[record.field_count++] = ta_input_str(true, ta_atom_text(&system->atoms, send->io_type));
    record.fields[record.field_count++] = (ta_record_field){
        .is_text = true, .text = { .present = destination, .write = ta_input_origin_write, .context = &texts }
    };
    record.fields[record.field_count++] = ta_input_number(send->due);
    record.fields[record.field_count++] = ta_input_number(flags);
    record.fields[record.field_count++] = ta_input_number(send->has_target_text ? send->target_text : 0u);
    record.fields[record.field_count++] = ta_input_number(send->has_content ? send->content : 0u);
    return ta_input_emit(system, seq, &record);
}

typedef struct {
    ta_actor *actor;
    const ta_actor_checkpoint_params *params;
    const ta_value *root;
    uint64_t seq;
} ta_input_checkpoint;

static uint32_t ta_input_history_parts(const ta_actor *actor) {
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < actor->definition->history.slot_count; slot += 1u) {
        ta_input_state_list list = { .actor = actor, .kind = TA_LIST_HISTORY, .slot = slot };
        count += ta_input_list_count(&list) != 0 ? 1u : 0u;
    }
    return count;
}

static uint32_t ta_input_send_parts(const ta_actor *actor) {
    const ta_timer_list *timers = actor->sparse != NULL ? &actor->sparse->timers : NULL;
    uint32_t count = 0;
    for (uint32_t index = 0; timers != NULL && index < timers->count; index += 1u) {
        count += timers->sends[index].submitted ? 0u : 1u;
    }
    return count;
}

static uint32_t ta_input_invoke_parts(const ta_actor *actor) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    uint32_t count = 0;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1u) {
        count += sparse->invokes[index].active ? 1u : 0u;
    }
    return count;
}

static ta_result ta_input_emit_header(ta_system *system, const ta_input_checkpoint *checkpoint) {
    const ta_actor *actor = checkpoint->actor;
    const ta_definition *definition = actor->definition;
    ta_actor_address address = ta_address_view(actor->record.self.as.named);
    ta_input_header_lists lists = {
        .active = { .actor = actor, .kind = TA_LIST_ACTIVE },
        .late = { .actor = actor, .kind = TA_LIST_LATE },
    };
    lists.active_count = ta_input_list_count(&lists.active);
    uint32_t late_count = ta_input_list_count(&lists.late);
    uint32_t parts = ta_input_history_parts(actor) + ta_input_send_parts(actor) + ta_input_invoke_parts(actor);
    ta_record_desc record = { .kind = TA_RECORD_CHECKPOINT,
                              .time = ta_actor_now(system, actor),
                              .field_count = 11,
                              .target_count = lists.active_count + late_count,
                              .target_at = ta_input_header_at,
                              .target_context = &lists,
                              .value = checkpoint->root,
                              .definition = definition };
    record.fields[0] = ta_input_str(true, address.family.name);
    record.fields[1] = ta_input_str(true, address.name);
    record.fields[2] = ta_input_str(true, (ta_str){ .ptr = (const char *)definition->identity.revision.ptr,
                                                    .len = definition->identity.revision.len });
    record.fields[3] = ta_input_number(actor->next_send_id);
    record.fields[4] = ta_input_number(actor->trace_macrostep);
    record.fields[5] = ta_input_number(actor->trace_microstep);
    record.fields[6] = ta_input_number(actor->trace_lifecycle);
    record.fields[7] = ta_input_number(parts);
    record.fields[8] = ta_input_number(checkpoint->params->owed_count);
    record.fields[9] = ta_input_number(lists.active_count);
    record.fields[10] = ta_input_number(late_count);
    return ta_input_emit(system, checkpoint->seq, &record);
}

static ta_result ta_input_emit_parts(ta_system *system, const ta_input_checkpoint *checkpoint) {
    const ta_actor *actor = checkpoint->actor;
    const ta_definition *definition = actor->definition;
    ta_result result = TA_OK;
    for (ta_state_id state = 0; result == TA_OK && state < definition->state_count; state += 1u) {
        uint32_t slot = definition->states[state].history_slot;
        if (slot == UINT32_MAX) {
            continue;
        }
        ta_input_state_list list = { .actor = actor, .kind = TA_LIST_HISTORY, .slot = slot };
        uint32_t count = ta_input_list_count(&list);
        if (count == 0) {
            continue;
        }
        ta_record_desc record = { .kind = TA_RECORD_CHECKPOINT_HISTORY,
                                  .time = ta_actor_now(system, actor),
                                  .field_count = 2,
                                  .target_count = count,
                                  .target_at = ta_input_list_at,
                                  .target_context = &list,
                                  .definition = definition };
        record.fields[0] = ta_input_str(true, ta_input_state_text(system, definition, state));
        record.fields[1] = ta_input_number(count);
        result = ta_input_emit(system, checkpoint->seq, &record);
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    for (uint32_t index = 0; result == TA_OK && sparse != NULL && index < sparse->timers.count; index += 1u) {
        if (!sparse->timers.sends[index].submitted) {
            result = ta_input_emit_send(system, actor, checkpoint->seq, &sparse->timers.sends[index]);
        }
    }
    for (uint32_t index = 0; result == TA_OK && sparse != NULL && index < sparse->invoke_count; index += 1u) {
        const ta_active_invoke *invoke = &sparse->invokes[index];
        if (!invoke->active) {
            continue;
        }
        ta_record_desc record = { .kind = TA_RECORD_CHECKPOINT_INVOKE,
                                  .time = ta_actor_now(system, actor),
                                  .field_count = 3,
                                  .definition = definition };
        record.fields[0] = ta_input_str(true, ta_invoke_id_text(system, invoke->id));
        record.fields[1] = ta_input_str(true, ta_atom_text(&system->atoms, invoke->type));
        record.fields[2] = ta_input_number(sparse->invoke_meta[index].decl);
        result = ta_input_emit(system, checkpoint->seq, &record);
    }
    const ta_actor_checkpoint_params *params = checkpoint->params;
    for (uint32_t index = 0; result == TA_OK && index < params->owed_count; index += 1u) {
        const ta_owed_message *owed = &params->owed[index];
        ta_record_desc record = { .kind = TA_RECORD_QUEUED, .time = ta_actor_now(system, actor) };
        ta_record_event_texts texts;
        ta_input_event_fields(&(ta_record_event_params){ .system = system,
                                                         .actor = actor,
                                                         .event = owed->event,
                                                         .host_seq = owed->host_seq },
                              &texts, &record);
        record.value = &owed->event->data;
        record.definition = definition;
        result = ta_input_emit(system, checkpoint->seq, &record);
    }
    return result;
}

ta_result ta_actor_checkpoint(ta_system *system, const ta_actor_checkpoint_params *params) {
    if (system == NULL || params == NULL || params->sink.v == NULL ||
        (params->owed_count != 0 && params->owed == NULL)) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    for (uint32_t index = 0; index < params->owed_count; index += 1u) {
        if (params->owed[index].event == NULL || params->owed[index].host_seq == 0) {
            return TA_ERR_INVALID;
        }
    }
    if (!actor->published || actor->record.self.kind != TA_ACTOR_REF_NAMED || !actor->definition->input_logged) {
        return TA_ERR_INVALID;
    }
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (system->input_open || actor->macrostep || ta_actor_external_event(actor) != NULL ||
        ta_interpreter_internal_pending(actor)) {
        return TA_ERR_BUSY;
    }
    ta_value inspected = { 0 };
    const ta_value *root = params->root;
    if (root == NULL && actor->definition->datamodel.v->inspect != NULL) {
        const ta_definition *definition = actor->definition;
        if (definition->datamodel.v->inspect(definition->datamodel.self,
                                             &(ta_datamodel_inspect_params){ .shared = definition->datamodel_shared,
                                                                             .instance = actor->datamodel_instance,
                                                                             .host = &actor->host,
                                                                             .out = &inspected }) == TA_OK) {
            root = &inspected;
        }
    }
    ta_input_checkpoint checkpoint = {
        .actor = actor, .params = params, .root = root, .seq = actor->sparse != NULL ? actor->sparse->log_seq : 0
    };
    /* The checkpoint goes to its own sink, through the input log's writer:
     * nothing counts as an input, so no record number moves. */
    ta_byte_sink input_sink = system->input_sink;
    system->input_sink = params->sink;
    ta_result result = TA_ERR_NOMEM;
    if (params->sink.v->begin(params->sink.self) == TA_OP_OK) {
        system->input_open = true;
        system->input_status = TA_OP_OK;
        system->input_used = 0;
        system->input_actor = NULL;
        result = ta_input_emit_header(system, &checkpoint);
        if (result == TA_OK) {
            result = ta_input_emit_parts(system, &checkpoint);
        }
        ta_input_close(system, result == TA_OK);
    }
    system->input_sink = input_sink;
    ta_value_release(&inspected);
    return result;
}

/* ------------------------------------------------------------------------
 * Restoring a checkpoint into a replaying actor
 */

static ta_result ta_input_restore_lists(ta_actor *actor, const ta_input_record *record) {
    const ta_definition *definition = actor->definition;
    uint64_t active = ta_input_field(record, 9);
    uint64_t late = ta_input_field(record, 10);
    ta_configuration_clear(&actor->configuration);
    for (uint64_t index = 0; index < active + late; index += 1u) {
        ta_str id = { 0 };
        ta_state_id state = 0;
        if (!ta_input_field_text(record, 11u + (uint32_t)index, &id)) {
            return TA_ERR_CORRUPT;
        }
        ta_result result = ta_input_state(definition, id, &state);
        if (result != TA_OK) {
            return result;
        }
        if (index < active) {
            ta_configuration_set_atomic(&actor->configuration, state, true);
        } else {
            ta_bits_put(actor->late_initialized, state, true);
        }
    }
    return TA_OK;
}

static ta_result ta_input_restore_history(ta_actor *actor, const ta_input_record *record) {
    const ta_definition *definition = actor->definition;
    ta_str id = { 0 };
    ta_state_id history = 0;
    if (!ta_input_field_text(record, 0, &id)) {
        return TA_ERR_CORRUPT;
    }
    ta_result result = ta_input_state(definition, id, &history);
    if (result != TA_OK) {
        return result;
    }
    uint32_t slot = definition->states[history].history_slot;
    if (slot == UINT32_MAX) {
        return TA_ERR_NOT_FOUND;
    }
    uint64_t *words = ta_history_slot_words(&actor->history, slot);
    uint64_t count = ta_input_field(record, 1);
    for (uint64_t index = 0; index < count; index += 1u) {
        ta_state_id state = 0;
        if (!ta_input_field_text(record, 2u + (uint32_t)index, &id)) {
            return TA_ERR_CORRUPT;
        }
        result = ta_input_state(definition, id, &state);
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(words, state, true);
    }
    return TA_OK;
}

static ta_result ta_input_restore_send(ta_system *system, ta_actor *actor, const ta_input_restore_params *params,
                                       const ta_input_record *record) {
    const ta_definition *definition = actor->definition;
    uint64_t flags = ta_input_field(record, 5);
    ta_delayed_send send = { .owner = actor->record.id,
                             .due = ta_input_field(record, 4),
                             .has_target_text = (flags & 4u) != 0,
                             .target_text = (ta_content_id)ta_input_field(record, 6),
                             .has_content = (flags & 8u) != 0,
                             .content = (ta_content_id)ta_input_field(record, 7),
                             .invoke_id = TA_INVOKE_ID_NONE };
    ta_str text = { 0 };
    ta_str name = { 0 };
    ta_str type = { 0 };
    if ((send.has_target_text && send.target_text >= definition->content_count) ||
        (send.has_content && send.content >= definition->content_count) || !ta_input_field_text(record, 1, &name) ||
        !ta_input_field_text(record, 2, &type) ||
        (unsigned)(((flags & 1u) != 0) + ((flags & 2u) != 0)) != record->value_count) {
        return TA_ERR_CORRUPT;
    }
    ta_result result = TA_OK;
    if (ta_input_field_text(record, 0, &text)) {
        result = ta_input_send_id(system, text, &send.send_id);
    }
    if (result == TA_OK) {
        result = ta_event_name_intern_dotted(&system->event_names, name, &send.event);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, type, &send.io_type);
    }
    if (result == TA_OK && (flags & 16u) != 0) {
        send.destination = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME,
                                           .family = definition->family,
                                           .as.runtime = actor->record.id };
    } else if (result == TA_OK && ta_input_field_text(record, 3, &text)) {
        ta_scxml_target target = { 0 };
        result = ta_scxml_target_parse(system, text, &target);
        if (result == TA_OK && target.kind != TA_SCXML_TARGET_ACTOR) {
            ta_scxml_target_release(&target);
            result = TA_ERR_CORRUPT;
        }
        if (result == TA_OK) {
            send.destination = target.as.actor; /* the target's reference passes to the send */
        }
    }
    const ta_input_value_params value = { .record = record, .source = params->log, .definition = definition };
    if (result == TA_OK && (flags & 1u) != 0) {
        result = ta_input_read_stream(&(ta_input_value_params){
            .record = value.record, .source = value.source, .definition = value.definition, .out = &send.data });
    }
    if (result == TA_OK && (flags & 2u) != 0) {
        result = ta_input_read_stream(&(ta_input_value_params){
            .record = value.record, .source = value.source, .definition = value.definition, .out = &send.target });
    }
    if (result == TA_OK) {
        result = ta_actor_hold_send(system, actor, &(ta_activation_send){ .send = send, .immediate = false });
    }
    if (result != TA_OK) {
        ta_value_release(&send.data);
        ta_value_release(&send.target);
        ta_actor_ref_release(&send.destination);
    }
    return result;
}

static ta_result ta_input_restore_invoke(ta_system *system, ta_actor *actor, const ta_input_record *record) {
    ta_str id = { 0 };
    ta_str type = { 0 };
    uint64_t decl = ta_input_field(record, 2);
    if (!ta_input_field_text(record, 0, &id) || !ta_input_field_text(record, 1, &type)) {
        return TA_ERR_CORRUPT;
    }
    if (decl >= actor->definition->invoke_decl_count) {
        return TA_ERR_NOT_FOUND;
    }
    ta_invoke_restore_params restore = { .decl = (ta_invoke_decl_id)decl };
    ta_result result = ta_invoke_id_from_text(system, id, &restore.id);
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, type, &restore.type);
    }
    return result == TA_OK ? ta_invoke_restore(system, actor, &restore) : result;
}

static ta_result ta_input_restore_part(ta_system *system, ta_actor *actor, const ta_input_restore_params *params) {
    ta_input_record record;
    ta_result result = ta_input_read_record(system, params->log, &record);
    if (result != TA_OK) {
        return result;
    }
    if (record.seq != params->spawn->seq) {
        result = TA_ERR_CORRUPT;
    } else if (record.kind == TA_RECORD_CHECKPOINT_HISTORY) {
        result = ta_input_restore_history(actor, &record);
    } else if (record.kind == TA_RECORD_CHECKPOINT_SEND) {
        result = ta_input_restore_send(system, actor, params, &record);
    } else if (record.kind == TA_RECORD_CHECKPOINT_INVOKE) {
        result = ta_input_restore_invoke(system, actor, &record);
    } else {
        result = TA_ERR_CORRUPT;
    }
    if (result == TA_OK && record.kind != TA_RECORD_CHECKPOINT_SEND) {
        result = ta_input_skip_values(&record, params->log);
    }
    ta_input_record_free(system, &record);
    return result;
}

ta_result ta_input_restore(ta_system *system, ta_actor *actor, const ta_input_restore_params *params) {
    const ta_input_record *record = &params->spawn->checkpoint;
    ta_result result = ta_input_restore_lists(actor, record);
    if (result != TA_OK) {
        return result;
    }
    /* It starts idle: the state it restores is past its bootstrap. */
    actor->bootstrapped = true;
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_IDLE;
    actor->next_send_id = ta_input_field(record, 3);
    actor->trace_macrostep = ta_input_field(record, 4);
    actor->trace_microstep = (uint32_t)ta_input_field(record, 5);
    actor->trace_lifecycle = (uint32_t)ta_input_field(record, 6);
    uint64_t parts = ta_input_field(record, 7);
    for (uint64_t index = 0; result == TA_OK && index < parts; index += 1u) {
        result = ta_input_restore_part(system, actor, params);
    }
    if (result == TA_OK) {
        uint64_t owed = ta_input_field(record, 8);
        actor->sparse->activation_effects->checkpoint_owed = owed > UINT32_MAX ? UINT32_MAX : (uint32_t)owed;
    }
    return result;
}

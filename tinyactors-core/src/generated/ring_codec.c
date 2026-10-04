/* The record codec: see ring_codec.h. Generated from protocol/catalog.rb
 * by `mise run protocol` in tinyactors-core. Do not edit. */
#include "ring_codec.h"

/* One kind as the reader and the writers check it. */
typedef struct {
    uint16_t kind;
    uint8_t ring;
    uint8_t span_count;
    uint32_t fixed_size;
    /* Its first span offset in ta_record_span_offsets. */
    uint32_t span_first;
} ta_record_descriptor;

/* Every kind's span offsets, a run per kind, in catalog order. */
static const uint32_t ta_record_span_offsets[] = {
    /* h2c.event */
    (uint32_t)offsetof(ta_h2c_event, target),
    (uint32_t)offsetof(ta_h2c_event, name),
    (uint32_t)offsetof(ta_h2c_event, sendid),
    (uint32_t)offsetof(ta_h2c_event, origin),
    (uint32_t)offsetof(ta_h2c_event, origin_type),
    (uint32_t)offsetof(ta_h2c_event, invokeid),
    /* h2c.spawn */
    (uint32_t)offsetof(ta_h2c_spawn, address),
    (uint32_t)offsetof(ta_h2c_spawn, revision),
    /* h2c.publish */
    (uint32_t)offsetof(ta_h2c_publish, address),
    /* h2c.load */
    (uint32_t)offsetof(ta_h2c_load, address),
    /* h2c.load_failed */
    (uint32_t)offsetof(ta_h2c_load_failed, address),
    (uint32_t)offsetof(ta_h2c_load_failed, reason),
    /* h2c.unload */
    (uint32_t)offsetof(ta_h2c_unload, address),
    /* h2c.checkpoint */
    (uint32_t)offsetof(ta_h2c_checkpoint, address),
    /* h2c.invoke_report */
    (uint32_t)offsetof(ta_h2c_invoke_report, invokeid),
    (uint32_t)offsetof(ta_h2c_invoke_report, reason),
    /* h2c.destroy */
    (uint32_t)offsetof(ta_h2c_destroy, address),
    /* h2c.type_declare */
    (uint32_t)offsetof(ta_h2c_type_declare, type),
    /* c2h.spawned */
    (uint32_t)offsetof(ta_c2h_spawned, address),
    (uint32_t)offsetof(ta_c2h_spawned, revision),
    /* c2h.input */
    (uint32_t)offsetof(ta_c2h_input, name),
    (uint32_t)offsetof(ta_c2h_input, sendid),
    (uint32_t)offsetof(ta_c2h_input, origin),
    (uint32_t)offsetof(ta_c2h_input, origin_type),
    (uint32_t)offsetof(ta_c2h_input, invokeid),
    (uint32_t)offsetof(ta_c2h_input, report_invokeid),
    (uint32_t)offsetof(ta_c2h_input, report_reason),
    /* c2h.send */
    (uint32_t)offsetof(ta_c2h_send, sendid),
    (uint32_t)offsetof(ta_c2h_send, io_type),
    (uint32_t)offsetof(ta_c2h_send, target),
    (uint32_t)offsetof(ta_c2h_send, name),
    /* c2h.invoke */
    (uint32_t)offsetof(ta_c2h_invoke, invokeid),
    (uint32_t)offsetof(ta_c2h_invoke, type),
    (uint32_t)offsetof(ta_c2h_invoke, src),
    /* c2h.invoke_cancel */
    (uint32_t)offsetof(ta_c2h_invoke_cancel, invokeid),
    /* c2h.invoke_forward */
    (uint32_t)offsetof(ta_c2h_invoke_forward, invokeid),
    (uint32_t)offsetof(ta_c2h_invoke_forward, name),
    (uint32_t)offsetof(ta_c2h_invoke_forward, sendid),
    (uint32_t)offsetof(ta_c2h_invoke_forward, origin),
    (uint32_t)offsetof(ta_c2h_invoke_forward, origin_type),
    (uint32_t)offsetof(ta_c2h_invoke_forward, event_invokeid),
    /* c2h.log */
    (uint32_t)offsetof(ta_c2h_log, label),
    /* c2h.bounced */
    (uint32_t)offsetof(ta_c2h_bounced, detail),
    /* c2h.load_needed */
    (uint32_t)offsetof(ta_c2h_load_needed, address),
    /* c2h.loaded */
    (uint32_t)offsetof(ta_c2h_loaded, address),
    /* c2h.unloaded */
    (uint32_t)offsetof(ta_c2h_unloaded, address),
    /* c2h.state */
    (uint32_t)offsetof(ta_c2h_state, address),
    /* c2h.published */
    (uint32_t)offsetof(ta_c2h_published, address),
    /* c2h.destroyed */
    (uint32_t)offsetof(ta_c2h_destroyed, address),
    /* tel.trace */
    (uint32_t)offsetof(ta_tel_trace, address),
    (uint32_t)offsetof(ta_tel_trace, definition),
    (uint32_t)offsetof(ta_tel_trace, state_name),
    (uint32_t)offsetof(ta_tel_trace, transition_name),
    (uint32_t)offsetof(ta_tel_trace, action_name),
    (uint32_t)offsetof(ta_tel_trace, name),
    (uint32_t)offsetof(ta_tel_trace, send_id),
    (uint32_t)offsetof(ta_tel_trace, invoke_id),
    (uint32_t)offsetof(ta_tel_trace, value_text),
    (uint32_t)offsetof(ta_tel_trace, fault_detail),
    (uint32_t)offsetof(ta_tel_trace, fault_message),
    (uint32_t)offsetof(ta_tel_trace, transitions),
    (uint32_t)offsetof(ta_tel_trace, states),
};

/* One descriptor per kind, in catalog order. */
static const ta_record_descriptor ta_record_descriptors[] = {
    { TA_RECORD_KIND_H2C_TIME, TA_RECORD_RING_H2C, 0u, (uint32_t)sizeof(ta_h2c_time), 0u },
    { TA_RECORD_KIND_H2C_EVENT, TA_RECORD_RING_H2C, 6u, (uint32_t)sizeof(ta_h2c_event), 0u },
    { TA_RECORD_KIND_H2C_SPAWN, TA_RECORD_RING_H2C, 2u, (uint32_t)sizeof(ta_h2c_spawn), 6u },
    { TA_RECORD_KIND_H2C_PUBLISH, TA_RECORD_RING_H2C, 1u, (uint32_t)sizeof(ta_h2c_publish), 8u },
    { TA_RECORD_KIND_H2C_LOAD, TA_RECORD_RING_H2C, 1u, (uint32_t)sizeof(ta_h2c_load), 9u },
    { TA_RECORD_KIND_H2C_LOAD_FAILED, TA_RECORD_RING_H2C, 2u, (uint32_t)sizeof(ta_h2c_load_failed), 10u },
    { TA_RECORD_KIND_H2C_UNLOAD, TA_RECORD_RING_H2C, 1u, (uint32_t)sizeof(ta_h2c_unload), 12u },
    { TA_RECORD_KIND_H2C_CHECKPOINT, TA_RECORD_RING_H2C, 1u, (uint32_t)sizeof(ta_h2c_checkpoint), 13u },
    { TA_RECORD_KIND_H2C_INVOKE_REPORT, TA_RECORD_RING_H2C, 2u, (uint32_t)sizeof(ta_h2c_invoke_report), 14u },
    { TA_RECORD_KIND_H2C_DESTROY, TA_RECORD_RING_H2C, 1u, (uint32_t)sizeof(ta_h2c_destroy), 16u },
    { TA_RECORD_KIND_H2C_DEFINITION_REGISTER, TA_RECORD_RING_H2C, 0u, (uint32_t)sizeof(ta_h2c_definition_register), 17u },
    { TA_RECORD_KIND_H2C_DEFINITION_RELEASE, TA_RECORD_RING_H2C, 0u, (uint32_t)sizeof(ta_h2c_definition_release), 17u },
    { TA_RECORD_KIND_H2C_DEFINITION_RETIRE, TA_RECORD_RING_H2C, 0u, (uint32_t)sizeof(ta_h2c_definition_retire), 17u },
    { TA_RECORD_KIND_H2C_WITHDRAW, TA_RECORD_RING_H2C, 0u, (uint32_t)sizeof(ta_h2c_withdraw), 17u },
    { TA_RECORD_KIND_H2C_TYPE_DECLARE, TA_RECORD_RING_H2C, 1u, (uint32_t)sizeof(ta_h2c_type_declare), 17u },
    { TA_RECORD_KIND_C2H_SPAWNED, TA_RECORD_RING_C2H, 2u, (uint32_t)sizeof(ta_c2h_spawned), 18u },
    { TA_RECORD_KIND_C2H_INPUT, TA_RECORD_RING_C2H, 7u, (uint32_t)sizeof(ta_c2h_input), 20u },
    { TA_RECORD_KIND_C2H_SEND, TA_RECORD_RING_C2H, 4u, (uint32_t)sizeof(ta_c2h_send), 27u },
    { TA_RECORD_KIND_C2H_INVOKE, TA_RECORD_RING_C2H, 3u, (uint32_t)sizeof(ta_c2h_invoke), 31u },
    { TA_RECORD_KIND_C2H_INVOKE_CANCEL, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_invoke_cancel), 34u },
    { TA_RECORD_KIND_C2H_INVOKE_FORWARD, TA_RECORD_RING_C2H, 6u, (uint32_t)sizeof(ta_c2h_invoke_forward), 35u },
    { TA_RECORD_KIND_C2H_LOG, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_log), 41u },
    { TA_RECORD_KIND_C2H_MACROSTEP_ENDED, TA_RECORD_RING_C2H, 0u, (uint32_t)sizeof(ta_c2h_macrostep_ended), 42u },
    { TA_RECORD_KIND_C2H_WAKE, TA_RECORD_RING_C2H, 0u, (uint32_t)sizeof(ta_c2h_wake), 42u },
    { TA_RECORD_KIND_C2H_BOUNCED, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_bounced), 42u },
    { TA_RECORD_KIND_C2H_LOAD_NEEDED, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_load_needed), 43u },
    { TA_RECORD_KIND_C2H_LOADED, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_loaded), 44u },
    { TA_RECORD_KIND_C2H_UNLOADED, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_unloaded), 45u },
    { TA_RECORD_KIND_C2H_STATE, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_state), 46u },
    { TA_RECORD_KIND_C2H_PUBLISHED, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_published), 47u },
    { TA_RECORD_KIND_C2H_FINISHED, TA_RECORD_RING_C2H, 0u, (uint32_t)sizeof(ta_c2h_finished), 48u },
    { TA_RECORD_KIND_C2H_DESTROYED, TA_RECORD_RING_C2H, 1u, (uint32_t)sizeof(ta_c2h_destroyed), 48u },
    { TA_RECORD_KIND_C2H_DEFINITION_RESULT, TA_RECORD_RING_C2H, 0u, (uint32_t)sizeof(ta_c2h_definition_result), 49u },
    { TA_RECORD_KIND_C2H_INPUT_REFUSED, TA_RECORD_RING_C2H, 0u, (uint32_t)sizeof(ta_c2h_input_refused), 49u },
    { TA_RECORD_KIND_C2H_INPUT_COMPLETED, TA_RECORD_RING_C2H, 0u, (uint32_t)sizeof(ta_c2h_input_completed), 49u },
    { TA_RECORD_KIND_C2H_LOAD_RESULT, TA_RECORD_RING_C2H, 0u, (uint32_t)sizeof(ta_c2h_load_result), 49u },
    { TA_RECORD_KIND_TEL_TRACE, TA_RECORD_RING_TEL, 13u, (uint32_t)sizeof(ta_tel_trace), 49u },
    { TA_RECORD_KIND_TEL_DROPPED, TA_RECORD_RING_TEL, 0u, (uint32_t)sizeof(ta_tel_dropped), 62u },
};

/* The index of `kind` in ta_record_descriptors, or their count when no
 * record has that kind. */
static uint32_t ta_record_descriptor_index(uint16_t kind) {
    switch (kind) {
    case TA_RECORD_KIND_H2C_TIME:
        return 0u;
    case TA_RECORD_KIND_H2C_EVENT:
        return 1u;
    case TA_RECORD_KIND_H2C_SPAWN:
        return 2u;
    case TA_RECORD_KIND_H2C_PUBLISH:
        return 3u;
    case TA_RECORD_KIND_H2C_LOAD:
        return 4u;
    case TA_RECORD_KIND_H2C_LOAD_FAILED:
        return 5u;
    case TA_RECORD_KIND_H2C_UNLOAD:
        return 6u;
    case TA_RECORD_KIND_H2C_CHECKPOINT:
        return 7u;
    case TA_RECORD_KIND_H2C_INVOKE_REPORT:
        return 8u;
    case TA_RECORD_KIND_H2C_DESTROY:
        return 9u;
    case TA_RECORD_KIND_H2C_DEFINITION_REGISTER:
        return 10u;
    case TA_RECORD_KIND_H2C_DEFINITION_RELEASE:
        return 11u;
    case TA_RECORD_KIND_H2C_DEFINITION_RETIRE:
        return 12u;
    case TA_RECORD_KIND_H2C_WITHDRAW:
        return 13u;
    case TA_RECORD_KIND_H2C_TYPE_DECLARE:
        return 14u;
    case TA_RECORD_KIND_C2H_SPAWNED:
        return 15u;
    case TA_RECORD_KIND_C2H_INPUT:
        return 16u;
    case TA_RECORD_KIND_C2H_SEND:
        return 17u;
    case TA_RECORD_KIND_C2H_INVOKE:
        return 18u;
    case TA_RECORD_KIND_C2H_INVOKE_CANCEL:
        return 19u;
    case TA_RECORD_KIND_C2H_INVOKE_FORWARD:
        return 20u;
    case TA_RECORD_KIND_C2H_LOG:
        return 21u;
    case TA_RECORD_KIND_C2H_MACROSTEP_ENDED:
        return 22u;
    case TA_RECORD_KIND_C2H_WAKE:
        return 23u;
    case TA_RECORD_KIND_C2H_BOUNCED:
        return 24u;
    case TA_RECORD_KIND_C2H_LOAD_NEEDED:
        return 25u;
    case TA_RECORD_KIND_C2H_LOADED:
        return 26u;
    case TA_RECORD_KIND_C2H_UNLOADED:
        return 27u;
    case TA_RECORD_KIND_C2H_STATE:
        return 28u;
    case TA_RECORD_KIND_C2H_PUBLISHED:
        return 29u;
    case TA_RECORD_KIND_C2H_FINISHED:
        return 30u;
    case TA_RECORD_KIND_C2H_DESTROYED:
        return 31u;
    case TA_RECORD_KIND_C2H_DEFINITION_RESULT:
        return 32u;
    case TA_RECORD_KIND_C2H_INPUT_REFUSED:
        return 33u;
    case TA_RECORD_KIND_C2H_INPUT_COMPLETED:
        return 34u;
    case TA_RECORD_KIND_C2H_LOAD_RESULT:
        return 35u;
    case TA_RECORD_KIND_TEL_TRACE:
        return 36u;
    case TA_RECORD_KIND_TEL_DROPPED:
        return 37u;
    default:
        return TA_ARRAY_COUNT(ta_record_descriptors);
    }
}

/* A writer's texts as one array of tails. */
typedef struct {
    uint16_t kind;
    uint32_t count;
    const ta_bytes *tails;
} ta_record_shape;

#define TA_RECORD_TAIL(text) { (const uint8_t *)(const void *)(text).ptr, (text).len }

/* The record's size: the fixed part plus the tails, rounded up to 8. */
static ta_result ta_record_measure(const ta_record_shape *shape, uint32_t *out) {
    uint32_t index = ta_record_descriptor_index(shape->kind);
    if (index == TA_ARRAY_COUNT(ta_record_descriptors)) {
        return TA_ERR_INVALID;
    }
    uint64_t size = ta_record_descriptors[index].fixed_size;
    for (uint32_t tail = 0; tail < shape->count; tail += 1u) {
        if (shape->tails[tail].ptr == NULL && shape->tails[tail].len != 0u) {
            return TA_ERR_INVALID;
        }
        size += shape->tails[tail].len;
    }
    size = (size + 7u) & ~(uint64_t)7u;
    if (size > UINT32_MAX) {
        return TA_ERR_INVALID;
    }
    *out = (uint32_t)size;
    return TA_OK;
}

/* Checks everything, then fills the record; see ring_codec.h. */
static ta_result ta_record_write(ta_record_buffer out, const void *fixed, const ta_record_shape *shape) {
    if (out.bytes == NULL || ((uintptr_t)(void *)out.bytes & 7u) != 0u || (out.size & 7u) != 0u ||
        fixed == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t needed = 0u;
    ta_result result = ta_record_measure(shape, &needed);
    if (result != TA_OK) {
        return result;
    }
    if (out.size < needed) {
        return TA_ERR_FULL;
    }
    const ta_record_descriptor *descriptor = &ta_record_descriptors[ta_record_descriptor_index(shape->kind)];
    uint64_t seq = 0u;
    ta_mem_copy(&seq, out.bytes + offsetof(ta_record_header, seq), sizeof(seq));
    if (fixed != (const void *)out.bytes) {
        ta_mem_copy(out.bytes, fixed, descriptor->fixed_size);
    }
    ta_mem_zero(out.bytes + descriptor->fixed_size, out.size - descriptor->fixed_size);
    uint16_t kind = shape->kind;
    ta_mem_copy(out.bytes + offsetof(ta_record_header, kind), &kind, sizeof(kind));
    ta_mem_copy(out.bytes + offsetof(ta_record_header, size), &out.size, sizeof(out.size));
    ta_mem_copy(out.bytes + offsetof(ta_record_header, seq), &seq, sizeof(seq));
    uint32_t cursor = descriptor->fixed_size;
    for (uint32_t tail = 0; tail < shape->count; tail += 1u) {
        ta_span32 span = { cursor, shape->tails[tail].len };
        ta_mem_copy(out.bytes + ta_record_span_offsets[descriptor->span_first + tail], &span, sizeof(span));
        if (span.length != 0u) {
            ta_mem_copy(out.bytes + cursor, shape->tails[tail].ptr, span.length);
        }
        cursor += span.length;
    }
    return TA_OK;
}

ta_result ta_record_read(const ta_record_read_params *params, ta_record_view *out) {
    if (params == NULL || out == NULL || params->bytes == NULL ||
        ((uintptr_t)(const void *)params->bytes & 7u) != 0u) {
        return TA_ERR_INVALID;
    }
    if ((uint32_t)params->ring == 0u || (uint32_t)params->ring > TA_RECORD_RING_COUNT) {
        return TA_ERR_INVALID;
    }
    if (params->size < TA_RECORD_HEADER_BYTES) {
        return TA_ERR_CORRUPT;
    }
    uint16_t kind = 0u;
    uint32_t size = 0u;
    ta_mem_copy(&kind, params->bytes + offsetof(ta_record_header, kind), sizeof(kind));
    ta_mem_copy(&size, params->bytes + offsetof(ta_record_header, size), sizeof(size));
    if (kind == TA_RECORD_KIND_PAD) {
        return TA_ERR_CORRUPT;
    }
    uint32_t index = ta_record_descriptor_index(kind);
    if (index == TA_ARRAY_COUNT(ta_record_descriptors)) {
        return TA_ERR_UNSUPPORTED;
    }
    const ta_record_descriptor *descriptor = &ta_record_descriptors[index];
    if ((uint32_t)descriptor->ring != (uint32_t)params->ring) {
        return TA_ERR_CORRUPT;
    }
    if ((size & 7u) != 0u || size > params->size || size < descriptor->fixed_size) {
        return TA_ERR_CORRUPT;
    }
    for (uint32_t index_in_kind = 0; index_in_kind < descriptor->span_count; index_in_kind += 1u) {
        ta_span32 span = { 0u, 0u };
        ta_mem_copy(&span, params->bytes + ta_record_span_offsets[descriptor->span_first + index_in_kind],
                    sizeof(span));
        if (span.offset < descriptor->fixed_size || span.offset > size || span.length > size - span.offset) {
            return TA_ERR_CORRUPT;
        }
    }
    out->bytes = params->bytes;
    out->size = size;
    out->kind = kind;
    return TA_OK;
}

ta_result ta_record_describe(uint16_t kind, ta_record_description *out) {
    uint32_t index = ta_record_descriptor_index(kind);
    if (index == TA_ARRAY_COUNT(ta_record_descriptors) || out == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_record_descriptor *descriptor = &ta_record_descriptors[index];
    out->ring = (ta_record_ring)descriptor->ring;
    out->fixed_size = descriptor->fixed_size;
    out->span_count = descriptor->span_count;
    out->span_offsets = &ta_record_span_offsets[descriptor->span_first];
    return TA_OK;
}

/* ---- h2c.time ---- */

ta_result ta_h2c_time_write(ta_record_buffer out, const ta_h2c_time *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_TIME, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.event ---- */

ta_result ta_h2c_event_size(const ta_h2c_event_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[6] = {
        TA_RECORD_TAIL(texts->target),
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->origin),
        TA_RECORD_TAIL(texts->origin_type),
        TA_RECORD_TAIL(texts->invokeid),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_EVENT, 6u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_event_write(ta_record_buffer out, const ta_h2c_event *fixed, const ta_h2c_event_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[6] = {
        TA_RECORD_TAIL(texts->target),
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->origin),
        TA_RECORD_TAIL(texts->origin_type),
        TA_RECORD_TAIL(texts->invokeid),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_EVENT, 6u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.spawn ---- */

ta_result ta_h2c_spawn_size(const ta_h2c_spawn_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->revision),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_SPAWN, 2u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_spawn_write(ta_record_buffer out, const ta_h2c_spawn *fixed, const ta_h2c_spawn_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->revision),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_SPAWN, 2u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.publish ---- */

ta_result ta_h2c_publish_size(const ta_h2c_publish_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_PUBLISH, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_publish_write(ta_record_buffer out, const ta_h2c_publish *fixed, const ta_h2c_publish_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_PUBLISH, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.load ---- */

ta_result ta_h2c_load_size(const ta_h2c_load_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_LOAD, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_load_write(ta_record_buffer out, const ta_h2c_load *fixed, const ta_h2c_load_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_LOAD, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.load_failed ---- */

ta_result ta_h2c_load_failed_size(const ta_h2c_load_failed_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->reason),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_LOAD_FAILED, 2u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_load_failed_write(ta_record_buffer out, const ta_h2c_load_failed *fixed, const ta_h2c_load_failed_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->reason),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_LOAD_FAILED, 2u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.unload ---- */

ta_result ta_h2c_unload_size(const ta_h2c_unload_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_UNLOAD, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_unload_write(ta_record_buffer out, const ta_h2c_unload *fixed, const ta_h2c_unload_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_UNLOAD, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.checkpoint ---- */

ta_result ta_h2c_checkpoint_size(const ta_h2c_checkpoint_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_CHECKPOINT, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_checkpoint_write(ta_record_buffer out, const ta_h2c_checkpoint *fixed, const ta_h2c_checkpoint_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_CHECKPOINT, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.invoke_report ---- */

ta_result ta_h2c_invoke_report_size(const ta_h2c_invoke_report_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->reason),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_INVOKE_REPORT, 2u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_invoke_report_write(ta_record_buffer out, const ta_h2c_invoke_report *fixed, const ta_h2c_invoke_report_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->reason),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_INVOKE_REPORT, 2u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.destroy ---- */

ta_result ta_h2c_destroy_size(const ta_h2c_destroy_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_DESTROY, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_destroy_write(ta_record_buffer out, const ta_h2c_destroy *fixed, const ta_h2c_destroy_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_DESTROY, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.definition_register ---- */

ta_result ta_h2c_definition_register_write(ta_record_buffer out, const ta_h2c_definition_register *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_DEFINITION_REGISTER, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.definition_release ---- */

ta_result ta_h2c_definition_release_write(ta_record_buffer out, const ta_h2c_definition_release *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_DEFINITION_RELEASE, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.definition_retire ---- */

ta_result ta_h2c_definition_retire_write(ta_record_buffer out, const ta_h2c_definition_retire *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_DEFINITION_RETIRE, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.withdraw ---- */

ta_result ta_h2c_withdraw_write(ta_record_buffer out, const ta_h2c_withdraw *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_WITHDRAW, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- h2c.type_declare ---- */

ta_result ta_h2c_type_declare_size(const ta_h2c_type_declare_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->type),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_TYPE_DECLARE, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_h2c_type_declare_write(ta_record_buffer out, const ta_h2c_type_declare *fixed, const ta_h2c_type_declare_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->type),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_H2C_TYPE_DECLARE, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.spawned ---- */

ta_result ta_c2h_spawned_size(const ta_c2h_spawned_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->revision),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_SPAWNED, 2u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_spawned_write(ta_record_buffer out, const ta_c2h_spawned *fixed, const ta_c2h_spawned_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[2] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->revision),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_SPAWNED, 2u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.input ---- */

ta_result ta_c2h_input_size(const ta_c2h_input_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[7] = {
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->origin),
        TA_RECORD_TAIL(texts->origin_type),
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->report_invokeid),
        TA_RECORD_TAIL(texts->report_reason),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INPUT, 7u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_input_write(ta_record_buffer out, const ta_c2h_input *fixed, const ta_c2h_input_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[7] = {
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->origin),
        TA_RECORD_TAIL(texts->origin_type),
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->report_invokeid),
        TA_RECORD_TAIL(texts->report_reason),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INPUT, 7u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.send ---- */

ta_result ta_c2h_send_size(const ta_c2h_send_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[4] = {
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->io_type),
        TA_RECORD_TAIL(texts->target),
        TA_RECORD_TAIL(texts->name),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_SEND, 4u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_send_write(ta_record_buffer out, const ta_c2h_send *fixed, const ta_c2h_send_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[4] = {
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->io_type),
        TA_RECORD_TAIL(texts->target),
        TA_RECORD_TAIL(texts->name),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_SEND, 4u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.invoke ---- */

ta_result ta_c2h_invoke_size(const ta_c2h_invoke_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[3] = {
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->type),
        TA_RECORD_TAIL(texts->src),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INVOKE, 3u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_invoke_write(ta_record_buffer out, const ta_c2h_invoke *fixed, const ta_c2h_invoke_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[3] = {
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->type),
        TA_RECORD_TAIL(texts->src),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INVOKE, 3u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.invoke_cancel ---- */

ta_result ta_c2h_invoke_cancel_size(const ta_c2h_invoke_cancel_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->invokeid),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INVOKE_CANCEL, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_invoke_cancel_write(ta_record_buffer out, const ta_c2h_invoke_cancel *fixed, const ta_c2h_invoke_cancel_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->invokeid),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INVOKE_CANCEL, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.invoke_forward ---- */

ta_result ta_c2h_invoke_forward_size(const ta_c2h_invoke_forward_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[6] = {
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->origin),
        TA_RECORD_TAIL(texts->origin_type),
        TA_RECORD_TAIL(texts->event_invokeid),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INVOKE_FORWARD, 6u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_invoke_forward_write(ta_record_buffer out, const ta_c2h_invoke_forward *fixed, const ta_c2h_invoke_forward_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[6] = {
        TA_RECORD_TAIL(texts->invokeid),
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->sendid),
        TA_RECORD_TAIL(texts->origin),
        TA_RECORD_TAIL(texts->origin_type),
        TA_RECORD_TAIL(texts->event_invokeid),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INVOKE_FORWARD, 6u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.log ---- */

ta_result ta_c2h_log_size(const ta_c2h_log_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->label),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_LOG, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_log_write(ta_record_buffer out, const ta_c2h_log *fixed, const ta_c2h_log_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->label),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_LOG, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.macrostep_ended ---- */

ta_result ta_c2h_macrostep_ended_write(ta_record_buffer out, const ta_c2h_macrostep_ended *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_MACROSTEP_ENDED, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.wake ---- */

ta_result ta_c2h_wake_write(ta_record_buffer out, const ta_c2h_wake *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_WAKE, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.bounced ---- */

ta_result ta_c2h_bounced_size(const ta_c2h_bounced_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->detail),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_BOUNCED, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_bounced_write(ta_record_buffer out, const ta_c2h_bounced *fixed, const ta_c2h_bounced_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->detail),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_BOUNCED, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.load_needed ---- */

ta_result ta_c2h_load_needed_size(const ta_c2h_load_needed_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_LOAD_NEEDED, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_load_needed_write(ta_record_buffer out, const ta_c2h_load_needed *fixed, const ta_c2h_load_needed_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_LOAD_NEEDED, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.loaded ---- */

ta_result ta_c2h_loaded_size(const ta_c2h_loaded_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_LOADED, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_loaded_write(ta_record_buffer out, const ta_c2h_loaded *fixed, const ta_c2h_loaded_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_LOADED, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.unloaded ---- */

ta_result ta_c2h_unloaded_size(const ta_c2h_unloaded_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_UNLOADED, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_unloaded_write(ta_record_buffer out, const ta_c2h_unloaded *fixed, const ta_c2h_unloaded_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_UNLOADED, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.state ---- */

ta_result ta_c2h_state_size(const ta_c2h_state_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_STATE, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_state_write(ta_record_buffer out, const ta_c2h_state *fixed, const ta_c2h_state_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_STATE, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.published ---- */

ta_result ta_c2h_published_size(const ta_c2h_published_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_PUBLISHED, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_published_write(ta_record_buffer out, const ta_c2h_published *fixed, const ta_c2h_published_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_PUBLISHED, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.finished ---- */

ta_result ta_c2h_finished_write(ta_record_buffer out, const ta_c2h_finished *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_FINISHED, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.destroyed ---- */

ta_result ta_c2h_destroyed_size(const ta_c2h_destroyed_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_DESTROYED, 1u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_c2h_destroyed_write(ta_record_buffer out, const ta_c2h_destroyed *fixed, const ta_c2h_destroyed_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[1] = {
        TA_RECORD_TAIL(texts->address),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_DESTROYED, 1u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.definition_result ---- */

ta_result ta_c2h_definition_result_write(ta_record_buffer out, const ta_c2h_definition_result *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_DEFINITION_RESULT, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.input_refused ---- */

ta_result ta_c2h_input_refused_write(ta_record_buffer out, const ta_c2h_input_refused *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INPUT_REFUSED, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.input_completed ---- */

ta_result ta_c2h_input_completed_write(ta_record_buffer out, const ta_c2h_input_completed *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_INPUT_COMPLETED, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- c2h.load_result ---- */

ta_result ta_c2h_load_result_write(ta_record_buffer out, const ta_c2h_load_result *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_C2H_LOAD_RESULT, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

/* ---- tel.trace ---- */

ta_result ta_tel_trace_size(const ta_tel_trace_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[13] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->definition),
        TA_RECORD_TAIL(texts->state_name),
        TA_RECORD_TAIL(texts->transition_name),
        TA_RECORD_TAIL(texts->action_name),
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->send_id),
        TA_RECORD_TAIL(texts->invoke_id),
        TA_RECORD_TAIL(texts->value_text),
        TA_RECORD_TAIL(texts->fault_detail),
        TA_RECORD_TAIL(texts->fault_message),
        TA_RECORD_TAIL(texts->transitions),
        TA_RECORD_TAIL(texts->states),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_TEL_TRACE, 13u, tails };
    return ta_record_measure(&shape, out);
}

ta_result ta_tel_trace_write(ta_record_buffer out, const ta_tel_trace *fixed, const ta_tel_trace_texts *texts) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_bytes tails[13] = {
        TA_RECORD_TAIL(texts->address),
        TA_RECORD_TAIL(texts->definition),
        TA_RECORD_TAIL(texts->state_name),
        TA_RECORD_TAIL(texts->transition_name),
        TA_RECORD_TAIL(texts->action_name),
        TA_RECORD_TAIL(texts->name),
        TA_RECORD_TAIL(texts->send_id),
        TA_RECORD_TAIL(texts->invoke_id),
        TA_RECORD_TAIL(texts->value_text),
        TA_RECORD_TAIL(texts->fault_detail),
        TA_RECORD_TAIL(texts->fault_message),
        TA_RECORD_TAIL(texts->transitions),
        TA_RECORD_TAIL(texts->states),
    };
    const ta_record_shape shape = { TA_RECORD_KIND_TEL_TRACE, 13u, tails };
    return ta_record_write(out, fixed, &shape);
}

/* ---- tel.dropped ---- */

ta_result ta_tel_dropped_write(ta_record_buffer out, const ta_tel_dropped *fixed) {
    const ta_record_shape shape = { TA_RECORD_KIND_TEL_DROPPED, 0u, NULL };
    return ta_record_write(out, fixed, &shape);
}

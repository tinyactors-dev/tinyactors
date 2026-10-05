/* The record codec: see ring_codec.h. Generated from protocol/catalog.rb
 * by `mise run protocol` in tinyactors-core. Do not edit. */
#include "ring_codec.h"
#include "../memory.h"
#include "../../include/tinyactors.h"
#include "../span.h"
#include "ring_records.h"

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
    (uint32_t)offsetof(ta_tel_trace, list_ids),
    (uint32_t)offsetof(ta_tel_trace, session_id),
    (uint32_t)offsetof(ta_tel_trace, cause_session_id),
    (uint32_t)offsetof(ta_tel_trace, address),
    (uint32_t)offsetof(ta_tel_trace, family_name),
    (uint32_t)offsetof(ta_tel_trace, revision),
    (uint32_t)offsetof(ta_tel_trace, document_name),
    (uint32_t)offsetof(ta_tel_trace, source),
    (uint32_t)offsetof(ta_tel_trace, state_name),
    (uint32_t)offsetof(ta_tel_trace, transition_name),
    (uint32_t)offsetof(ta_tel_trace, action_name),
    (uint32_t)offsetof(ta_tel_trace, subject_state_name),
    (uint32_t)offsetof(ta_tel_trace, subject_action_name),
    (uint32_t)offsetof(ta_tel_trace, name),
    (uint32_t)offsetof(ta_tel_trace, event_name),
    (uint32_t)offsetof(ta_tel_trace, event_send_id),
    (uint32_t)offsetof(ta_tel_trace, event_invoke_id),
    (uint32_t)offsetof(ta_tel_trace, event_data),
    (uint32_t)offsetof(ta_tel_trace, send_id),
    (uint32_t)offsetof(ta_tel_trace, invoke_id),
    (uint32_t)offsetof(ta_tel_trace, value_text),
    (uint32_t)offsetof(ta_tel_trace, fault_detail),
    (uint32_t)offsetof(ta_tel_trace, fault_message),
    (uint32_t)offsetof(ta_tel_trace, fault_data),
    (uint32_t)offsetof(ta_tel_trace, list_names),
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
    { TA_RECORD_KIND_TEL_TRACE, TA_RECORD_RING_TEL, 25u, (uint32_t)sizeof(ta_tel_trace), 49u },
    { TA_RECORD_KIND_TEL_DROPPED, TA_RECORD_RING_TEL, 0u, (uint32_t)sizeof(ta_tel_dropped), 74u },
};

/* Each kind's descriptor index plus one, by its code less 0x0101; 0 for a
 * code no record has. One load finds a kind, read or written. */
static const uint8_t ta_record_descriptor_slots[0x0202] = {
    [TA_RECORD_KIND_H2C_TIME - 0x0101] = 1u,
    [TA_RECORD_KIND_H2C_EVENT - 0x0101] = 2u,
    [TA_RECORD_KIND_H2C_SPAWN - 0x0101] = 3u,
    [TA_RECORD_KIND_H2C_PUBLISH - 0x0101] = 4u,
    [TA_RECORD_KIND_H2C_LOAD - 0x0101] = 5u,
    [TA_RECORD_KIND_H2C_LOAD_FAILED - 0x0101] = 6u,
    [TA_RECORD_KIND_H2C_UNLOAD - 0x0101] = 7u,
    [TA_RECORD_KIND_H2C_CHECKPOINT - 0x0101] = 8u,
    [TA_RECORD_KIND_H2C_INVOKE_REPORT - 0x0101] = 9u,
    [TA_RECORD_KIND_H2C_DESTROY - 0x0101] = 10u,
    [TA_RECORD_KIND_H2C_DEFINITION_REGISTER - 0x0101] = 11u,
    [TA_RECORD_KIND_H2C_DEFINITION_RELEASE - 0x0101] = 12u,
    [TA_RECORD_KIND_H2C_DEFINITION_RETIRE - 0x0101] = 13u,
    [TA_RECORD_KIND_H2C_WITHDRAW - 0x0101] = 14u,
    [TA_RECORD_KIND_H2C_TYPE_DECLARE - 0x0101] = 15u,
    [TA_RECORD_KIND_C2H_SPAWNED - 0x0101] = 16u,
    [TA_RECORD_KIND_C2H_INPUT - 0x0101] = 17u,
    [TA_RECORD_KIND_C2H_SEND - 0x0101] = 18u,
    [TA_RECORD_KIND_C2H_INVOKE - 0x0101] = 19u,
    [TA_RECORD_KIND_C2H_INVOKE_CANCEL - 0x0101] = 20u,
    [TA_RECORD_KIND_C2H_INVOKE_FORWARD - 0x0101] = 21u,
    [TA_RECORD_KIND_C2H_LOG - 0x0101] = 22u,
    [TA_RECORD_KIND_C2H_MACROSTEP_ENDED - 0x0101] = 23u,
    [TA_RECORD_KIND_C2H_WAKE - 0x0101] = 24u,
    [TA_RECORD_KIND_C2H_BOUNCED - 0x0101] = 25u,
    [TA_RECORD_KIND_C2H_LOAD_NEEDED - 0x0101] = 26u,
    [TA_RECORD_KIND_C2H_LOADED - 0x0101] = 27u,
    [TA_RECORD_KIND_C2H_UNLOADED - 0x0101] = 28u,
    [TA_RECORD_KIND_C2H_STATE - 0x0101] = 29u,
    [TA_RECORD_KIND_C2H_PUBLISHED - 0x0101] = 30u,
    [TA_RECORD_KIND_C2H_FINISHED - 0x0101] = 31u,
    [TA_RECORD_KIND_C2H_DESTROYED - 0x0101] = 32u,
    [TA_RECORD_KIND_C2H_DEFINITION_RESULT - 0x0101] = 33u,
    [TA_RECORD_KIND_C2H_INPUT_REFUSED - 0x0101] = 34u,
    [TA_RECORD_KIND_C2H_INPUT_COMPLETED - 0x0101] = 35u,
    [TA_RECORD_KIND_C2H_LOAD_RESULT - 0x0101] = 36u,
    [TA_RECORD_KIND_TEL_TRACE - 0x0101] = 37u,
    [TA_RECORD_KIND_TEL_DROPPED - 0x0101] = 38u,
};

/* The index of `kind` in ta_record_descriptors, or their count when no
 * record has that kind. */
static uint32_t ta_record_descriptor_index(uint16_t kind) {
    uint32_t at = (uint32_t)kind - 0x0101u; /* wraps past the table below the first code */
    uint32_t slot = at < TA_ARRAY_COUNT(ta_record_descriptor_slots) ? ta_record_descriptor_slots[at] : 0u;
    return slot == 0u ? TA_ARRAY_COUNT(ta_record_descriptors) : slot - 1u;
}

/* What a writer checks a buffer against: its kind, its fixed part and the record's size. */
typedef struct {
    uint16_t kind;
    uint32_t fixed_size;
    uint32_t needed;
} ta_record_frame;

/*
 * A record being written: its bytes, where its next text goes, and the
 * run of texts not yet copied. Texts that lie end to end in the caller's
 * memory, as a producer that composes them in catalog order lays them,
 * are copied as one run.
 */
typedef struct {
    unsigned char *bytes;
    uint32_t cursor;
    const uint8_t *run;
    uint32_t run_at;
    uint32_t run_len;
} ta_record_tail;

#define TA_RECORD_TEXT(text) ((ta_bytes){ (const uint8_t *)(const void *)(text).ptr, (text).len })

/* Adds a text's length to `*size`; false for a text with a length but no bytes. */
static inline bool ta_record_measure(uint64_t *size, ta_bytes text) {
    *size += text.len;
    return text.ptr != NULL || text.len == 0u;
}

/* A measured record's size, rounded up to 8: TA_ERR_INVALID for a bad text
 * or a record past the largest size a u32 holds. */
static ta_result ta_record_round(uint64_t size, bool valid, uint32_t *out) {
    size = (size + 7u) & ~(uint64_t)7u;
    if (!valid || size > UINT32_MAX) {
        return TA_ERR_INVALID;
    }
    *out = (uint32_t)size;
    return TA_OK;
}

/* Checks the buffer, then fills the fixed part and the header; see ring_codec.h. */
static ta_result ta_record_begin(ta_record_buffer out, const void *fixed, const ta_record_frame *frame) {
    if (out.bytes == NULL || ((uintptr_t)(void *)out.bytes & 7u) != 0u || (out.size & 7u) != 0u ||
        fixed == NULL) {
        return TA_ERR_INVALID;
    }
    if (out.size < frame->needed) {
        return TA_ERR_FULL;
    }
    uint64_t seq = 0u;
    ta_mem_copy(&seq, out.bytes + offsetof(ta_record_header, seq), sizeof(seq));
    if (fixed != (const void *)out.bytes) {
        ta_mem_copy(out.bytes, fixed, frame->fixed_size);
    }
    uint16_t kind = frame->kind;
    ta_mem_copy(out.bytes + offsetof(ta_record_header, kind), &kind, sizeof(kind));
    ta_mem_copy(out.bytes + offsetof(ta_record_header, size), &out.size, sizeof(out.size));
    ta_mem_copy(out.bytes + offsetof(ta_record_header, seq), &seq, sizeof(seq));
    return TA_OK;
}

/* Copies the run of texts not yet copied. */
static void ta_record_flush(ta_record_tail *tail) {
    if (tail->run_len != 0u) {
        ta_mem_copy(tail->bytes + tail->run_at, tail->run, tail->run_len);
    }
    tail->run_len = 0u;
}

/* Lays one text at the cursor and points the span at `span_offset` at it. */
static inline void ta_record_put(ta_record_tail *tail, uint32_t span_offset, ta_bytes text) {
    ta_span32 span = { tail->cursor, text.len };
    ta_mem_copy(tail->bytes + span_offset, &span, sizeof(span));
    if (text.len == 0u) {
        return;
    }
    if (tail->run_len == 0u || text.ptr != tail->run + tail->run_len) {
        ta_record_flush(tail);
        tail->run = text.ptr;
        tail->run_at = tail->cursor;
    }
    tail->run_len += text.len;
    tail->cursor += text.len;
}

/* Copies the last run, then zeroes the bytes after the last text: the
 * texts cover the rest of the tail. */
static void ta_record_finish(ta_record_buffer out, ta_record_tail *tail) {
    ta_record_flush(tail);
    ta_mem_zero(out.bytes + tail->cursor, out.size - tail->cursor);
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
    const ta_record_frame frame = { TA_RECORD_KIND_H2C_TIME, (uint32_t)sizeof(ta_h2c_time), (uint32_t)sizeof(ta_h2c_time) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_time), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- h2c.event ---- */

ta_result ta_h2c_event_size(const ta_h2c_event_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_event);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->target));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->sendid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->origin));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->origin_type));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->invokeid));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_event_write(ta_record_buffer out, const ta_h2c_event *fixed, const ta_h2c_event_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_event_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_EVENT, (uint32_t)sizeof(ta_h2c_event), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_event), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_event, target), TA_RECORD_TEXT(texts->target));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_event, name), TA_RECORD_TEXT(texts->name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_event, sendid), TA_RECORD_TEXT(texts->sendid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_event, origin), TA_RECORD_TEXT(texts->origin));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_event, origin_type), TA_RECORD_TEXT(texts->origin_type));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_event, invokeid), TA_RECORD_TEXT(texts->invokeid));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.spawn ---- */

ta_result ta_h2c_spawn_size(const ta_h2c_spawn_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_spawn);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->revision));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_spawn_write(ta_record_buffer out, const ta_h2c_spawn *fixed, const ta_h2c_spawn_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_spawn_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_SPAWN, (uint32_t)sizeof(ta_h2c_spawn), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_spawn), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_spawn, address), TA_RECORD_TEXT(texts->address));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_spawn, revision), TA_RECORD_TEXT(texts->revision));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.publish ---- */

ta_result ta_h2c_publish_size(const ta_h2c_publish_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_publish);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_publish_write(ta_record_buffer out, const ta_h2c_publish *fixed, const ta_h2c_publish_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_publish_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_PUBLISH, (uint32_t)sizeof(ta_h2c_publish), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_publish), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_publish, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.load ---- */

ta_result ta_h2c_load_size(const ta_h2c_load_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_load);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_load_write(ta_record_buffer out, const ta_h2c_load *fixed, const ta_h2c_load_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_load_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_LOAD, (uint32_t)sizeof(ta_h2c_load), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_load), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_load, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.load_failed ---- */

ta_result ta_h2c_load_failed_size(const ta_h2c_load_failed_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_load_failed);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->reason));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_load_failed_write(ta_record_buffer out, const ta_h2c_load_failed *fixed, const ta_h2c_load_failed_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_load_failed_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_LOAD_FAILED, (uint32_t)sizeof(ta_h2c_load_failed), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_load_failed), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_load_failed, address), TA_RECORD_TEXT(texts->address));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_load_failed, reason), TA_RECORD_TEXT(texts->reason));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.unload ---- */

ta_result ta_h2c_unload_size(const ta_h2c_unload_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_unload);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_unload_write(ta_record_buffer out, const ta_h2c_unload *fixed, const ta_h2c_unload_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_unload_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_UNLOAD, (uint32_t)sizeof(ta_h2c_unload), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_unload), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_unload, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.checkpoint ---- */

ta_result ta_h2c_checkpoint_size(const ta_h2c_checkpoint_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_checkpoint);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_checkpoint_write(ta_record_buffer out, const ta_h2c_checkpoint *fixed, const ta_h2c_checkpoint_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_checkpoint_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_CHECKPOINT, (uint32_t)sizeof(ta_h2c_checkpoint), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_checkpoint), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_checkpoint, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.invoke_report ---- */

ta_result ta_h2c_invoke_report_size(const ta_h2c_invoke_report_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_invoke_report);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->invokeid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->reason));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_invoke_report_write(ta_record_buffer out, const ta_h2c_invoke_report *fixed, const ta_h2c_invoke_report_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_invoke_report_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_INVOKE_REPORT, (uint32_t)sizeof(ta_h2c_invoke_report), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_invoke_report), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_invoke_report, invokeid), TA_RECORD_TEXT(texts->invokeid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_invoke_report, reason), TA_RECORD_TEXT(texts->reason));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.destroy ---- */

ta_result ta_h2c_destroy_size(const ta_h2c_destroy_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_destroy);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_destroy_write(ta_record_buffer out, const ta_h2c_destroy *fixed, const ta_h2c_destroy_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_destroy_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_DESTROY, (uint32_t)sizeof(ta_h2c_destroy), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_destroy), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_destroy, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- h2c.definition_register ---- */

ta_result ta_h2c_definition_register_write(ta_record_buffer out, const ta_h2c_definition_register *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_H2C_DEFINITION_REGISTER, (uint32_t)sizeof(ta_h2c_definition_register), (uint32_t)sizeof(ta_h2c_definition_register) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_definition_register), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- h2c.definition_release ---- */

ta_result ta_h2c_definition_release_write(ta_record_buffer out, const ta_h2c_definition_release *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_H2C_DEFINITION_RELEASE, (uint32_t)sizeof(ta_h2c_definition_release), (uint32_t)sizeof(ta_h2c_definition_release) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_definition_release), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- h2c.definition_retire ---- */

ta_result ta_h2c_definition_retire_write(ta_record_buffer out, const ta_h2c_definition_retire *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_H2C_DEFINITION_RETIRE, (uint32_t)sizeof(ta_h2c_definition_retire), (uint32_t)sizeof(ta_h2c_definition_retire) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_definition_retire), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- h2c.withdraw ---- */

ta_result ta_h2c_withdraw_write(ta_record_buffer out, const ta_h2c_withdraw *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_H2C_WITHDRAW, (uint32_t)sizeof(ta_h2c_withdraw), (uint32_t)sizeof(ta_h2c_withdraw) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_withdraw), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- h2c.type_declare ---- */

ta_result ta_h2c_type_declare_size(const ta_h2c_type_declare_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_h2c_type_declare);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->type));
    return ta_record_round(size, valid, out);
}

ta_result ta_h2c_type_declare_write(ta_record_buffer out, const ta_h2c_type_declare *fixed, const ta_h2c_type_declare_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_h2c_type_declare_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_H2C_TYPE_DECLARE, (uint32_t)sizeof(ta_h2c_type_declare), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_h2c_type_declare), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_h2c_type_declare, type), TA_RECORD_TEXT(texts->type));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.spawned ---- */

ta_result ta_c2h_spawned_size(const ta_c2h_spawned_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_spawned);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->revision));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_spawned_write(ta_record_buffer out, const ta_c2h_spawned *fixed, const ta_c2h_spawned_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_spawned_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_SPAWNED, (uint32_t)sizeof(ta_c2h_spawned), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_spawned), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_spawned, address), TA_RECORD_TEXT(texts->address));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_spawned, revision), TA_RECORD_TEXT(texts->revision));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.input ---- */

ta_result ta_c2h_input_size(const ta_c2h_input_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_input);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->sendid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->origin));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->origin_type));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->invokeid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->report_invokeid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->report_reason));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_input_write(ta_record_buffer out, const ta_c2h_input *fixed, const ta_c2h_input_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_input_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_INPUT, (uint32_t)sizeof(ta_c2h_input), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_input), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_input, name), TA_RECORD_TEXT(texts->name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_input, sendid), TA_RECORD_TEXT(texts->sendid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_input, origin), TA_RECORD_TEXT(texts->origin));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_input, origin_type), TA_RECORD_TEXT(texts->origin_type));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_input, invokeid), TA_RECORD_TEXT(texts->invokeid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_input, report_invokeid), TA_RECORD_TEXT(texts->report_invokeid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_input, report_reason), TA_RECORD_TEXT(texts->report_reason));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.send ---- */

ta_result ta_c2h_send_size(const ta_c2h_send_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_send);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->sendid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->io_type));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->target));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->name));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_send_write(ta_record_buffer out, const ta_c2h_send *fixed, const ta_c2h_send_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_send_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_SEND, (uint32_t)sizeof(ta_c2h_send), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_send), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_send, sendid), TA_RECORD_TEXT(texts->sendid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_send, io_type), TA_RECORD_TEXT(texts->io_type));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_send, target), TA_RECORD_TEXT(texts->target));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_send, name), TA_RECORD_TEXT(texts->name));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.invoke ---- */

ta_result ta_c2h_invoke_size(const ta_c2h_invoke_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_invoke);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->invokeid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->type));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->src));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_invoke_write(ta_record_buffer out, const ta_c2h_invoke *fixed, const ta_c2h_invoke_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_invoke_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_INVOKE, (uint32_t)sizeof(ta_c2h_invoke), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_invoke), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke, invokeid), TA_RECORD_TEXT(texts->invokeid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke, type), TA_RECORD_TEXT(texts->type));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke, src), TA_RECORD_TEXT(texts->src));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.invoke_cancel ---- */

ta_result ta_c2h_invoke_cancel_size(const ta_c2h_invoke_cancel_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_invoke_cancel);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->invokeid));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_invoke_cancel_write(ta_record_buffer out, const ta_c2h_invoke_cancel *fixed, const ta_c2h_invoke_cancel_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_invoke_cancel_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_INVOKE_CANCEL, (uint32_t)sizeof(ta_c2h_invoke_cancel), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_invoke_cancel), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke_cancel, invokeid), TA_RECORD_TEXT(texts->invokeid));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.invoke_forward ---- */

ta_result ta_c2h_invoke_forward_size(const ta_c2h_invoke_forward_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_invoke_forward);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->invokeid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->sendid));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->origin));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->origin_type));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->event_invokeid));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_invoke_forward_write(ta_record_buffer out, const ta_c2h_invoke_forward *fixed, const ta_c2h_invoke_forward_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_invoke_forward_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_INVOKE_FORWARD, (uint32_t)sizeof(ta_c2h_invoke_forward), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_invoke_forward), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke_forward, invokeid), TA_RECORD_TEXT(texts->invokeid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke_forward, name), TA_RECORD_TEXT(texts->name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke_forward, sendid), TA_RECORD_TEXT(texts->sendid));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke_forward, origin), TA_RECORD_TEXT(texts->origin));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke_forward, origin_type), TA_RECORD_TEXT(texts->origin_type));
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_invoke_forward, event_invokeid), TA_RECORD_TEXT(texts->event_invokeid));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.log ---- */

ta_result ta_c2h_log_size(const ta_c2h_log_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_log);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->label));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_log_write(ta_record_buffer out, const ta_c2h_log *fixed, const ta_c2h_log_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_log_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_LOG, (uint32_t)sizeof(ta_c2h_log), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_log), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_log, label), TA_RECORD_TEXT(texts->label));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.macrostep_ended ---- */

ta_result ta_c2h_macrostep_ended_write(ta_record_buffer out, const ta_c2h_macrostep_ended *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_C2H_MACROSTEP_ENDED, (uint32_t)sizeof(ta_c2h_macrostep_ended), (uint32_t)sizeof(ta_c2h_macrostep_ended) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_macrostep_ended), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- c2h.wake ---- */

ta_result ta_c2h_wake_write(ta_record_buffer out, const ta_c2h_wake *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_C2H_WAKE, (uint32_t)sizeof(ta_c2h_wake), (uint32_t)sizeof(ta_c2h_wake) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_wake), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- c2h.bounced ---- */

ta_result ta_c2h_bounced_size(const ta_c2h_bounced_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_bounced);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->detail));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_bounced_write(ta_record_buffer out, const ta_c2h_bounced *fixed, const ta_c2h_bounced_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_bounced_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_BOUNCED, (uint32_t)sizeof(ta_c2h_bounced), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_bounced), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_bounced, detail), TA_RECORD_TEXT(texts->detail));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.load_needed ---- */

ta_result ta_c2h_load_needed_size(const ta_c2h_load_needed_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_load_needed);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_load_needed_write(ta_record_buffer out, const ta_c2h_load_needed *fixed, const ta_c2h_load_needed_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_load_needed_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_LOAD_NEEDED, (uint32_t)sizeof(ta_c2h_load_needed), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_load_needed), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_load_needed, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.loaded ---- */

ta_result ta_c2h_loaded_size(const ta_c2h_loaded_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_loaded);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_loaded_write(ta_record_buffer out, const ta_c2h_loaded *fixed, const ta_c2h_loaded_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_loaded_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_LOADED, (uint32_t)sizeof(ta_c2h_loaded), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_loaded), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_loaded, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.unloaded ---- */

ta_result ta_c2h_unloaded_size(const ta_c2h_unloaded_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_unloaded);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_unloaded_write(ta_record_buffer out, const ta_c2h_unloaded *fixed, const ta_c2h_unloaded_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_unloaded_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_UNLOADED, (uint32_t)sizeof(ta_c2h_unloaded), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_unloaded), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_unloaded, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.state ---- */

ta_result ta_c2h_state_size(const ta_c2h_state_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_state);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_state_write(ta_record_buffer out, const ta_c2h_state *fixed, const ta_c2h_state_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_state_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_STATE, (uint32_t)sizeof(ta_c2h_state), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_state), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_state, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.published ---- */

ta_result ta_c2h_published_size(const ta_c2h_published_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_published);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_published_write(ta_record_buffer out, const ta_c2h_published *fixed, const ta_c2h_published_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_published_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_PUBLISHED, (uint32_t)sizeof(ta_c2h_published), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_published), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_published, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.finished ---- */

ta_result ta_c2h_finished_write(ta_record_buffer out, const ta_c2h_finished *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_C2H_FINISHED, (uint32_t)sizeof(ta_c2h_finished), (uint32_t)sizeof(ta_c2h_finished) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_finished), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- c2h.destroyed ---- */

ta_result ta_c2h_destroyed_size(const ta_c2h_destroyed_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_c2h_destroyed);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    return ta_record_round(size, valid, out);
}

ta_result ta_c2h_destroyed_write(ta_record_buffer out, const ta_c2h_destroyed *fixed, const ta_c2h_destroyed_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_c2h_destroyed_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_C2H_DESTROYED, (uint32_t)sizeof(ta_c2h_destroyed), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_destroyed), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_c2h_destroyed, address), TA_RECORD_TEXT(texts->address));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- c2h.definition_result ---- */

ta_result ta_c2h_definition_result_write(ta_record_buffer out, const ta_c2h_definition_result *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_C2H_DEFINITION_RESULT, (uint32_t)sizeof(ta_c2h_definition_result), (uint32_t)sizeof(ta_c2h_definition_result) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_definition_result), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- c2h.input_refused ---- */

ta_result ta_c2h_input_refused_write(ta_record_buffer out, const ta_c2h_input_refused *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_C2H_INPUT_REFUSED, (uint32_t)sizeof(ta_c2h_input_refused), (uint32_t)sizeof(ta_c2h_input_refused) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_input_refused), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- c2h.input_completed ---- */

ta_result ta_c2h_input_completed_write(ta_record_buffer out, const ta_c2h_input_completed *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_C2H_INPUT_COMPLETED, (uint32_t)sizeof(ta_c2h_input_completed), (uint32_t)sizeof(ta_c2h_input_completed) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_input_completed), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- c2h.load_result ---- */

ta_result ta_c2h_load_result_write(ta_record_buffer out, const ta_c2h_load_result *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_C2H_LOAD_RESULT, (uint32_t)sizeof(ta_c2h_load_result), (uint32_t)sizeof(ta_c2h_load_result) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_c2h_load_result), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

/* ---- tel.trace ---- */

ta_result ta_tel_trace_size(const ta_tel_trace_texts *texts, uint32_t *out) {
    if (texts == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t size = sizeof(ta_tel_trace);
    bool valid = true;
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->list_ids));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->session_id));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->cause_session_id));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->address));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->family_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->revision));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->document_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->source));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->state_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->transition_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->action_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->subject_state_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->subject_action_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->event_name));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->event_send_id));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->event_invoke_id));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->event_data));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->send_id));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->invoke_id));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->value_text));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->fault_detail));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->fault_message));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->fault_data));
    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->list_names));
    return ta_record_round(size, valid, out);
}

ta_result ta_tel_trace_write(ta_record_buffer out, const ta_tel_trace *fixed, const ta_tel_trace_texts *texts) {
    uint32_t needed = 0u;
    ta_result result = ta_tel_trace_size(texts, &needed);
    if (result == TA_OK) {
        const ta_record_frame frame = { TA_RECORD_KIND_TEL_TRACE, (uint32_t)sizeof(ta_tel_trace), needed };
        result = ta_record_begin(out, fixed, &frame);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_tel_trace), NULL, 0u, 0u };
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, list_ids), TA_RECORD_TEXT(texts->list_ids));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, session_id), TA_RECORD_TEXT(texts->session_id));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, cause_session_id), TA_RECORD_TEXT(texts->cause_session_id));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, address), TA_RECORD_TEXT(texts->address));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, family_name), TA_RECORD_TEXT(texts->family_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, revision), TA_RECORD_TEXT(texts->revision));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, document_name), TA_RECORD_TEXT(texts->document_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, source), TA_RECORD_TEXT(texts->source));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, state_name), TA_RECORD_TEXT(texts->state_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, transition_name), TA_RECORD_TEXT(texts->transition_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, action_name), TA_RECORD_TEXT(texts->action_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, subject_state_name), TA_RECORD_TEXT(texts->subject_state_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, subject_action_name), TA_RECORD_TEXT(texts->subject_action_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, name), TA_RECORD_TEXT(texts->name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, event_name), TA_RECORD_TEXT(texts->event_name));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, event_send_id), TA_RECORD_TEXT(texts->event_send_id));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, event_invoke_id), TA_RECORD_TEXT(texts->event_invoke_id));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, event_data), TA_RECORD_TEXT(texts->event_data));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, send_id), TA_RECORD_TEXT(texts->send_id));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, invoke_id), TA_RECORD_TEXT(texts->invoke_id));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, value_text), TA_RECORD_TEXT(texts->value_text));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, fault_detail), TA_RECORD_TEXT(texts->fault_detail));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, fault_message), TA_RECORD_TEXT(texts->fault_message));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, fault_data), TA_RECORD_TEXT(texts->fault_data));
    ta_record_put(&tail, (uint32_t)offsetof(ta_tel_trace, list_names), TA_RECORD_TEXT(texts->list_names));
    ta_record_finish(out, &tail);
    return TA_OK;
}

/* ---- tel.dropped ---- */

ta_result ta_tel_dropped_write(ta_record_buffer out, const ta_tel_dropped *fixed) {
    const ta_record_frame frame = { TA_RECORD_KIND_TEL_DROPPED, (uint32_t)sizeof(ta_tel_dropped), (uint32_t)sizeof(ta_tel_dropped) };
    ta_result result = ta_record_begin(out, fixed, &frame);
    if (result == TA_OK) {
        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(ta_tel_dropped), NULL, 0u, 0u };
        ta_record_finish(out, &tail);
    }
    return result;
}

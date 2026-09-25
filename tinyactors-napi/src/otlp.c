#include "otlp.h"

#include <string.h>

/*
 * Field numbers of opentelemetry-proto (v1): common/v1, resource/v1,
 * trace/v1 and collector/trace/v1. Stable since OTLP 1.0.
 */
enum {
    /* ExportTraceServiceRequest */
    F_REQUEST_RESOURCE_SPANS = 1,
    /* ResourceSpans */
    F_RS_RESOURCE = 1,
    F_RS_SCOPE_SPANS = 2,
    /* Resource */
    F_RESOURCE_ATTRIBUTES = 1,
    /* ScopeSpans */
    F_SS_SCOPE = 1,
    F_SS_SPANS = 2,
    /* InstrumentationScope */
    F_SCOPE_NAME = 1,
    F_SCOPE_VERSION = 2,
    /* Span */
    F_SPAN_TRACE_ID = 1,
    F_SPAN_SPAN_ID = 2,
    F_SPAN_PARENT_SPAN_ID = 4,
    F_SPAN_NAME = 5,
    F_SPAN_KIND = 6,
    F_SPAN_START = 7,
    F_SPAN_END = 8,
    F_SPAN_ATTRIBUTES = 9,
    F_SPAN_EVENTS = 11,
    F_SPAN_LINKS = 13,
    F_SPAN_STATUS = 15,
    /* Span.Event */
    F_EVENT_TIME = 1,
    F_EVENT_NAME = 2,
    F_EVENT_ATTRIBUTES = 3,
    /* Span.Link */
    F_LINK_TRACE_ID = 1,
    F_LINK_SPAN_ID = 2,
    /* Status */
    F_STATUS_CODE = 3,
    /* KeyValue */
    F_KV_KEY = 1,
    F_KV_VALUE = 2,
    /* AnyValue */
    F_ANY_STRING = 1,
    F_ANY_BOOL = 2,
    F_ANY_INT = 3,
    F_ANY_ARRAY = 5,
    /* ArrayValue */
    F_ARRAY_VALUES = 1,
};

enum { WIRE_VARINT = 0, WIRE_I64 = 1, WIRE_LEN = 2 };
enum { SPAN_KIND_INTERNAL = 1, STATUS_CODE_ERROR = 2 };

#define LIT(text) ((ta_str){ (text), (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * Bytes
 */

static void tna_buf_init(tna_otlp_buf *buf, ta_allocator allocator) {
    *buf = (tna_otlp_buf){ .allocator = allocator };
}

static void tna_buf_free(tna_otlp_buf *buf) {
    if (buf->data != NULL) {
        buf->allocator.v->free(buf->allocator.self,
                               &(ta_allocator_free_params){ .ptr = buf->data, .size = buf->cap, .alignment = 1 });
    }
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/* Room for `more` bytes past the end; false when the allocator refuses. */
static bool tna_buf_room(tna_otlp_buf *buf, size_t more) {
    if (buf->len + more <= buf->cap) {
        return true;
    }
    size_t cap = buf->cap == 0 ? 256u : buf->cap;
    while (cap < buf->len + more) {
        cap *= 2u;
    }
    uint8_t *data = buf->allocator.v->alloc(buf->allocator.self, cap, 1);
    if (data == NULL) {
        return false;
    }
    size_t len = buf->len;
    if (len != 0) {
        memcpy(data, buf->data, len);
    }
    tna_buf_free(buf);
    buf->data = data;
    buf->len = len;
    buf->cap = cap;
    return true;
}

static size_t tna_varint_size(uint64_t value) {
    size_t size = 1;
    while (value >= 0x80u) {
        value >>= 7;
        size += 1u;
    }
    return size;
}

/* The writers below assume the room was made first. */
static void tna_put_varint(tna_otlp_buf *buf, uint64_t value) {
    while (value >= 0x80u) {
        buf->data[buf->len++] = (uint8_t)(value | 0x80u);
        value >>= 7;
    }
    buf->data[buf->len++] = (uint8_t)value;
}

static void tna_put_tag(tna_otlp_buf *buf, uint32_t field, uint32_t wire) {
    tna_put_varint(buf, ((uint64_t)field << 3) | wire);
}

static void tna_put_bytes(tna_otlp_buf *buf, const void *bytes, size_t len) {
    if (len != 0) {
        memcpy(buf->data + buf->len, bytes, len);
        buf->len += len;
    }
}

static void tna_put_fixed64(tna_otlp_buf *buf, uint64_t value) {
    for (int at = 0; at < 8; at += 1) {
        buf->data[buf->len++] = (uint8_t)(value >> (8 * at));
    }
}

/* Span and trace IDs are big-endian bytes: their hex form reads the same. */
static void tna_put_id64(tna_otlp_buf *buf, uint64_t value) {
    for (int at = 7; at >= 0; at -= 1) {
        buf->data[buf->len++] = (uint8_t)(value >> (8 * at));
    }
}

/* The size of a length-delimited field whose body is `len` bytes. */
static size_t tna_len_field_size(uint32_t field, size_t len) {
    return tna_varint_size((uint64_t)field << 3) + tna_varint_size(len) + len;
}

static void tna_put_len_header(tna_otlp_buf *buf, uint32_t field, size_t len) {
    tna_put_tag(buf, field, WIRE_LEN);
    tna_put_varint(buf, len);
}

/* ------------------------------------------------------------------------
 * Attributes: KeyValue { key, AnyValue }
 */

/* AnyValue sizes. */
static size_t tna_any_string_size(size_t len) { return tna_len_field_size(F_ANY_STRING, len); }
static size_t tna_any_int_size(int64_t value) { return 1u + tna_varint_size((uint64_t)value); }

static size_t tna_kv_size(ta_str key, size_t any) {
    return tna_len_field_size(F_KV_KEY, key.len) + tna_len_field_size(F_KV_VALUE, any);
}

static void tna_put_kv_head(tna_otlp_buf *buf, uint32_t field, ta_str key, size_t any) {
    tna_put_len_header(buf, field, tna_kv_size(key, any));
    tna_put_len_header(buf, F_KV_KEY, key.len);
    tna_put_bytes(buf, key.ptr, key.len);
    tna_put_len_header(buf, F_KV_VALUE, any);
}

static bool tna_kv_string(tna_otlp_buf *buf, uint32_t field, ta_str key, ta_str value) {
    size_t any = tna_any_string_size(value.len);
    if (!tna_buf_room(buf, tna_len_field_size(field, tna_kv_size(key, any)))) {
        return false;
    }
    tna_put_kv_head(buf, field, key, any);
    tna_put_len_header(buf, F_ANY_STRING, value.len);
    tna_put_bytes(buf, value.ptr, value.len);
    return true;
}

static bool tna_kv_int(tna_otlp_buf *buf, uint32_t field, ta_str key, int64_t value) {
    size_t any = tna_any_int_size(value);
    if (!tna_buf_room(buf, tna_len_field_size(field, tna_kv_size(key, any)))) {
        return false;
    }
    tna_put_kv_head(buf, field, key, any);
    tna_put_tag(buf, F_ANY_INT, WIRE_VARINT);
    tna_put_varint(buf, (uint64_t)value);
    return true;
}

static bool tna_kv_bool(tna_otlp_buf *buf, uint32_t field, ta_str key, bool value) {
    size_t any = 2u;
    if (!tna_buf_room(buf, tna_len_field_size(field, tna_kv_size(key, any)))) {
        return false;
    }
    tna_put_kv_head(buf, field, key, any);
    tna_put_tag(buf, F_ANY_BOOL, WIRE_VARINT);
    tna_put_varint(buf, value ? 1u : 0u);
    return true;
}

static bool tna_kv_int_array(tna_otlp_buf *buf, uint32_t field, ta_str key, const uint32_t *values, uint32_t count) {
    size_t array = 0;
    for (uint32_t at = 0; at < count; at += 1u) {
        array += tna_len_field_size(F_ARRAY_VALUES, tna_any_int_size(values[at]));
    }
    size_t any = tna_len_field_size(F_ANY_ARRAY, array);
    if (!tna_buf_room(buf, tna_len_field_size(field, tna_kv_size(key, any)))) {
        return false;
    }
    tna_put_kv_head(buf, field, key, any);
    tna_put_len_header(buf, F_ANY_ARRAY, array);
    for (uint32_t at = 0; at < count; at += 1u) {
        tna_put_len_header(buf, F_ARRAY_VALUES, tna_any_int_size(values[at]));
        tna_put_tag(buf, F_ANY_INT, WIRE_VARINT);
        tna_put_varint(buf, values[at]);
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Names (core ADR 0071), cached per definition
 */

enum { NAME_STATE = 0u, NAME_TRANSITION = 1u, NAME_ACTION = 2u };
#define NAME_TAG(kind, id) (((uint32_t)(kind) << 30) | ((uint32_t)(id) & 0x3fffffffu))

static uint64_t tna_name_hash(uint64_t definition, uint32_t tag) {
    uint64_t value = definition * 0x9e3779b97f4a7c15ull ^ tag;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    return value;
}

static tna_otlp_name *tna_name_slot(const tna_otlp *otlp, uint64_t definition, uint32_t tag) {
    uint32_t mask = otlp->name_capacity - 1u;
    uint32_t at = (uint32_t)tna_name_hash(definition, tag) & mask;
    for (;;) {
        tna_otlp_name *slot = &otlp->name_slots[at];
        if (!slot->used || (slot->definition == definition && slot->tag == tag)) {
            return slot;
        }
        at = (at + 1u) & mask;
    }
}

static bool tna_names_grow(tna_otlp *otlp) {
    uint32_t capacity = otlp->name_capacity == 0 ? 64u : otlp->name_capacity * 2u;
    ta_allocator allocator = otlp->config.allocator;
    tna_otlp_name *slots = allocator.v->alloc(allocator.self, (size_t)capacity * sizeof *slots, _Alignof(tna_otlp_name));
    if (slots == NULL) {
        return false;
    }
    memset(slots, 0, (size_t)capacity * sizeof *slots);
    tna_otlp_name *old = otlp->name_slots;
    uint32_t old_capacity = otlp->name_capacity;
    otlp->name_slots = slots;
    otlp->name_capacity = capacity;
    for (uint32_t at = 0; at < old_capacity; at += 1u) {
        if (old[at].used) {
            *tna_name_slot(otlp, old[at].definition, old[at].tag) = old[at];
        }
    }
    if (old != NULL) {
        allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = old,
                                                                        .size = (size_t)old_capacity * sizeof *old,
                                                                        .alignment = _Alignof(tna_otlp_name) });
    }
    return true;
}

static ta_str tna_name_text(const tna_otlp *otlp, const tna_otlp_name *slot) {
    return (ta_str){ (const char *)otlp->names.data + slot->at, slot->len };
}

/*
 * Drops the names of definitions core has freed (napi ADR 0017 section 5).
 * Keys hold the definition's generation, so a stale entry never matches;
 * this only reclaims its room. The table is rebuilt in place, and the
 * labels still used move to the front of `names`.
 */
static void tna_names_sweep(tna_otlp *otlp) {
    uint32_t capacity = otlp->name_capacity;
    size_t text = 0;
    for (uint32_t at = 0; at < capacity; at += 1u) {
        tna_otlp_name *slot = &otlp->name_slots[at];
        ta_definition_inspection view;
        if (slot->used && ta_definition_inspect(otlp->config.core, slot->definition, &view) != TA_OK) {
            slot->used = false;
            otlp->name_count -= 1u;
        } else if (slot->used) {
            text += slot->len;
        }
    }
    /* The labels still used, copied to a fresh buffer; without memory for
     * it, their text stays where it is and only the slots are reclaimed. */
    tna_otlp_buf names;
    tna_buf_init(&names, otlp->config.allocator);
    bool compact = text == 0 || tna_buf_room(&names, text);
    for (uint32_t at = 0; compact && at < capacity; at += 1u) {
        tna_otlp_name *slot = &otlp->name_slots[at];
        if (slot->used) {
            size_t from = slot->at;
            slot->at = names.len;
            tna_put_bytes(&names, otlp->names.data + from, slot->len);
        }
    }
    if (compact) {
        tna_buf_free(&otlp->names);
        otlp->names = names;
    }
    /* Reinsert what is left, so no probe run has a hole in it. */
    for (uint32_t at = 0; at < capacity; at += 1u) {
        tna_otlp_name entry = otlp->name_slots[at];
        if (entry.used) {
            otlp->name_slots[at].used = false;
            *tna_name_slot(otlp, entry.definition, entry.tag) = entry;
        }
    }
}

/* Caches `otlp->label` under (definition, tag); the label as cached, or empty. */
static ta_str tna_name_keep(tna_otlp *otlp, uint64_t definition, uint32_t tag) {
    ta_str label = { (const char *)otlp->label.data, (uint32_t)otlp->label.len };
    if ((otlp->name_count + 1u) * 10u > otlp->name_capacity * 7u && otlp->name_capacity != 0) {
        tna_names_sweep(otlp);
    }
    if ((otlp->name_count + 1u) * 10u > otlp->name_capacity * 7u && !tna_names_grow(otlp)) {
        return label;
    }
    if (!tna_buf_room(&otlp->names, label.len)) {
        return label;
    }
    tna_otlp_name *slot = tna_name_slot(otlp, definition, tag);
    *slot = (tna_otlp_name){ .definition = definition, .tag = tag, .len = label.len, .at = otlp->names.len,
                             .used = true };
    tna_put_bytes(&otlp->names, label.ptr, label.len);
    otlp->name_count += 1u;
    return tna_name_text(otlp, slot);
}

static const tna_otlp_name *tna_name_find(const tna_otlp *otlp, uint64_t definition, uint32_t tag) {
    if (otlp->name_capacity == 0) {
        return NULL;
    }
    const tna_otlp_name *slot = tna_name_slot(otlp, definition, tag);
    return slot->used ? slot : NULL;
}

static bool tna_label_add(tna_otlp *otlp, ta_str text) {
    if (!tna_buf_room(&otlp->label, text.len)) {
        return false;
    }
    tna_put_bytes(&otlp->label, text.ptr, text.len);
    return true;
}

static bool tna_label_number(tna_otlp *otlp, const char *prefix, uint32_t value) {
    char digits[16];
    int at = (int)sizeof digits;
    do {
        digits[--at] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    return tna_label_add(otlp, (ta_str){ prefix, (uint32_t)strlen(prefix) }) &&
           tna_label_add(otlp, (ta_str){ digits + at, (uint32_t)(sizeof digits - (size_t)at) });
}

/* A state's author ID, or "s<id>" when its definition is gone. Not cached:
 * core's lookup is a read of the definition. */
static ta_str tna_state_name(tna_otlp *otlp, uint64_t definition, ta_state_id state) {
    ta_state_info info = { 0 };
    if (ta_definition_state(otlp->config.core, &(ta_definition_state_params){ .definition = definition, .state = state,
                                                                               .out = &info }) == TA_OK) {
        return info.id;
    }
    otlp->label.len = 0;
    const tna_otlp_name *cached = tna_name_find(otlp, definition, NAME_TAG(NAME_STATE, state));
    if (cached != NULL) {
        return tna_name_text(otlp, cached);
    }
    return tna_label_number(otlp, "s", state) ? tna_name_keep(otlp, definition, NAME_TAG(NAME_STATE, state))
                                              : LIT("?");
}

/* Appends a state's name to the label being built. */
static bool tna_label_state(tna_otlp *otlp, uint64_t definition, ta_state_id state) {
    ta_state_info info = { 0 };
    if (ta_definition_state(otlp->config.core, &(ta_definition_state_params){ .definition = definition, .state = state,
                                                                               .out = &info }) == TA_OK) {
        return tna_label_add(otlp, info.id);
    }
    return tna_label_number(otlp, "s", state);
}

/* Appends a transition's label: "idle → busy on order.placed [guarded]",
 * "initial of p → a", "busy on poke (internal)", or "t7" once it is gone. */
static bool tna_label_transition(tna_otlp *otlp, uint64_t definition, ta_transition_id transition) {
    ta_transition_info info = { 0 };
    if (ta_definition_transition(otlp->config.core,
                                 &(ta_definition_transition_params){ .definition = definition,
                                                                     .transition = transition,
                                                                     .out = &info }) != TA_OK) {
        return tna_label_number(otlp, "t", transition);
    }
    bool ok = true;
    if (info.origin != TA_TRANSITION_AUTHORED) {
        ok = tna_label_add(otlp, LIT("initial of "));
        ok = ok && (info.origin == TA_TRANSITION_BOOTSTRAP ? tna_label_add(otlp, LIT("the document"))
                                                           : tna_label_state(otlp, definition, info.source));
        ok = ok && tna_label_add(otlp, LIT(" \xe2\x86\x92"));
        for (uint32_t at = 0; ok && at < info.target_count; at += 1u) {
            ok = tna_label_add(otlp, LIT(" ")) && tna_label_state(otlp, definition, info.targets[at]);
        }
        return ok;
    }
    ok = tna_label_state(otlp, definition, info.source);
    if (ok && info.target_count != 0) {
        ok = tna_label_add(otlp, LIT(" \xe2\x86\x92"));
        for (uint32_t at = 0; ok && at < info.target_count; at += 1u) {
            ok = tna_label_add(otlp, LIT(" ")) && tna_label_state(otlp, definition, info.targets[at]);
        }
    }
    if (ok && info.event.len != 0) {
        ok = tna_label_add(otlp, LIT(" on ")) && tna_label_add(otlp, info.event);
    }
    if (ok && info.condition != TA_EXPR_ID_NONE) {
        ok = tna_label_add(otlp, LIT(" [guarded]"));
    }
    if (ok && info.type == TA_SCXML_TRANSITION_INTERNAL) {
        ok = tna_label_add(otlp, LIT(" (internal)"));
    }
    return ok;
}

static ta_str tna_transition_name(tna_otlp *otlp, uint64_t definition, ta_transition_id transition) {
    uint32_t tag = NAME_TAG(NAME_TRANSITION, transition);
    const tna_otlp_name *cached = tna_name_find(otlp, definition, tag);
    if (cached != NULL) {
        return tna_name_text(otlp, cached);
    }
    otlp->label.len = 0;
    return tna_label_transition(otlp, definition, transition) ? tna_name_keep(otlp, definition, tag) : LIT("?");
}

static const char *const tna_action_elements[] = {
    "<?>", "<raise>", "<send>", "<cancel>", "<assign>", "<script>", "<log>", "<if>", "<foreach>", "<invoke>",
};

/* An action's label: "<assign> in onentry of busy", "<raise> in then of
 * <if> in onexit of idle", "<log> in finalize of <invoke> #2 in a". It
 * walks up from the action to its block's owner, one level at a time. */
static ta_str tna_action_name(tna_otlp *otlp, uint64_t definition, ta_action_id action) {
    uint32_t tag = NAME_TAG(NAME_ACTION, action);
    const tna_otlp_name *cached = tna_name_find(otlp, definition, tag);
    if (cached != NULL) {
        return tna_name_text(otlp, cached);
    }
    otlp->label.len = 0;
    bool ok = true;
    ta_action_id id = action;
    for (;;) {
        ta_action_info info = { 0 };
        if (ta_definition_action(otlp->config.core, &(ta_definition_action_params){ .definition = definition,
                                                                                    .action = id,
                                                                                    .out = &info }) != TA_OK) {
            otlp->label.len = 0;
            ok = tna_label_number(otlp, "a", action);
            break;
        }
        const char *element = tna_action_elements[(uint32_t)info.kind < 10u ? (uint32_t)info.kind : 0u];
        ok = ok && tna_label_add(otlp, (ta_str){ element, (uint32_t)strlen(element) });
        if (info.block == TA_ACTION_IN_THEN || info.block == TA_ACTION_IN_ELSE || info.block == TA_ACTION_IN_FOREACH) {
            ok = ok && tna_label_add(otlp, info.block == TA_ACTION_IN_THEN   ? LIT(" in then of ")
                                           : info.block == TA_ACTION_IN_ELSE ? LIT(" in else of ")
                                                                             : LIT(" in the body of "));
            id = info.owner;
            if (!ok) {
                break;
            }
            continue;
        }
        switch (info.block) {
            case TA_ACTION_IN_ONENTRY:
            case TA_ACTION_IN_ONEXIT:
                ok = ok && tna_label_add(otlp, info.block == TA_ACTION_IN_ONENTRY ? LIT(" in onentry") : LIT(" in onexit"));
                if (ok && info.block_index > 0) {
                    ok = tna_label_number(otlp, " #", info.block_index + 1u);
                }
                ok = ok && tna_label_add(otlp, LIT(" of ")) && tna_label_state(otlp, definition, info.owner);
                break;
            case TA_ACTION_IN_TRANSITION:
                ok = ok && tna_label_add(otlp, LIT(" in ")) && tna_label_transition(otlp, definition, info.owner);
                break;
            case TA_ACTION_IN_FINALIZE:
                ok = ok && tna_label_add(otlp, LIT(" in finalize of <invoke>"));
                if (ok && info.block_index > 0) {
                    ok = tna_label_number(otlp, " #", info.block_index + 1u);
                }
                ok = ok && tna_label_add(otlp, LIT(" in ")) && tna_label_state(otlp, definition, info.owner);
                break;
            default:
                ok = ok && tna_label_add(otlp, LIT(" in the document's <script>"));
                break;
        }
        break;
    }
    return ok ? tna_name_keep(otlp, definition, tag) : LIT("?");
}

/* ------------------------------------------------------------------------
 * Text core formats: event names, send IDs, invoke IDs
 */

static ta_str tna_event_name(tna_otlp *otlp, ta_event_name_id name) {
    uint32_t needed = 0;
    ta_event_name_format_params params = { .name = name, .buffer = (char *)otlp->text.data,
                                           .capacity = (uint32_t)otlp->text.cap, .needed = &needed };
    ta_result result = ta_event_name_format(otlp->config.core, &params);
    if (result == TA_ERR_NOMEM && needed > otlp->text.cap) {
        otlp->text.len = 0;
        if (!tna_buf_room(&otlp->text, needed)) {
            return LIT("?");
        }
        params.buffer = (char *)otlp->text.data;
        params.capacity = (uint32_t)otlp->text.cap;
        result = ta_event_name_format(otlp->config.core, &params);
    }
    if (result != TA_OK) {
        return LIT("?");
    }
    return (ta_str){ (const char *)otlp->text.data, needed > 0 ? needed - 1u : 0u };
}

/* A send ID's text into `out` (at most 128 bytes), or empty for none. */
static ta_str tna_send_text(tna_otlp *otlp, ta_send_id send, char out[128]) {
    uint32_t needed = 0;
    if (send == 0 || ta_send_id_format(otlp->config.core, &(ta_send_id_format_params){ .send_id = send, .buffer = out,
                                                                                         .capacity = 128,
                                                                                         .needed = &needed }) != TA_OK) {
        return (ta_str){ NULL, 0 };
    }
    return (ta_str){ out, needed > 0 ? needed - 1u : 0u };
}

static ta_str tna_invoke_text(tna_otlp *otlp, ta_invoke_id invoke) {
    ta_str text = { NULL, 0 };
    if (invoke == TA_INVOKE_ID_NONE || ta_system_invoke_id_text(otlp->config.core, invoke, &text) != TA_OK) {
        return (ta_str){ NULL, 0 };
    }
    return text;
}

/* Decimal text for a u64 into `out` (at least 21 bytes). */
static ta_str tna_decimal(uint64_t value, char *out) {
    char digits[24];
    int at = (int)sizeof digits;
    do {
        digits[--at] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    size_t len = sizeof digits - (size_t)at;
    memcpy(out, digits + at, len);
    return (ta_str){ out, (uint32_t)len };
}

/* ------------------------------------------------------------------------
 * The encoder
 */

static const char *const tna_kind_names[] = {
    "spawned",   "loaded",    "unloaded", "finished", "destroyed", "macrostep_begin", "macrostep_end", "condition",
    "microstep", "raise",     "data",     "assign",   "script",    "foreach",         "error",         "send",
    "cancel",    "invoke",    "invoke.cancel", "paused", "resumed", "stopped",  "ignored",
};
_Static_assert(sizeof tna_kind_names / sizeof tna_kind_names[0] == (size_t)TA_TRACE_IGNORED + 1u,
               "one name per ta_trace_kind");

static const char *const tna_at_names[] = { "none", "select", "exit", "transition", "enter", "bootstrap", "finalize",
                                            "invoke" };
static const char *const tna_event_types[] = { "external", "internal", "platform" };

static ta_str tna_cstr(const char *text) { return (ta_str){ text, (uint32_t)strlen(text) }; }

static void tna_span_init(tna_otlp_span *span, ta_allocator allocator);

void tna_otlp_init(tna_otlp *otlp, const tna_otlp_config *config) {
    *otlp = (tna_otlp){ .config = *config };
    ta_allocator allocator = config->allocator;
    tna_buf_init(&otlp->names, allocator);
    tna_buf_init(&otlp->resource, allocator);
    tna_buf_init(&otlp->spans, allocator);
    tna_buf_init(&otlp->request, allocator);
    tna_buf_init(&otlp->scratch, allocator);
    tna_buf_init(&otlp->label, allocator);
    tna_buf_init(&otlp->text, allocator);
    tna_span_init(&otlp->lifecycle, allocator);
}

ta_result tna_otlp_set_resource(tna_otlp *otlp, const tna_otlp_attribute *attributes, uint32_t count) {
    otlp->resource.len = 0;
    for (uint32_t at = 0; at < count; at += 1u) {
        if (!tna_kv_string(&otlp->resource, F_RESOURCE_ATTRIBUTES, attributes[at].key, attributes[at].value)) {
            otlp->resource.len = 0;
            return TA_ERR_NOMEM;
        }
    }
    return TA_OK;
}

static void tna_span_init(tna_otlp_span *span, ta_allocator allocator) {
    *span = (tna_otlp_span){ 0 };
    tna_buf_init(&span->name, allocator);
    tna_buf_init(&span->attributes, allocator);
    tna_buf_init(&span->events, allocator);
}

static void tna_span_free(tna_otlp_span *span) {
    tna_buf_free(&span->name);
    tna_buf_free(&span->attributes);
    tna_buf_free(&span->events);
}

/* Empties a span for reuse, keeping its buffers' capacity. */
static void tna_span_reset(tna_otlp_span *span) {
    tna_otlp_buf name = span->name, attributes = span->attributes, events = span->events;
    *span = (tna_otlp_span){ 0 };
    span->name = name;
    span->attributes = attributes;
    span->events = events;
    span->name.len = 0;
    span->attributes.len = 0;
    span->events.len = 0;
}

static uint32_t tna_open_hash(uint64_t session) {
    session ^= session >> 33;
    session *= 0xff51afd7ed558ccdull;
    session ^= session >> 33;
    return (uint32_t)session;
}

/* The index slot holding `session`, or the empty slot where it would go. */
static uint32_t tna_open_slot(const tna_otlp *otlp, uint64_t session) {
    uint32_t mask = otlp->open_index_capacity - 1u;
    uint32_t at = tna_open_hash(session) & mask;
    while (otlp->open_index[at] != 0 && otlp->open[otlp->open_index[at] - 1u].session != session) {
        at = (at + 1u) & mask;
    }
    return at;
}

static tna_otlp_open *tna_open_find(tna_otlp *otlp, uint64_t session) {
    if (otlp->open_count == 0) {
        return NULL;
    }
    uint32_t entry = otlp->open_index[tna_open_slot(otlp, session)];
    return entry == 0 ? NULL : &otlp->open[entry - 1u];
}

static void tna_open_index_free(tna_otlp *otlp) {
    if (otlp->open_index != NULL) {
        ta_allocator allocator = otlp->config.allocator;
        allocator.v->free(allocator.self,
                          &(ta_allocator_free_params){ .ptr = otlp->open_index,
                                                       .size = (size_t)otlp->open_index_capacity * sizeof(uint32_t),
                                                       .alignment = _Alignof(uint32_t) });
    }
    otlp->open_index = NULL;
    otlp->open_index_capacity = 0;
}

/* An index twice the size of the open array, rebuilt from the open entries. */
static bool tna_open_index_build(tna_otlp *otlp, uint32_t capacity) {
    ta_allocator allocator = otlp->config.allocator;
    uint32_t *index = allocator.v->alloc(allocator.self, (size_t)capacity * sizeof(uint32_t), _Alignof(uint32_t));
    if (index == NULL) {
        return false;
    }
    memset(index, 0, (size_t)capacity * sizeof(uint32_t));
    tna_open_index_free(otlp);
    otlp->open_index = index;
    otlp->open_index_capacity = capacity;
    for (uint32_t at = 0; at < otlp->open_count; at += 1u) {
        otlp->open_index[tna_open_slot(otlp, otlp->open[at].session)] = at + 1u;
    }
    return true;
}

/* Removes `session` from the index, shifting the probe run back over the gap. */
static void tna_open_unindex(tna_otlp *otlp, uint64_t session) {
    uint32_t mask = otlp->open_index_capacity - 1u;
    uint32_t hole = tna_open_slot(otlp, session);
    otlp->open_index[hole] = 0;
    for (uint32_t at = (hole + 1u) & mask; otlp->open_index[at] != 0; at = (at + 1u) & mask) {
        uint32_t home = tna_open_hash(otlp->open[otlp->open_index[at] - 1u].session) & mask;
        /* Move it into the hole unless its home lies cyclically in (hole, at]. */
        bool stays = hole <= at ? (home > hole && home <= at) : (home > hole || home <= at);
        if (!stays) {
            otlp->open_index[hole] = otlp->open_index[at];
            otlp->open_index[at] = 0;
            hole = at;
        }
    }
}

/* A fresh open entry for `session`, reusing a spare one when there is one. */
static tna_otlp_open *tna_open_add(tna_otlp *otlp, uint64_t session) {
    if (otlp->open_count == otlp->open_capacity) {
        uint32_t capacity = otlp->open_capacity == 0 ? 8u : otlp->open_capacity * 2u;
        ta_allocator allocator = otlp->config.allocator;
        tna_otlp_open *open = allocator.v->alloc(allocator.self, (size_t)capacity * sizeof *open,
                                                 _Alignof(tna_otlp_open));
        if (open == NULL) {
            return NULL;
        }
        /* The spares past `open_count` own buffers too: move them all. */
        if (otlp->open_capacity != 0) {
            memcpy(open, otlp->open, (size_t)otlp->open_capacity * sizeof *open);
        }
        for (uint32_t at = otlp->open_capacity; at < capacity; at += 1u) {
            open[at].session = 0;
            tna_span_init(&open[at].macro, allocator);
            tna_span_init(&open[at].micro, allocator);
        }
        if (otlp->open != NULL) {
            allocator.v->free(allocator.self,
                              &(ta_allocator_free_params){ .ptr = otlp->open,
                                                           .size = (size_t)otlp->open_capacity * sizeof *otlp->open,
                                                           .alignment = _Alignof(tna_otlp_open) });
        }
        otlp->open = open;
        otlp->open_capacity = capacity;
    }
    if (otlp->open_index_capacity < otlp->open_capacity * 2u &&
        !tna_open_index_build(otlp, otlp->open_capacity * 2u)) {
        return NULL;
    }
    tna_otlp_open *entry = &otlp->open[otlp->open_count++];
    entry->session = session;
    entry->has_micro = false;
    tna_span_reset(&entry->macro);
    tna_span_reset(&entry->micro);
    otlp->open_index[tna_open_slot(otlp, session)] = otlp->open_count;
    return entry;
}

/* Removes an open entry, keeping its buffers as the spare past the end. */
static void tna_open_remove(tna_otlp *otlp, tna_otlp_open *entry) {
    tna_open_unindex(otlp, entry->session);
    tna_otlp_open *last = &otlp->open[otlp->open_count - 1u];
    if (entry != last) {
        uint32_t moved = tna_open_slot(otlp, last->session);
        tna_otlp_open spare = *entry;
        *entry = *last;
        *last = spare;
        otlp->open_index[moved] = (uint32_t)(entry - otlp->open) + 1u;
    }
    otlp->open_count -= 1u;
}

static bool tna_context_empty(const ta_trace_context *context) {
    return context->trace_id_high == 0 && context->trace_id_low == 0;
}

static void tna_trace_id_of(tna_otlp *otlp, const ta_trace_context *context, uint8_t out[16]) {
    if (tna_context_empty(context)) {
        otlp->config.host.trace_id(otlp->config.host.self, out);
        return;
    }
    for (int at = 0; at < 8; at += 1) {
        out[at] = (uint8_t)(context->trace_id_high >> (56 - 8 * at));
        out[8 + at] = (uint8_t)(context->trace_id_low >> (56 - 8 * at));
    }
}

static bool tna_key_empty(const ta_step_key *key) {
    return key->session == 0 && key->macrostep == 0 && key->microstep == 0;
}

/* Appends a finished span to `spans`. */
static bool tna_span_end(tna_otlp *otlp, const tna_otlp_span *span, uint64_t end) {
    size_t links = 0;
    if (span->link_span_id != 0) {
        links = tna_len_field_size(F_LINK_TRACE_ID, 16) + tna_len_field_size(F_LINK_SPAN_ID, 8);
    }
    size_t body = tna_len_field_size(F_SPAN_TRACE_ID, 16) + tna_len_field_size(F_SPAN_SPAN_ID, 8) +
                  (span->parent_span_id != 0 ? tna_len_field_size(F_SPAN_PARENT_SPAN_ID, 8) : 0) +
                  tna_len_field_size(F_SPAN_NAME, span->name.len) + 2u /* kind */ + 9u + 9u /* start, end */ +
                  span->attributes.len + span->events.len + (links != 0 ? tna_len_field_size(F_SPAN_LINKS, links) : 0) +
                  (span->error ? tna_len_field_size(F_SPAN_STATUS, 2) : 0);
    if (!tna_buf_room(&otlp->spans, tna_len_field_size(F_SS_SPANS, body))) {
        return false;
    }
    tna_otlp_buf *out = &otlp->spans;
    tna_put_len_header(out, F_SS_SPANS, body);
    tna_put_len_header(out, F_SPAN_TRACE_ID, 16);
    tna_put_bytes(out, span->trace_id, 16);
    tna_put_len_header(out, F_SPAN_SPAN_ID, 8);
    tna_put_id64(out, span->span_id);
    if (span->parent_span_id != 0) {
        tna_put_len_header(out, F_SPAN_PARENT_SPAN_ID, 8);
        tna_put_id64(out, span->parent_span_id);
    }
    tna_put_len_header(out, F_SPAN_NAME, span->name.len);
    tna_put_bytes(out, span->name.data, span->name.len);
    tna_put_tag(out, F_SPAN_KIND, WIRE_VARINT);
    tna_put_varint(out, SPAN_KIND_INTERNAL);
    tna_put_tag(out, F_SPAN_START, WIRE_I64);
    tna_put_fixed64(out, span->start);
    tna_put_tag(out, F_SPAN_END, WIRE_I64);
    tna_put_fixed64(out, end < span->start ? span->start : end);
    tna_put_bytes(out, span->attributes.data, span->attributes.len);
    tna_put_bytes(out, span->events.data, span->events.len);
    if (links != 0) {
        tna_put_len_header(out, F_SPAN_LINKS, links);
        tna_put_len_header(out, F_LINK_TRACE_ID, 16);
        tna_put_bytes(out, span->link_trace_id, 16);
        tna_put_len_header(out, F_LINK_SPAN_ID, 8);
        tna_put_id64(out, span->link_span_id);
    }
    if (span->error) {
        tna_put_len_header(out, F_SPAN_STATUS, 2);
        tna_put_tag(out, F_STATUS_CODE, WIRE_VARINT);
        tna_put_varint(out, STATUS_CODE_ERROR);
    }
    otlp->span_count += 1u;
    return true;
}

static bool tna_span_name(tna_otlp_span *span, ta_str prefix, ta_str rest) {
    span->name.len = 0;
    if (!tna_buf_room(&span->name, prefix.len + rest.len)) {
        return false;
    }
    tna_put_bytes(&span->name, prefix.ptr, prefix.len);
    tna_put_bytes(&span->name, rest.ptr, rest.len);
    return true;
}

/* Attributes every span and span event may carry about the record's event. */
static bool tna_event_attributes(tna_otlp *otlp, tna_otlp_buf *buf, uint32_t field, const ta_event *event) {
    if (event == NULL) {
        return true;
    }
    char send[128];
    ta_str sendid = tna_send_text(otlp, event->send_id, send);
    ta_str invokeid = tna_invoke_text(otlp, event->invoke_id);
    bool ok = tna_kv_string(buf, field, LIT("scxml.event.name"), tna_event_name(otlp, event->name)) &&
              tna_kv_string(buf, field, LIT("scxml.event.type"),
                            tna_cstr(tna_event_types[event->type <= TA_EVENT_PLATFORM ? event->type : 0]));
    if (ok && sendid.len != 0) {
        ok = tna_kv_string(buf, field, LIT("scxml.event.sendid"), sendid);
    }
    if (ok && invokeid.len != 0) {
        ok = tna_kv_string(buf, field, LIT("scxml.event.invokeid"), invokeid);
    }
    if (ok && otlp->config.values && event->data.v != NULL && otlp->config.host.value_text != NULL) {
        ta_str text = { NULL, 0 };
        if (otlp->config.host.value_text(otlp->config.host.self, &event->data, &text)) {
            ok = tna_kv_string(buf, field, LIT("scxml.event.data"), text);
        }
    }
    return ok;
}

/* The attributes of a step's span: the session, step and actor. */
static bool tna_step_attributes(tna_otlp *otlp, tna_otlp_buf *buf, const ta_trace_record *record) {
    char session[24];
    char actor[48];
    ta_str slot = tna_decimal(ta_actor_id_slot(record->actor), actor);
    actor[slot.len] = ':';
    ta_str generation = tna_decimal(ta_actor_id_generation(record->actor), actor + slot.len + 1u);
    (void)otlp;
    return tna_kv_string(buf, F_SPAN_ATTRIBUTES, LIT("scxml.session_id"), tna_decimal(record->key.session, session)) &&
           tna_kv_int(buf, F_SPAN_ATTRIBUTES, LIT("scxml.macrostep"), (int64_t)record->key.macrostep) &&
           tna_kv_string(buf, F_SPAN_ATTRIBUTES, LIT("scxml.actor"),
                         (ta_str){ actor, slot.len + 1u + generation.len }) &&
           tna_kv_int(buf, F_SPAN_ATTRIBUTES, LIT("scxml.definition"), (int64_t)record->definition) &&
           tna_kv_int(buf, F_SPAN_ATTRIBUTES, LIT("scxml.time"), (int64_t)record->now);
}

/* A cause, as three attributes (the span parent says the same, lossily). */
static bool tna_cause_attributes(tna_otlp_buf *buf, uint32_t field, const ta_step_key *cause) {
    if (tna_key_empty(cause)) {
        return true;
    }
    char session[24];
    return tna_kv_string(buf, field, LIT("scxml.cause.session_id"), tna_decimal(cause->session, session)) &&
           tna_kv_int(buf, field, LIT("scxml.cause.macrostep"), (int64_t)cause->macrostep) &&
           tna_kv_int(buf, field, LIT("scxml.cause.microstep"), (int64_t)cause->microstep);
}

/* A list of states: names, and IDs (names are unique within a chart, but a
 * gone definition answers with numbers). */
static bool tna_states_attribute(tna_otlp *otlp, tna_otlp_buf *buf, ta_str key, ta_str id_key,
                                 const ta_trace_record *record, const ta_state_id *states, uint32_t count) {
    /* Two passes over the names: sizes first, then bytes. */
    size_t array = 0;
    for (uint32_t at = 0; at < count; at += 1u) {
        ta_str name = tna_state_name(otlp, record->definition, states[at]);
        array += tna_len_field_size(F_ARRAY_VALUES, tna_any_string_size(name.len));
    }
    size_t any = tna_len_field_size(F_ANY_ARRAY, array);
    if (!tna_buf_room(buf, tna_len_field_size(F_SPAN_ATTRIBUTES, tna_kv_size(key, any)))) {
        return false;
    }
    tna_put_kv_head(buf, F_SPAN_ATTRIBUTES, key, any);
    tna_put_len_header(buf, F_ANY_ARRAY, array);
    for (uint32_t at = 0; at < count; at += 1u) {
        ta_str name = tna_state_name(otlp, record->definition, states[at]);
        tna_put_len_header(buf, F_ARRAY_VALUES, tna_any_string_size(name.len));
        tna_put_len_header(buf, F_ANY_STRING, name.len);
        tna_put_bytes(buf, name.ptr, name.len);
    }
    return tna_kv_int_array(buf, F_SPAN_ATTRIBUTES, id_key, states, count);
}

static bool tna_transitions_attribute(tna_otlp *otlp, tna_otlp_buf *buf, const ta_trace_record *record) {
    size_t array = 0;
    for (uint32_t at = 0; at < record->transition_count; at += 1u) {
        ta_str name = tna_transition_name(otlp, record->definition, record->transitions[at]);
        array += tna_len_field_size(F_ARRAY_VALUES, tna_any_string_size(name.len));
    }
    size_t any = tna_len_field_size(F_ANY_ARRAY, array);
    ta_str key = LIT("scxml.transition");
    if (!tna_buf_room(buf, tna_len_field_size(F_SPAN_ATTRIBUTES, tna_kv_size(key, any)))) {
        return false;
    }
    tna_put_kv_head(buf, F_SPAN_ATTRIBUTES, key, any);
    tna_put_len_header(buf, F_ANY_ARRAY, array);
    for (uint32_t at = 0; at < record->transition_count; at += 1u) {
        ta_str name = tna_transition_name(otlp, record->definition, record->transitions[at]);
        tna_put_len_header(buf, F_ARRAY_VALUES, tna_any_string_size(name.len));
        tna_put_len_header(buf, F_ANY_STRING, name.len);
        tna_put_bytes(buf, name.ptr, name.len);
    }
    return tna_kv_int_array(buf, F_SPAN_ATTRIBUTES, LIT("scxml.transition.id"), record->transitions,
                            record->transition_count);
}

/* Opens `span` for a record's step. */
static bool tna_span_open(tna_otlp *otlp, tna_otlp_span *span, const ta_trace_record *record, uint64_t now) {
    tna_span_reset(span);
    tna_trace_id_of(otlp, &record->context, span->trace_id);
    span->span_id = ta_step_key_span_id(record->key);
    span->start = now;
    span->microstep = record->key.microstep;
    return tna_step_attributes(otlp, &span->attributes, record);
}

/* Parents `span` under its cause, or links it there. */
static void tna_span_parent(tna_otlp *otlp, tna_otlp_span *span, const ta_step_key *cause) {
    if (tna_key_empty(cause)) {
        return;
    }
    uint64_t parent = ta_step_key_span_id(*cause);
    if (parent == 0) {
        return;
    }
    if (otlp->config.link_parents) {
        memcpy(span->link_trace_id, span->trace_id, 16);
        span->link_span_id = parent;
    } else {
        span->parent_span_id = parent;
    }
}

/* Ends a session's open spans, marked incomplete when `incomplete`. */
static bool tna_open_end(tna_otlp *otlp, tna_otlp_open *open, uint64_t now, bool incomplete) {
    bool ok = true;
    if (open->has_micro) {
        ok = (!incomplete || tna_kv_bool(&open->micro.attributes, F_SPAN_ATTRIBUTES, LIT("scxml.incomplete"), true)) &&
             tna_span_end(otlp, &open->micro, now);
        open->has_micro = false;
    }
    ok = ok && (!incomplete || tna_kv_bool(&open->macro.attributes, F_SPAN_ATTRIBUTES, LIT("scxml.incomplete"), true)) &&
         tna_span_end(otlp, &open->macro, now);
    tna_open_remove(otlp, open);
    return ok;
}

/* The session's open macrostep, opened as "continued" when a record comes
 * for a macrostep whose BEGIN this encoder never saw (a sink installed
 * mid-macrostep): no record is dropped. */
static tna_otlp_open *tna_open_for(tna_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tna_otlp_open *open = tna_open_find(otlp, record->key.session);
    if (open != NULL) {
        return open;
    }
    open = tna_open_add(otlp, record->key.session);
    if (open == NULL) {
        return NULL;
    }
    ta_trace_record begin = *record;
    begin.key.microstep = 0;
    if (!tna_span_open(otlp, &open->macro, &begin, now) ||
        !tna_span_name(&open->macro, LIT("scxml.macrostep"), LIT(" (continued)")) ||
        !tna_kv_bool(&open->macro.attributes, F_SPAN_ATTRIBUTES, LIT("scxml.continued"), true)) {
        tna_open_remove(otlp, open);
        return NULL;
    }
    return open;
}

static bool tna_begin(tna_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tna_otlp_open *open = tna_open_find(otlp, record->key.session);
    if (open != NULL && !tna_open_end(otlp, open, now, true)) {
        return false;
    }
    open = tna_open_add(otlp, record->key.session);
    if (open == NULL) {
        return false;
    }
    tna_otlp_span *span = &open->macro;
    ta_str what = record->event != NULL ? tna_event_name(otlp, record->event->name)
                  : record->key.macrostep == 1 ? LIT("bootstrap")
                                               : LIT("(no event)");
    bool ok = tna_span_open(otlp, span, record, now) && tna_span_name(span, LIT("scxml.macrostep "), what) &&
              tna_event_attributes(otlp, &span->attributes, F_SPAN_ATTRIBUTES, record->event) &&
              tna_cause_attributes(&span->attributes, F_SPAN_ATTRIBUTES, &record->cause) &&
              tna_kv_int(&span->attributes, F_SPAN_ATTRIBUTES, LIT("scxml.trace.flags"), record->context.flags);
    if (!ok) {
        tna_open_remove(otlp, open);
        return false;
    }
    tna_span_parent(otlp, span, &record->cause);
    return true;
}

static bool tna_microstep(tna_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tna_otlp_open *open = tna_open_for(otlp, record, now);
    if (open == NULL) {
        return false;
    }
    if (open->has_micro) {
        if (!tna_span_end(otlp, &open->micro, now)) {
            return false;
        }
        open->has_micro = false;
    }
    tna_otlp_span *span = &open->micro;
    ta_str what = record->event != NULL ? tna_event_name(otlp, record->event->name) : LIT("(eventless)");
    bool ok = tna_span_open(otlp, span, record, now) && tna_span_name(span, LIT("scxml.microstep "), what) &&
              tna_kv_int(&span->attributes, F_SPAN_ATTRIBUTES, LIT("scxml.microstep"), record->key.microstep) &&
              tna_event_attributes(otlp, &span->attributes, F_SPAN_ATTRIBUTES, record->event) &&
              tna_transitions_attribute(otlp, &span->attributes, record) &&
              tna_states_attribute(otlp, &span->attributes, LIT("scxml.state.exited"), LIT("scxml.state.exited.id"),
                                   record, record->exited, record->exited_count) &&
              tna_states_attribute(otlp, &span->attributes, LIT("scxml.state.entered"), LIT("scxml.state.entered.id"),
                                   record, record->entered, record->entered_count) &&
              (record->history_count == 0 ||
               tna_states_attribute(otlp, &span->attributes, LIT("scxml.state.history"),
                                    LIT("scxml.state.history.id"), record, record->history, record->history_count));
    if (!ok) {
        return false;
    }
    span->parent_span_id = open->macro.span_id;
    memcpy(span->trace_id, open->macro.trace_id, 16);
    open->has_micro = true;
    return true;
}

static bool tna_end(tna_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tna_otlp_open *open = tna_open_for(otlp, record, now);
    if (open == NULL) {
        return false;
    }
    if (!tna_states_attribute(otlp, &open->macro.attributes, LIT("scxml.state"), LIT("scxml.state.id"), record,
                              record->states, record->state_count)) {
        return false;
    }
    return tna_open_end(otlp, open, now, false);
}

/* Where in the macrostep a record was written. */
static bool tna_position_attributes(tna_otlp *otlp, tna_otlp_buf *buf, const ta_trace_record *record) {
    const ta_trace_position *at = &record->position;
    bool ok = true;
    if (at->at != TA_TRACE_AT_NONE) {
        ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.position"),
                           tna_cstr(tna_at_names[(uint32_t)at->at <= TA_TRACE_AT_INVOKE ? at->at : 0]));
    }
    if (ok && at->state != TA_STATE_NONE) {
        ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.state"),
                           tna_state_name(otlp, record->definition, at->state)) &&
             tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.state.id"), at->state);
    }
    if (ok && at->transition != TA_TRANSITION_NONE) {
        ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.transition"),
                           tna_transition_name(otlp, record->definition, at->transition)) &&
             tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.transition.id"), at->transition);
    }
    if (ok && at->action != TA_ACTION_NONE) {
        ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action"),
                           tna_action_name(otlp, record->definition, at->action)) &&
             tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action.id"), at->action);
    }
    if (ok && (at->iteration != 0 || record->kind == TA_TRACE_FOREACH)) {
        ok = tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.iteration"), at->iteration);
    }
    return ok;
}

/* The fields of a record that becomes a span event. */
static bool tna_record_attributes(tna_otlp *otlp, tna_otlp_buf *buf, const ta_trace_record *record) {
    bool ok = tna_position_attributes(otlp, buf, record) &&
              tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.microstep"), record->key.microstep) &&
              tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.time"), (int64_t)record->now) &&
              tna_event_attributes(otlp, buf, F_EVENT_ATTRIBUTES, record->event);
    if (ok && record->expr != TA_EXPR_ID_NONE) {
        ok = tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.expression.id"), record->expr);
    }
    if (ok && record->kind == TA_TRACE_CONDITION && !record->has_fault) {
        ok = tna_kv_bool(buf, F_EVENT_ATTRIBUTES, LIT("scxml.result"), record->result);
    }
    if (ok && record->name.len != 0) {
        ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.data.name"), record->name);
    }
    if (ok && record->action != TA_ACTION_NONE && record->action != record->position.action) {
        ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action"),
                           tna_action_name(otlp, record->definition, record->action)) &&
             tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action.id"), record->action);
    }
    if (ok && record->iteration != 0 && record->iteration != record->position.iteration) {
        ok = tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.iteration"), record->iteration);
    }
    if (ok && record->send != 0) {
        char send[128];
        ta_str text = tna_send_text(otlp, record->send, send);
        if (text.len != 0) {
            ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.send_id"), text);
        }
    }
    if (ok && record->invoke != TA_INVOKE_ID_NONE) {
        ta_str text = tna_invoke_text(otlp, record->invoke);
        if (text.len != 0) {
            ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.invoke_id"), text);
        }
    }
    if (ok && record->has_fault) {
        ok = tna_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.fault.code"), record->fault.code);
        if (ok && record->fault.detail != 0) {
            ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.fault.detail"),
                               tna_event_name(otlp, record->fault.detail));
        }
    }
    if (ok && otlp->config.values && record->has_value && otlp->config.host.value_text != NULL) {
        ta_str text = { NULL, 0 };
        if (otlp->config.host.value_text(otlp->config.host.self, &record->value, &text)) {
            ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.value"), text);
        }
    }
    if (ok && record->state != TA_STATE_NONE) {
        ok = tna_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.final_state"),
                           tna_state_name(otlp, record->definition, record->state));
    }
    return ok && tna_cause_attributes(buf, F_EVENT_ATTRIBUTES, &record->cause);
}

/* A record inside a macrostep: a span event on the step it happened in. */
static bool tna_span_event(tna_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tna_otlp_open *open = tna_open_for(otlp, record, now);
    if (open == NULL) {
        return false;
    }
    tna_otlp_span *target = open->has_micro && record->key.microstep != 0 && record->key.microstep == open->micro.microstep
                                ? &open->micro
                                : &open->macro;
    otlp->scratch.len = 0;
    if (!tna_record_attributes(otlp, &otlp->scratch, record)) {
        return false;
    }
    char name[40] = "scxml.";
    const char *kind = tna_kind_names[record->kind];
    size_t kind_len = strlen(kind);
    memcpy(name + 6, kind, kind_len);
    ta_str event_name = { name, (uint32_t)(6u + kind_len) };
    size_t body = 9u + tna_len_field_size(F_EVENT_NAME, event_name.len) + otlp->scratch.len;
    if (!tna_buf_room(&target->events, tna_len_field_size(F_SPAN_EVENTS, body))) {
        return false;
    }
    tna_put_len_header(&target->events, F_SPAN_EVENTS, body);
    tna_put_tag(&target->events, F_EVENT_TIME, WIRE_I64);
    tna_put_fixed64(&target->events, now);
    tna_put_len_header(&target->events, F_EVENT_NAME, event_name.len);
    tna_put_bytes(&target->events, event_name.ptr, event_name.len);
    tna_put_bytes(&target->events, otlp->scratch.data, otlp->scratch.len);
    if (record->has_fault || record->kind == TA_TRACE_ACTION_FAILED) {
        target->error = true;
    }
    return true;
}

/* A lifecycle record: a span of no duration. */
static bool tna_lifecycle(tna_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    if (record->kind == TA_TRACE_UNLOADED || record->kind == TA_TRACE_DESTROYED) {
        tna_otlp_open *open = tna_open_find(otlp, record->key.session);
        if (open != NULL && !tna_open_end(otlp, open, now, true)) {
            return false;
        }
    }
    uint32_t wanted = (record->context.detail & TA_TRACE_DETAIL_WATCHED) != 0 ? otlp->config.wanted_watched_kinds
                                                                              : otlp->config.wanted_kinds;
    if ((wanted & TA_TRACE_BIT(record->kind)) == 0) {
        return true;
    }
    tna_otlp_span *span = &otlp->lifecycle;
    tna_span_reset(span);
    bool ok = tna_span_open(otlp, span, record, now) &&
              tna_span_name(span, LIT("scxml."), tna_cstr(tna_kind_names[record->kind])) &&
              tna_cause_attributes(&span->attributes, F_SPAN_ATTRIBUTES, &record->cause) &&
              (record->state == TA_STATE_NONE ||
               tna_kv_string(&span->attributes, F_SPAN_ATTRIBUTES, LIT("scxml.final_state"),
                             tna_state_name(otlp, record->definition, record->state)));
    if (ok) {
        tna_span_parent(otlp, span, &record->cause);
        ok = tna_span_end(otlp, span, now);
    }
    return ok;
}

void tna_otlp_write(tna_otlp *otlp, const ta_trace_record *record) {
    if ((uint32_t)record->kind > (uint32_t)TA_TRACE_IGNORED) {
        otlp->dropped += 1u;
        return;
    }
    /* The clock is read at span boundaries only; span events use that reading. */
    uint64_t now = otlp->last_now;
    if (record->kind <= TA_TRACE_MICROSTEP && record->kind != TA_TRACE_CONDITION) {
        now = otlp->config.host.now(otlp->config.host.self);
        otlp->last_now = now;
    }
    size_t spans_before = otlp->spans.len;
    uint32_t count_before = otlp->span_count;
    bool ok;
    switch (record->kind) {
        case TA_TRACE_SPAWNED:
        case TA_TRACE_LOADED:
        case TA_TRACE_UNLOADED:
        case TA_TRACE_FINISHED:
        case TA_TRACE_DESTROYED:
            ok = tna_lifecycle(otlp, record, now);
            break;
        case TA_TRACE_MACROSTEP_BEGIN:
            ok = tna_begin(otlp, record, now);
            break;
        case TA_TRACE_MACROSTEP_END:
            ok = tna_end(otlp, record, now);
            break;
        case TA_TRACE_MICROSTEP:
            ok = tna_microstep(otlp, record, now);
            break;
        default:
            ok = tna_span_event(otlp, record, now);
            break;
    }
    if (!ok) {
        /* A span half-appended to `spans` would corrupt the request. */
        otlp->spans.len = spans_before;
        otlp->span_count = count_before;
        otlp->dropped += 1u;
    }
}

ta_str tna_otlp_take(tna_otlp *otlp) {
    otlp->request.len = 0;
    if (otlp->span_count == 0) {
        return (ta_str){ NULL, 0 };
    }
    ta_str name = LIT("tinyactors");
    ta_str version = otlp->config.version;
    size_t scope = tna_len_field_size(F_SCOPE_NAME, name.len) +
                   (version.len != 0 ? tna_len_field_size(F_SCOPE_VERSION, version.len) : 0);
    size_t scope_spans = tna_len_field_size(F_SS_SCOPE, scope) + otlp->spans.len;
    size_t resource_spans = tna_len_field_size(F_RS_RESOURCE, otlp->resource.len) +
                            tna_len_field_size(F_RS_SCOPE_SPANS, scope_spans);
    size_t total = tna_len_field_size(F_REQUEST_RESOURCE_SPANS, resource_spans);
    if (!tna_buf_room(&otlp->request, total)) {
        /* The spans stay, and the next take tries again. */
        return (ta_str){ NULL, 0 };
    }
    tna_otlp_buf *out = &otlp->request;
    tna_put_len_header(out, F_REQUEST_RESOURCE_SPANS, resource_spans);
    tna_put_len_header(out, F_RS_RESOURCE, otlp->resource.len);
    tna_put_bytes(out, otlp->resource.data, otlp->resource.len);
    tna_put_len_header(out, F_RS_SCOPE_SPANS, scope_spans);
    tna_put_len_header(out, F_SS_SCOPE, scope);
    tna_put_len_header(out, F_SCOPE_NAME, name.len);
    tna_put_bytes(out, name.ptr, name.len);
    if (version.len != 0) {
        tna_put_len_header(out, F_SCOPE_VERSION, version.len);
        tna_put_bytes(out, version.ptr, version.len);
    }
    tna_put_bytes(out, otlp->spans.data, otlp->spans.len);
    otlp->spans.len = 0;
    otlp->span_count = 0;
    return (ta_str){ (const char *)out->data, (uint32_t)out->len };
}

void tna_otlp_end_all(tna_otlp *otlp) {
    uint64_t now = otlp->config.host.now(otlp->config.host.self);
    while (otlp->open_count != 0) {
        if (!tna_open_end(otlp, &otlp->open[otlp->open_count - 1u], now, true)) {
            otlp->dropped += 1u;
        }
    }
}

void tna_otlp_destroy(tna_otlp *otlp) {
    ta_allocator allocator = otlp->config.allocator;
    for (uint32_t at = 0; at < otlp->open_capacity; at += 1u) {
        tna_span_free(&otlp->open[at].macro);
        tna_span_free(&otlp->open[at].micro);
    }
    if (otlp->open != NULL) {
        allocator.v->free(allocator.self,
                          &(ta_allocator_free_params){ .ptr = otlp->open,
                                                       .size = (size_t)otlp->open_capacity * sizeof *otlp->open,
                                                       .alignment = _Alignof(tna_otlp_open) });
    }
    if (otlp->name_slots != NULL) {
        allocator.v->free(allocator.self,
                          &(ta_allocator_free_params){ .ptr = otlp->name_slots,
                                                       .size = (size_t)otlp->name_capacity * sizeof *otlp->name_slots,
                                                       .alignment = _Alignof(tna_otlp_name) });
    }
    tna_open_index_free(otlp);
    tna_span_free(&otlp->lifecycle);
    tna_buf_free(&otlp->names);
    tna_buf_free(&otlp->resource);
    tna_buf_free(&otlp->spans);
    tna_buf_free(&otlp->request);
    tna_buf_free(&otlp->scratch);
    tna_buf_free(&otlp->label);
    tna_buf_free(&otlp->text);
    *otlp = (tna_otlp){ 0 };
}

#include "tinyactors_host/otlp.h"

#include "memory.h"
#include "otlp_private.h"


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
    F_SCOPE_ATTRIBUTES = 3,
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
    F_ANY_BYTES = 7,
    /* ArrayValue */
    F_ARRAY_VALUES = 1,
    /* ExportLogsServiceRequest, ResourceLogs, ScopeLogs */
    F_REQUEST_RESOURCE_LOGS = 1,
    F_RL_RESOURCE = 1,
    F_RL_SCOPE_LOGS = 2,
    F_SL_SCOPE = 1,
    F_SL_LOG_RECORDS = 2,
    /* LogRecord */
    F_LOG_TIME = 1,
    F_LOG_SEVERITY_NUMBER = 2,
    F_LOG_SEVERITY_TEXT = 3,
    F_LOG_BODY = 5,
    F_LOG_ATTRIBUTES = 6,
    F_LOG_FLAGS = 8,
    F_LOG_TRACE_ID = 9,
    F_LOG_SPAN_ID = 10,
    F_LOG_OBSERVED_TIME = 11,
    F_LOG_EVENT_NAME = 12,
};

enum { SEVERITY_DEBUG = 5, SEVERITY_INFO = 9, SEVERITY_WARN = 13 };

enum { WIRE_VARINT = 0, WIRE_I64 = 1, WIRE_LEN = 2 };
enum { SPAN_KIND_INTERNAL = 1, STATUS_CODE_ERROR = 2 };

#define LIT(text) ((ta_str){ (text), (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * Bytes
 */

static void tah_buf_init(tah_otlp_buf *buf, ta_allocator allocator) {
    *buf = (tah_otlp_buf){ .allocator = allocator };
}

static void tah_buf_free(tah_otlp_buf *buf) {
    if (buf->data != NULL) {
        buf->allocator.v->free(buf->allocator.self,
                               &(ta_allocator_free_params){ .ptr = buf->data, .size = buf->cap, .alignment = 1 });
    }
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/*
 * The staging bound (traceStagingBytes, napi ADRs 0035 and 0036). The span
 * staging is reserved whole when the sink is installed; a turn stops early
 * once it has staged that much (tah_system_turn), and a call outside a turn
 * hands the sink a request once it has (tna_trace_flush_if_full). A
 * macrostep is never split, so a burst can grow the buffer past it for one
 * request; the buffer shrinks back to the bound when that request is taken.
 * The bound is therefore the bound plus one call's (or one macrostep's)
 * records, never checked per record: a per-record check would call the
 * sink from inside core (napi ADR 0036, follow-up of 2026-09-30).
 */
static size_t tah_keep_bytes(const tah_otlp *otlp) {
    return otlp->config.keep_bytes != 0 ? otlp->config.keep_bytes : (size_t)1 << 20;
}

/* An empty buffer beyond the kept bytes, and four times a typical turn's use, is freed. */
static void tah_buf_settle(tah_otlp_buf *buf, size_t used, size_t keep) {
    if (buf->len == 0 && buf->cap > keep && buf->cap / 4u > used) {
        tah_buf_free(buf);
    }
}

static bool tah_buf_room(tah_otlp_buf *buf, size_t more);

/* The span staging after a request was taken: back to exactly the bound
 * when a burst grew it past four times a typical turn's use; a steady load
 * that runs past the bound keeps its buffer (a failed reallocation keeps it
 * empty, and the next span regrows it as before). */
static void tah_spans_settle(tah_otlp_buf *buf, size_t used, size_t keep) {
    if (buf->len == 0 && buf->cap > keep && buf->cap / 4u > used) {
        tah_buf_free(buf);
        (void)tah_buf_room(buf, keep);
    }
}

/* Room for `more` bytes past the end; false when the allocator refuses. */
static bool tah_buf_room(tah_otlp_buf *buf, size_t more) {
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
        tah_mem_copy(data, buf->data, len);
    }
    tah_buf_free(buf);
    buf->data = data;
    buf->len = len;
    buf->cap = cap;
    return true;
}

static size_t tah_varint_size(uint64_t value) {
    size_t size = 1;
    while (value >= 0x80u) {
        value >>= 7;
        size += 1u;
    }
    return size;
}

/* The writers below assume the room was made first. */
static void tah_put_varint(tah_otlp_buf *buf, uint64_t value) {
    while (value >= 0x80u) {
        buf->data[buf->len++] = (uint8_t)(value | 0x80u);
        value >>= 7;
    }
    buf->data[buf->len++] = (uint8_t)value;
}

static void tah_put_tag(tah_otlp_buf *buf, uint32_t field, uint32_t wire) {
    tah_put_varint(buf, ((uint64_t)field << 3) | wire);
}

/*
 * Most copies here are short: keys, names, small values. A copy of up to 16
 * bytes is two overlapping fixed-size moves, which the compiler inlines; a
 * call to memcpy through the dynamic-library stub costs more than the copy
 * (napi ADR 0017 section 4).
 */
static void tah_put_bytes(tah_otlp_buf *buf, const void *bytes, size_t len) {
    uint8_t *to = buf->data + buf->len;
    const uint8_t *from = bytes;
    if (len >= 8u && len <= 16u) {
        uint64_t head;
        uint64_t tail;
        tah_mem_copy(&head, from, 8);
        tah_mem_copy(&tail, from + len - 8u, 8);
        tah_mem_copy(to, &head, 8);
        tah_mem_copy(to + len - 8u, &tail, 8);
    } else if (len >= 4u && len < 8u) {
        uint32_t head;
        uint32_t tail;
        tah_mem_copy(&head, from, 4);
        tah_mem_copy(&tail, from + len - 4u, 4);
        tah_mem_copy(to, &head, 4);
        tah_mem_copy(to + len - 4u, &tail, 4);
    } else if (len < 4u) {
        for (size_t at = 0; at < len; at += 1u) {
            to[at] = from[at];
        }
    } else {
        tah_mem_copy(to, from, len);
    }
    buf->len += len;
}

static void tah_put_fixed64(tah_otlp_buf *buf, uint64_t value) {
    for (int at = 0; at < 8; at += 1) {
        buf->data[buf->len++] = (uint8_t)(value >> (8 * at));
    }
}

/* Span and trace IDs are big-endian bytes: their hex form reads the same. */
static void tah_put_id64(tah_otlp_buf *buf, uint64_t value) {
    for (int at = 7; at >= 0; at -= 1) {
        buf->data[buf->len++] = (uint8_t)(value >> (8 * at));
    }
}

/* The size of a length-delimited field whose body is `len` bytes. */
static size_t tah_len_field_size(uint32_t field, size_t len) {
    return tah_varint_size((uint64_t)field << 3) + tah_varint_size(len) + len;
}

static void tah_put_len_header(tah_otlp_buf *buf, uint32_t field, size_t len) {
    tah_put_tag(buf, field, WIRE_LEN);
    tah_put_varint(buf, len);
}

/* ------------------------------------------------------------------------
 * Attributes: KeyValue { key, AnyValue }
 */

/* AnyValue sizes. */
static size_t tah_any_string_size(size_t len) { return tah_len_field_size(F_ANY_STRING, len); }
static size_t tah_any_int_size(int64_t value) { return 1u + tah_varint_size((uint64_t)value); }

static size_t tah_kv_size(ta_str key, size_t any) {
    return tah_len_field_size(F_KV_KEY, key.len) + tah_len_field_size(F_KV_VALUE, any);
}

static void tah_put_kv_head(tah_otlp_buf *buf, uint32_t field, ta_str key, size_t any) {
    tah_put_len_header(buf, field, tah_kv_size(key, any));
    tah_put_len_header(buf, F_KV_KEY, key.len);
    tah_put_bytes(buf, key.ptr, key.len);
    tah_put_len_header(buf, F_KV_VALUE, any);
}

static bool tah_kv_string(tah_otlp_buf *buf, uint32_t field, ta_str key, ta_str value) {
    size_t any = tah_any_string_size(value.len);
    if (!tah_buf_room(buf, tah_len_field_size(field, tah_kv_size(key, any)))) {
        return false;
    }
    tah_put_kv_head(buf, field, key, any);
    tah_put_len_header(buf, F_ANY_STRING, value.len);
    tah_put_bytes(buf, value.ptr, value.len);
    return true;
}

static bool tah_kv_bytes(tah_otlp_buf *buf, uint32_t field, ta_str key, ta_bytes value) {
    size_t any = tah_len_field_size(F_ANY_BYTES, value.len);
    if (!tah_buf_room(buf, tah_len_field_size(field, tah_kv_size(key, any)))) {
        return false;
    }
    tah_put_kv_head(buf, field, key, any);
    tah_put_len_header(buf, F_ANY_BYTES, value.len);
    tah_put_bytes(buf, value.ptr, value.len);
    return true;
}

static bool tah_kv_int(tah_otlp_buf *buf, uint32_t field, ta_str key, int64_t value) {
    size_t any = tah_any_int_size(value);
    if (!tah_buf_room(buf, tah_len_field_size(field, tah_kv_size(key, any)))) {
        return false;
    }
    tah_put_kv_head(buf, field, key, any);
    tah_put_tag(buf, F_ANY_INT, WIRE_VARINT);
    tah_put_varint(buf, (uint64_t)value);
    return true;
}

static bool tah_kv_bool(tah_otlp_buf *buf, uint32_t field, ta_str key, bool value) {
    size_t any = 2u;
    if (!tah_buf_room(buf, tah_len_field_size(field, tah_kv_size(key, any)))) {
        return false;
    }
    tah_put_kv_head(buf, field, key, any);
    tah_put_tag(buf, F_ANY_BOOL, WIRE_VARINT);
    tah_put_varint(buf, value ? 1u : 0u);
    return true;
}

static bool tah_kv_int_array(tah_otlp_buf *buf, uint32_t field, ta_str key, const uint32_t *values, uint32_t count) {
    size_t array = 0;
    for (uint32_t at = 0; at < count; at += 1u) {
        array += tah_len_field_size(F_ARRAY_VALUES, tah_any_int_size(values[at]));
    }
    size_t any = tah_len_field_size(F_ANY_ARRAY, array);
    if (!tah_buf_room(buf, tah_len_field_size(field, tah_kv_size(key, any)))) {
        return false;
    }
    tah_put_kv_head(buf, field, key, any);
    tah_put_len_header(buf, F_ANY_ARRAY, array);
    for (uint32_t at = 0; at < count; at += 1u) {
        tah_put_len_header(buf, F_ARRAY_VALUES, tah_any_int_size(values[at]));
        tah_put_tag(buf, F_ANY_INT, WIRE_VARINT);
        tah_put_varint(buf, values[at]);
    }
    return true;
}

/* ------------------------------------------------------------------------
 * The cache: a definition's facts, and event names for host logs
 *
 * A trace record names everything it refers to (core ADR 0098), so the
 * encoder looks nothing up in core for a record. What it keeps is what only
 * some records carry: a definition's family, revision, document name and
 * source, which come on the records that open a run and name the scope of
 * every span of that definition. Event names a host log names (not a
 * record) are formatted by core once and cached under definition 0, which
 * core never frees, since it never frees an interned event name.
 */
enum { NAME_FACT = 0u, NAME_EVENT = 3u };
enum { FACT_FAMILY = 0u, FACT_REVISION = 1u, FACT_DOCUMENT = 2u, FACT_SOURCE = 3u };
#define NAME_TAG(kind, id) (((uint32_t)(kind) << 30) | ((uint32_t)(id) & 0x3fffffffu))

static uint64_t tah_name_hash(uint64_t definition, uint32_t tag) {
    uint64_t value = definition * 0x9e3779b97f4a7c15ull ^ tag;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    return value;
}

static tah_otlp_name *tah_name_slot(const tah_otlp *otlp, uint64_t definition, uint32_t tag) {
    uint32_t mask = otlp->name_capacity - 1u;
    uint32_t at = (uint32_t)tah_name_hash(definition, tag) & mask;
    for (;;) {
        tah_otlp_name *slot = &otlp->name_slots[at];
        if (!slot->used || (slot->definition == definition && slot->tag == tag)) {
            return slot;
        }
        at = (at + 1u) & mask;
    }
}

static bool tah_names_grow(tah_otlp *otlp) {
    uint32_t capacity = otlp->name_capacity == 0 ? 64u : otlp->name_capacity * 2u;
    ta_allocator allocator = otlp->config.allocator;
    tah_otlp_name *slots = allocator.v->alloc(allocator.self, (size_t)capacity * sizeof *slots, _Alignof(tah_otlp_name));
    if (slots == NULL) {
        return false;
    }
    tah_mem_set(slots, 0u, (size_t)capacity * sizeof *slots);
    tah_otlp_name *old = otlp->name_slots;
    uint32_t old_capacity = otlp->name_capacity;
    otlp->name_slots = slots;
    otlp->name_capacity = capacity;
    for (uint32_t at = 0; at < old_capacity; at += 1u) {
        if (old[at].used) {
            *tah_name_slot(otlp, old[at].definition, old[at].tag) = old[at];
        }
    }
    if (old != NULL) {
        allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = old,
                                                                        .size = (size_t)old_capacity * sizeof *old,
                                                                        .alignment = _Alignof(tah_otlp_name) });
    }
    return true;
}

static ta_str tah_name_text(const tah_otlp *otlp, const tah_otlp_name *slot) {
    return (ta_str){ (const char *)otlp->names.data + slot->at, slot->len };
}

/*
 * Rebuilds the table in place after entries were dropped: the labels still
 * used move to the front of `names`, and every entry is reinserted, so no
 * probe run has a hole in it.
 */
static void tah_names_rebuild(tah_otlp *otlp) {
    uint32_t capacity = otlp->name_capacity;
    size_t text = 0;
    for (uint32_t at = 0; at < capacity; at += 1u) {
        if (otlp->name_slots[at].used) {
            text += otlp->name_slots[at].len;
        }
    }
    /* The labels still used, copied to a fresh buffer; without memory for
     * it, their text stays where it is and only the slots are reclaimed. */
    tah_otlp_buf names;
    tah_buf_init(&names, otlp->config.allocator);
    bool compact = text == 0 || tah_buf_room(&names, text);
    for (uint32_t at = 0; compact && at < capacity; at += 1u) {
        tah_otlp_name *slot = &otlp->name_slots[at];
        if (slot->used) {
            size_t from = slot->at;
            slot->at = names.len;
            tah_put_bytes(&names, otlp->names.data + from, slot->len);
        }
    }
    if (compact) {
        tah_buf_free(&otlp->names);
        otlp->names = names;
    }
    /* Reinsert what is left, so no probe run has a hole in it. */
    for (uint32_t at = 0; at < capacity; at += 1u) {
        tah_otlp_name entry = otlp->name_slots[at];
        if (entry.used) {
            otlp->name_slots[at].used = false;
            *tah_name_slot(otlp, entry.definition, entry.tag) = entry;
        }
    }
}

void tah_otlp_forget_definition(tah_otlp *otlp, ta_definition_id definition) {
    uint32_t dropped = 0;
    for (uint32_t at = 0; at < otlp->name_capacity; at += 1u) {
        tah_otlp_name *slot = &otlp->name_slots[at];
        if (slot->used && slot->definition == definition) {
            slot->used = false;
            dropped += 1u;
        }
    }
    if (dropped != 0) {
        otlp->name_count -= dropped;
        tah_names_rebuild(otlp);
    }
}

/* Caches `otlp->label` under (definition, tag); the label as cached, or empty. */
static ta_str tah_name_keep(tah_otlp *otlp, uint64_t definition, uint32_t tag) {
    ta_str label = { (const char *)otlp->label.data, (uint32_t)otlp->label.len };
    if ((otlp->name_count + 1u) * 10u > otlp->name_capacity * 7u && !tah_names_grow(otlp)) {
        return label;
    }
    if (!tah_buf_room(&otlp->names, label.len)) {
        return label;
    }
    tah_otlp_name *slot = tah_name_slot(otlp, definition, tag);
    *slot = (tah_otlp_name){ .definition = definition, .tag = tag, .len = label.len, .at = otlp->names.len,
                             .used = true };
    tah_put_bytes(&otlp->names, label.ptr, label.len);
    otlp->name_count += 1u;
    return tah_name_text(otlp, slot);
}

static const tah_otlp_name *tah_name_find(const tah_otlp *otlp, uint64_t definition, uint32_t tag) {
    if (otlp->name_capacity == 0) {
        return NULL;
    }
    const tah_otlp_name *slot = tah_name_slot(otlp, definition, tag);
    return slot->used ? slot : NULL;
}

static bool tah_label_add(tah_otlp *otlp, ta_str text) {
    if (!tah_buf_room(&otlp->label, text.len)) {
        return false;
    }
    tah_put_bytes(&otlp->label, text.ptr, text.len);
    return true;
}

/* Caches a definition's facts from a record that carries them, once. */
static void tah_facts_keep(tah_otlp *otlp, const ta_trace_record *record) {
    ta_str family = ta_trace_record_text(record, TA_TRACE_TEXT_FAMILY);
    if (family.len == 0 || tah_name_find(otlp, record->definition, NAME_TAG(NAME_FACT, FACT_FAMILY)) != NULL) {
        return;
    }
    const ta_str facts[] = {
        [FACT_FAMILY] = family,
        [FACT_REVISION] = { (const char *)record->revision.ptr, record->revision.len },
        [FACT_DOCUMENT] = ta_trace_record_text(record, TA_TRACE_TEXT_DOCUMENT_NAME),
        [FACT_SOURCE] = ta_trace_record_text(record, TA_TRACE_TEXT_SOURCE),
    };
    /* The family last: it says the others are there. */
    for (uint32_t fact = 4u; fact > 0; fact -= 1u) {
        if (tah_name_find(otlp, record->definition, NAME_TAG(NAME_FACT, fact - 1u)) != NULL) {
            continue; /* kept by a record whose family did not fit */
        }
        otlp->label.len = 0;
        if (!tah_label_add(otlp, facts[fact - 1u])) {
            return;
        }
        (void)tah_name_keep(otlp, record->definition, NAME_TAG(NAME_FACT, fact - 1u));
    }
}

/* A cached fact of a definition; false when the encoder never saw it. */
static bool tah_fact(const tah_otlp *otlp, uint64_t definition, uint32_t fact, ta_str *out) {
    const tah_otlp_name *cached = tah_name_find(otlp, definition, NAME_TAG(NAME_FACT, fact));
    if (cached == NULL || tah_name_find(otlp, definition, NAME_TAG(NAME_FACT, FACT_FAMILY)) == NULL) {
        return false;
    }
    *out = tah_name_text(otlp, cached);
    return true;
}

/* ------------------------------------------------------------------------
 * Text core formats for a host log: event names
 */

/* An event name's dotted text, formatted once and then cached. The text is
 * valid until the next name is cached: use it before naming anything else. */
static ta_str tah_event_name(tah_otlp *otlp, ta_event_name_id name) {
    uint32_t tag = NAME_TAG(NAME_EVENT, name);
    const tah_otlp_name *cached = tah_name_find(otlp, 0, tag);
    if (cached != NULL) {
        return tah_name_text(otlp, cached);
    }
    uint32_t needed = 0;
    ta_event_name_format_params params = { .name = name, .buffer = (char *)otlp->text.data,
                                           .capacity = (uint32_t)otlp->text.cap, .needed = &needed };
    ta_result result = ta_event_name_format(otlp->config.core, &params);
    if (result == TA_ERR_NOMEM && needed > otlp->text.cap) {
        otlp->text.len = 0;
        if (!tah_buf_room(&otlp->text, needed)) {
            return LIT("?");
        }
        params.buffer = (char *)otlp->text.data;
        params.capacity = (uint32_t)otlp->text.cap;
        result = ta_event_name_format(otlp->config.core, &params);
    }
    if (result != TA_OK) {
        return LIT("?");
    }
    ta_str text = { (const char *)otlp->text.data, needed > 0 ? needed - 1u : 0u };
    otlp->label.len = 0;
    return tah_label_add(otlp, text) ? tah_name_keep(otlp, 0, tag) : text;
}

/* Decimal text for a u64 into `out` (at least 21 bytes). */
static ta_str tah_decimal(uint64_t value, char *out) {
    char digits[24];
    int at = (int)sizeof digits;
    do {
        digits[--at] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    size_t len = sizeof digits - (size_t)at;
    tah_mem_copy(out, digits + at, len);
    return (ta_str){ out, (uint32_t)len };
}

/* A session ID as text (core ADR 0088, napi ADR 0032 section 1),
 * `<incarnation>.<session>.<family>`, into `out` (TAH_SESSION_TEXT bytes):
 * the text `_sessionid` holds. The decimal session alone when the family is
 * unknown, which only a host's cause or a definition record has, or when
 * the text does not fit. */
#define TAH_SESSION_TEXT 256u
static ta_str tah_session_text(const tah_otlp *otlp, uint64_t session, ta_str family, char *out) {
    uint32_t needed = 0;
    if (family.len != 0 && otlp->config.core != NULL &&
        ta_session_id_format(otlp->config.core,
                             &(ta_session_id_format_params){ .session = session, .family = family, .buffer = out,
                                                             .capacity = TAH_SESSION_TEXT, .needed = &needed }) ==
            TA_OK &&
        needed != 0) {
        return (ta_str){ out, needed - 1u };
    }
    return tah_decimal(session, out);
}

/* The session of a step key as text: a key names its session's family (core ADR 0088). */
static ta_str tah_key_session(const tah_otlp *otlp, const ta_step_key *key, char *out) {
    ta_str family = { 0 };
    if (key->family != 0 && otlp->config.core != NULL &&
        ta_system_atom_text(otlp->config.core, key->family, &family) != TA_OK) {
        family = (ta_str){ 0 };
    }
    return tah_session_text(otlp, key->session, family, out);
}

/* ------------------------------------------------------------------------
 * The encoder
 */

static const char *const tah_kind_names[] = {
    "spawned",   "loaded",    "unloaded", "finished", "destroyed", "macrostep_begin", "macrostep_end", "condition",
    "microstep", "raise",     "data",     "assign",   "script",    "foreach",         "error",         "send",
    "cancel",    "invoke",    "invoke.cancel", "paused", "resumed", "stopped",  "ignored", "forward",
    "macrostep.long", "runaway", "definition.registered", "definition.retired", "definition.freed",
    "abandoned", "withdrawn",
};
_Static_assert(sizeof tah_kind_names / sizeof tah_kind_names[0] == (size_t)TA_TRACE_WITHDRAWN + 1u,
               "one name per ta_trace_kind");

static const char *const tah_at_names[] = { "none", "select", "exit", "transition", "enter", "bootstrap", "finalize",
                                            "invoke" };
static const char *const tah_event_types[] = { "external", "internal", "platform" };

static ta_str tah_cstr(const char *text) { return (ta_str){ text, (uint32_t)tah_str_length(text) }; }

static void tah_span_init(tah_otlp_span *span, ta_allocator allocator);

bool tah_otlp_init(tah_otlp *otlp, const tah_otlp_config *config) {
    *otlp = (tah_otlp){ .config = *config };
    ta_allocator allocator = config->allocator;
    tah_buf_init(&otlp->names, allocator);
    tah_buf_init(&otlp->resource, allocator);
    tah_buf_init(&otlp->spans, allocator);
    tah_buf_init(&otlp->scopes, allocator);
    tah_buf_init(&otlp->scratch, allocator);
    tah_buf_init(&otlp->label, allocator);
    tah_buf_init(&otlp->text, allocator);
    tah_span_init(&otlp->lifecycle, allocator);
    tah_buf_init(&otlp->logs, allocator);
    tah_buf_init(&otlp->log_body, allocator);
    return tah_buf_room(&otlp->spans, tah_keep_bytes(otlp));
}

ta_result tah_otlp_set_resource(tah_otlp *otlp, const tah_otlp_attribute *attributes, uint32_t count) {
    otlp->resource.len = 0;
    for (uint32_t at = 0; at < count; at += 1u) {
        if (!tah_kv_string(&otlp->resource, F_RESOURCE_ATTRIBUTES, attributes[at].key, attributes[at].value)) {
            otlp->resource.len = 0;
            return TA_ERR_NOMEM;
        }
    }
    return TA_OK;
}

static void tah_span_init(tah_otlp_span *span, ta_allocator allocator) {
    *span = (tah_otlp_span){ 0 };
    tah_buf_init(&span->name, allocator);
    tah_buf_init(&span->attributes, allocator);
    tah_buf_init(&span->events, allocator);
}

static void tah_span_free(tah_otlp_span *span) {
    tah_buf_free(&span->name);
    tah_buf_free(&span->attributes);
    tah_buf_free(&span->events);
}

/* Empties a span for reuse, keeping its buffers' capacity. */
static void tah_span_reset(tah_otlp_span *span) {
    tah_otlp_buf name = span->name, attributes = span->attributes, events = span->events;
    *span = (tah_otlp_span){ 0 };
    span->name = name;
    span->attributes = attributes;
    span->events = events;
    span->name.len = 0;
    span->attributes.len = 0;
    span->events.len = 0;
}

static uint32_t tah_open_hash(uint64_t session) {
    session ^= session >> 33;
    session *= 0xff51afd7ed558ccdull;
    session ^= session >> 33;
    return (uint32_t)session;
}

/* The index slot holding `session`, or the empty slot where it would go. */
static uint32_t tah_open_slot(const tah_otlp *otlp, uint64_t session) {
    uint32_t mask = otlp->open_index_capacity - 1u;
    uint32_t at = tah_open_hash(session) & mask;
    while (otlp->open_index[at] != 0 && otlp->open[otlp->open_index[at] - 1u].session != session) {
        at = (at + 1u) & mask;
    }
    return at;
}

static tah_otlp_open *tah_open_find(tah_otlp *otlp, uint64_t session) {
    if (otlp->open_count == 0) {
        return NULL;
    }
    uint32_t entry = otlp->open_index[tah_open_slot(otlp, session)];
    return entry == 0 ? NULL : &otlp->open[entry - 1u];
}

static void tah_open_index_free(tah_otlp *otlp) {
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
static bool tah_open_index_build(tah_otlp *otlp, uint32_t capacity) {
    ta_allocator allocator = otlp->config.allocator;
    uint32_t *index = allocator.v->alloc(allocator.self, (size_t)capacity * sizeof(uint32_t), _Alignof(uint32_t));
    if (index == NULL) {
        return false;
    }
    tah_mem_set(index, 0u, (size_t)capacity * sizeof(uint32_t));
    tah_open_index_free(otlp);
    otlp->open_index = index;
    otlp->open_index_capacity = capacity;
    for (uint32_t at = 0; at < otlp->open_count; at += 1u) {
        otlp->open_index[tah_open_slot(otlp, otlp->open[at].session)] = at + 1u;
    }
    return true;
}

/* Removes `session` from the index, shifting the probe run back over the gap. */
static void tah_open_unindex(tah_otlp *otlp, uint64_t session) {
    uint32_t mask = otlp->open_index_capacity - 1u;
    uint32_t hole = tah_open_slot(otlp, session);
    otlp->open_index[hole] = 0;
    for (uint32_t at = (hole + 1u) & mask; otlp->open_index[at] != 0; at = (at + 1u) & mask) {
        uint32_t home = tah_open_hash(otlp->open[otlp->open_index[at] - 1u].session) & mask;
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
static tah_otlp_open *tah_open_add(tah_otlp *otlp, uint64_t session) {
    if (otlp->open_count == otlp->open_capacity) {
        uint32_t capacity = otlp->open_capacity == 0 ? 8u : otlp->open_capacity * 2u;
        ta_allocator allocator = otlp->config.allocator;
        tah_otlp_open *open = allocator.v->alloc(allocator.self, (size_t)capacity * sizeof *open,
                                                 _Alignof(tah_otlp_open));
        if (open == NULL) {
            return NULL;
        }
        /* The spares past `open_count` own buffers too: move them all. */
        if (otlp->open_capacity != 0) {
            tah_mem_copy(open, otlp->open, (size_t)otlp->open_capacity * sizeof *open);
        }
        for (uint32_t at = otlp->open_capacity; at < capacity; at += 1u) {
            open[at].session = 0;
            tah_span_init(&open[at].macro, allocator);
            tah_span_init(&open[at].micro, allocator);
        }
        if (otlp->open != NULL) {
            allocator.v->free(allocator.self,
                              &(ta_allocator_free_params){ .ptr = otlp->open,
                                                           .size = (size_t)otlp->open_capacity * sizeof *otlp->open,
                                                           .alignment = _Alignof(tah_otlp_open) });
        }
        otlp->open = open;
        otlp->open_capacity = capacity;
    }
    if (otlp->open_index_capacity < otlp->open_capacity * 2u &&
        !tah_open_index_build(otlp, otlp->open_capacity * 2u)) {
        return NULL;
    }
    tah_otlp_open *entry = &otlp->open[otlp->open_count++];
    entry->session = session;
    entry->has_micro = false;
    tah_span_reset(&entry->macro);
    tah_span_reset(&entry->micro);
    otlp->open_index[tah_open_slot(otlp, session)] = otlp->open_count;
    return entry;
}

/* Removes an open entry, keeping its buffers as the spare past the end. */
static void tah_open_remove(tah_otlp *otlp, tah_otlp_open *entry) {
    tah_open_unindex(otlp, entry->session);
    tah_otlp_open *last = &otlp->open[otlp->open_count - 1u];
    if (entry != last) {
        uint32_t moved = tah_open_slot(otlp, last->session);
        tah_otlp_open spare = *entry;
        *entry = *last;
        *last = spare;
        otlp->open_index[moved] = (uint32_t)(entry - otlp->open) + 1u;
    }
    otlp->open_count -= 1u;
}

static bool tah_context_empty(const ta_trace_context *context) {
    return context->trace_id_high == 0 && context->trace_id_low == 0;
}

static void tah_trace_id_of(tah_otlp *otlp, const ta_trace_context *context, uint8_t out[16]) {
    if (tah_context_empty(context)) {
        otlp->config.host.trace_id(otlp->config.host.self, out);
        return;
    }
    for (int at = 0; at < 8; at += 1) {
        out[at] = (uint8_t)(context->trace_id_high >> (56 - 8 * at));
        out[8 + at] = (uint8_t)(context->trace_id_low >> (56 - 8 * at));
    }
}

static bool tah_key_empty(const ta_step_key *key) {
    return key->session == 0 && key->macrostep == 0 && key->microstep == 0;
}

/* ------------------------------------------------------------------------
 * One ScopeSpans per definition (napi ADR 0028)
 */

/* Grows an array of `size`-byte elements to hold `need`; false when the allocator refuses. */
static bool tah_array_room(ta_allocator allocator, void **items, uint32_t *capacity, uint32_t count, size_t size,
                           size_t alignment, uint32_t need) {
    if (need <= *capacity) {
        return true;
    }
    uint32_t grown = *capacity == 0 ? 16u : *capacity;
    while (grown < need) {
        grown *= 2u;
    }
    void *moved = allocator.v->alloc(allocator.self, (size_t)grown * size, alignment);
    if (moved == NULL) {
        return false;
    }
    if (count != 0) {
        tah_mem_copy(moved, *items, (size_t)count * size);
    }
    if (*items != NULL) {
        allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = *items,
                                                                     .size = (size_t)*capacity * size,
                                                                     .alignment = alignment });
    }
    *items = moved;
    *capacity = grown;
    return true;
}

static void tah_array_free(ta_allocator allocator, void **items, uint32_t *capacity, size_t size, size_t alignment) {
    if (*items != NULL) {
        allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = *items,
                                                                     .size = (size_t)*capacity * size,
                                                                     .alignment = alignment });
    }
    *items = NULL;
    *capacity = 0;
}

static uint32_t tah_run_hash(uint64_t definition) {
    definition ^= definition >> 33;
    definition *= 0xc4ceb9fe1a85ec53ull;
    definition ^= definition >> 33;
    return (uint32_t)definition;
}

/* The index slot of `definition`: its run's, or the empty one it would take. */
static uint32_t *tah_run_slot(const tah_otlp *otlp, uint64_t definition) {
    uint32_t mask = otlp->run_index_capacity - 1u;
    for (uint32_t at = tah_run_hash(definition) & mask;; at = (at + 1u) & mask) {
        uint32_t *slot = &otlp->run_index[at];
        if (*slot == 0 || otlp->runs[*slot - 1u].definition == definition) {
            return slot;
        }
    }
}

/* Indexes the first `otlp->run_count` runs again, into `capacity` slots. */
static bool tah_run_index_build(tah_otlp *otlp, uint32_t capacity) {
    ta_allocator allocator = otlp->config.allocator;
    if (capacity != otlp->run_index_capacity) {
        uint32_t *index = allocator.v->alloc(allocator.self, (size_t)capacity * sizeof *index, _Alignof(uint32_t));
        if (index == NULL) {
            return false;
        }
        tah_array_free(allocator, (void **)&otlp->run_index, &otlp->run_index_capacity, sizeof *otlp->run_index,
                       _Alignof(uint32_t));
        otlp->run_index = index;
        otlp->run_index_capacity = capacity;
    }
    tah_mem_set(otlp->run_index, 0u, (size_t)capacity * sizeof *otlp->run_index);
    for (uint32_t at = 0; at < otlp->run_count; at += 1u) {
        *tah_run_slot(otlp, otlp->runs[at].definition) = at + 1u;
    }
    return true;
}

/* The ResourceSpans.scope_spans field a run writes. */
static size_t tah_run_field_size(const tah_otlp_run *run) {
    return tah_len_field_size(F_RS_SCOPE_SPANS, tah_len_field_size(F_SS_SCOPE, run->scope_len) + run->spans);
}

/* Whether `bytes` are UTF-8: no overlong forms, surrogates or code points past U+10FFFF. */
static bool tah_valid_utf8(const uint8_t *bytes, uint32_t len) {
    for (uint32_t at = 0; at < len;) {
        uint8_t lead = bytes[at];
        uint32_t more;
        uint32_t least;
        if (lead < 0x80u) {
            at += 1u;
            continue;
        } else if ((lead & 0xe0u) == 0xc0u) {
            more = 1u;
            least = 0x80u;
        } else if ((lead & 0xf0u) == 0xe0u) {
            more = 2u;
            least = 0x800u;
        } else if ((lead & 0xf8u) == 0xf0u) {
            more = 3u;
            least = 0x10000u;
        } else {
            return false;
        }
        if (len - at <= more) {
            return false;
        }
        uint32_t code = lead & (0x7fu >> (more + 1u));
        for (uint32_t next = 1; next <= more; next += 1u) {
            if ((bytes[at + next] & 0xc0u) != 0x80u) {
                return false;
            }
            code = (code << 6) | (bytes[at + next] & 0x3fu);
        }
        if (code < least || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) {
            return false;
        }
        at += more + 1u;
    }
    return true;
}

/* A definition's name and the source its first state was written in, empty when it has none. */
static void tah_definition_facts(const tah_otlp *otlp, ta_definition_id definition, ta_str *name, ta_str *source) {
    *name = (ta_str){ 0 };
    *source = (ta_str){ 0 };
    ta_definition_inspection view;
    if (ta_definition_inspect(otlp->config.core, definition, &view) == TA_OK) {
        *name = view.document_name;
    }
    ta_source_location place;
    if (ta_definition_span(otlp->config.core, &(ta_definition_span_params){ .definition = definition,
                                                                         .kind = TA_ELEMENT_STATE,
                                                                         .id = 0,
                                                                         .out = &place }) == TA_OK) {
        *source = place.source;
    }
}

/*
 * Encodes a run's InstrumentationScope into `scopes`: the name and version
 * every scope carries, and the definition's identity as attributes, from the
 * facts the records that open a run carry. A definition whose facts the
 * encoder never saw is named by its ID alone.
 */
static bool tah_scope_encode(tah_otlp *otlp, tah_otlp_run *run) {
    tah_otlp_buf *out = &otlp->scopes;
    run->scope_at = out->len;
    ta_str name = LIT("tinyactors");
    ta_str version = otlp->config.version;
    bool ok = tah_buf_room(out, tah_len_field_size(F_SCOPE_NAME, name.len) +
                                    (version.len != 0 ? tah_len_field_size(F_SCOPE_VERSION, version.len) : 0));
    if (ok) {
        tah_put_len_header(out, F_SCOPE_NAME, name.len);
        tah_put_bytes(out, name.ptr, name.len);
        if (version.len != 0) {
            tah_put_len_header(out, F_SCOPE_VERSION, version.len);
            tah_put_bytes(out, version.ptr, version.len);
        }
    }
    ta_str family = { 0 };
    ta_str revision = { 0 };
    bool known = tah_fact(otlp, run->definition, FACT_FAMILY, &family) &&
                 tah_fact(otlp, run->definition, FACT_REVISION, &revision);
    if (ok && known) {
        const uint8_t *bytes = (const uint8_t *)revision.ptr;
        ok = tah_kv_string(out, F_SCOPE_ATTRIBUTES, LIT("tinyactors.definition.family"), family) &&
             (tah_valid_utf8(bytes, revision.len)
                  ? tah_kv_string(out, F_SCOPE_ATTRIBUTES, LIT("tinyactors.definition.revision"), revision)
                  : tah_kv_bytes(out, F_SCOPE_ATTRIBUTES, LIT("tinyactors.definition.revision"),
                                 (ta_bytes){ bytes, revision.len }));
    }
    ok = ok && tah_kv_int(out, F_SCOPE_ATTRIBUTES, LIT("tinyactors.definition.id"), (int64_t)run->definition);
    if (ok && known) {
        ta_str document = { 0 };
        ta_str source = { 0 };
        (void)tah_fact(otlp, run->definition, FACT_DOCUMENT, &document);
        (void)tah_fact(otlp, run->definition, FACT_SOURCE, &source);
        ok = (document.len == 0 || tah_kv_string(out, F_SCOPE_ATTRIBUTES, LIT("tinyactors.definition.name"), document)) &&
             (source.len == 0 || tah_kv_string(out, F_SCOPE_ATTRIBUTES, LIT("tinyactors.definition.source"), source));
    }
    run->scope_len = (uint32_t)(out->len - run->scope_at);
    return ok;
}

/* The run `definition`'s spans go in, begun with its scope when it has none yet. */
static tah_otlp_run *tah_run_for(tah_otlp *otlp, uint64_t definition) {
    if (otlp->run_index_capacity != 0) {
        uint32_t index = *tah_run_slot(otlp, definition);
        if (index != 0) {
            return &otlp->runs[index - 1u];
        }
    }
    ta_allocator allocator = otlp->config.allocator;
    if (!tah_array_room(allocator, (void **)&otlp->runs, &otlp->run_capacity, otlp->run_count, sizeof *otlp->runs,
                        _Alignof(tah_otlp_run), otlp->run_count + 1u)) {
        return NULL;
    }
    if ((otlp->run_count + 1u) * 2u > otlp->run_index_capacity) {
        uint32_t capacity = otlp->run_index_capacity == 0 ? 32u : otlp->run_index_capacity * 2u;
        if (!tah_run_index_build(otlp, capacity)) {
            return NULL;
        }
    }
    tah_otlp_run *run = &otlp->runs[otlp->run_count];
    *run = (tah_otlp_run){ .definition = definition };
    size_t scopes_before = otlp->scopes.len;
    if (!tah_scope_encode(otlp, run)) {
        otlp->scopes.len = scopes_before;
        return NULL;
    }
    otlp->run_count += 1u;
    *tah_run_slot(otlp, definition) = otlp->run_count;
    otlp->scope_spans += tah_run_field_size(run);
    return run;
}

/* Counts the span of `len` bytes at `at` in `spans` as `run`'s latest. */
static void tah_run_add(tah_otlp *otlp, tah_otlp_run *run, size_t at, uint32_t len) {
    uint32_t index = otlp->entry_count;
    otlp->entries[index] = (tah_otlp_entry){ .at = at, .len = len };
    otlp->entry_count += 1u;
    if (run->last != 0) {
        otlp->entries[run->last - 1u].next = index + 1u;
    } else {
        run->first = index + 1u;
    }
    run->last = index + 1u;
    otlp->scope_spans -= tah_run_field_size(run);
    run->spans += len;
    otlp->scope_spans += tah_run_field_size(run);
}

/*
 * Forgets what a failed record added: the entries past `entries`, the runs
 * past `runs` and their scopes. The runs that stay are rebuilt from the
 * entries that stay. Only an allocation failure comes here.
 */
static void tah_runs_rollback(tah_otlp *otlp, uint32_t entries, uint32_t runs, size_t scopes) {
    otlp->entry_count = entries;
    otlp->run_count = runs;
    otlp->scopes.len = scopes;
    otlp->scope_spans = 0;
    for (uint32_t at = 0; at < runs; at += 1u) {
        tah_otlp_run *run = &otlp->runs[at];
        uint32_t index = run->first;
        run->spans = 0;
        run->last = 0;
        while (index != 0 && index <= entries) {
            run->spans += otlp->entries[index - 1u].len;
            run->last = index;
            index = otlp->entries[index - 1u].next;
        }
        if (run->last == 0) {
            run->first = 0;
        } else {
            otlp->entries[run->last - 1u].next = 0;
        }
        otlp->scope_spans += tah_run_field_size(run);
    }
    if (otlp->run_index_capacity != 0) {
        (void)tah_run_index_build(otlp, otlp->run_index_capacity);
    }
}

/* Appends a finished span to `spans`, in its definition's run. */
static bool tah_span_end(tah_otlp *otlp, const tah_otlp_span *span, uint64_t end) {
    size_t links = 0;
    if (span->link_span_id != 0) {
        links = tah_len_field_size(F_LINK_TRACE_ID, 16) + tah_len_field_size(F_LINK_SPAN_ID, 8);
    }
    size_t body = tah_len_field_size(F_SPAN_TRACE_ID, 16) + tah_len_field_size(F_SPAN_SPAN_ID, 8) +
                  (span->parent_span_id != 0 ? tah_len_field_size(F_SPAN_PARENT_SPAN_ID, 8) : 0) +
                  tah_len_field_size(F_SPAN_NAME, span->name.len) + 2u /* kind */ + 9u + 9u /* start, end */ +
                  span->attributes.len + span->events.len + (links != 0 ? tah_len_field_size(F_SPAN_LINKS, links) : 0) +
                  (span->error ? tah_len_field_size(F_SPAN_STATUS, 2) : 0);
    size_t entry = tah_len_field_size(F_SS_SPANS, body);
    tah_otlp_run *run = tah_run_for(otlp, span->definition);
    if (run == NULL || !tah_buf_room(&otlp->spans, entry) ||
        !tah_array_room(otlp->config.allocator, (void **)&otlp->entries, &otlp->entry_capacity, otlp->entry_count,
                        sizeof *otlp->entries, _Alignof(tah_otlp_entry), otlp->entry_count + 1u)) {
        return false;
    }
    tah_otlp_buf *out = &otlp->spans;
    size_t at = out->len;
    tah_put_len_header(out, F_SS_SPANS, body);
    tah_put_len_header(out, F_SPAN_TRACE_ID, 16);
    tah_put_bytes(out, span->trace_id, 16);
    tah_put_len_header(out, F_SPAN_SPAN_ID, 8);
    tah_put_id64(out, span->span_id);
    if (span->parent_span_id != 0) {
        tah_put_len_header(out, F_SPAN_PARENT_SPAN_ID, 8);
        tah_put_id64(out, span->parent_span_id);
    }
    tah_put_len_header(out, F_SPAN_NAME, span->name.len);
    tah_put_bytes(out, span->name.data, span->name.len);
    tah_put_tag(out, F_SPAN_KIND, WIRE_VARINT);
    tah_put_varint(out, SPAN_KIND_INTERNAL);
    tah_put_tag(out, F_SPAN_START, WIRE_I64);
    tah_put_fixed64(out, span->start);
    tah_put_tag(out, F_SPAN_END, WIRE_I64);
    tah_put_fixed64(out, end < span->start ? span->start : end);
    tah_put_bytes(out, span->attributes.data, span->attributes.len);
    tah_put_bytes(out, span->events.data, span->events.len);
    if (links != 0) {
        tah_put_len_header(out, F_SPAN_LINKS, links);
        tah_put_len_header(out, F_LINK_TRACE_ID, 16);
        tah_put_bytes(out, span->link_trace_id, 16);
        tah_put_len_header(out, F_LINK_SPAN_ID, 8);
        tah_put_id64(out, span->link_span_id);
    }
    if (span->error) {
        tah_put_len_header(out, F_SPAN_STATUS, 2);
        tah_put_tag(out, F_STATUS_CODE, WIRE_VARINT);
        tah_put_varint(out, STATUS_CODE_ERROR);
    }
    tah_run_add(otlp, run, at, (uint32_t)entry);
    otlp->span_count += 1u;
    return true;
}

static bool tah_span_name(tah_otlp_span *span, ta_str prefix, ta_str rest) {
    span->name.len = 0;
    if (!tah_buf_room(&span->name, prefix.len + rest.len)) {
        return false;
    }
    tah_put_bytes(&span->name, prefix.ptr, prefix.len);
    tah_put_bytes(&span->name, rest.ptr, rest.len);
    return true;
}

/* The time of a record: the host clock core stamped it with (core ADR
 * 0098), or, without one, the system's logical time. */
static uint64_t tah_record_time(const ta_trace_record *record) {
    return record->wall_ns != 0 ? record->wall_ns : (uint64_t)record->now;
}

/* Walks the names of a record's lists, in list order: transitions, exited,
 * entered, history, states, each a u32 length and its bytes. */
typedef struct {
    const uint8_t *at;
    const uint8_t *end;
} tah_list_names;

/* The next entry's name; "?" when the record has no more names. */
static ta_str tah_list_name(tah_list_names *names) {
    uint32_t len = 0;
    if (names->end - names->at < 4) {
        return LIT("?");
    }
    tah_mem_copy(&len, names->at, sizeof len);
    if ((size_t)(names->end - names->at) - 4u < len) {
        names->at = names->end;
        return LIT("?");
    }
    ta_str name = { (const char *)names->at + 4, len };
    names->at += 4u + len;
    return name;
}

/* The names of a record's lists from the first entry of list `skip` lists in. */
static tah_list_names tah_list_names_at(const ta_trace_record *record, uint32_t skip) {
    tah_list_names names = { record->list_names.ptr, record->list_names.ptr + record->list_names.len };
    const uint32_t counts[] = { record->transition_count, record->exited_count, record->entered_count,
                                record->history_count, record->state_count };
    for (uint32_t list = 0; list < skip; list += 1u) {
        for (uint32_t index = 0; index < counts[list]; index += 1u) {
            (void)tah_list_name(&names);
        }
    }
    return names;
}

enum { LIST_TRANSITIONS = 0u, LIST_EXITED = 1u, LIST_ENTERED = 2u, LIST_HISTORY = 3u, LIST_STATES = 4u };

/* Attributes every span and span event may carry about the record's event. */
static bool tah_event_attributes(tah_otlp *otlp, tah_otlp_buf *buf, uint32_t field, const ta_trace_record *record) {
    if (!record->has_event) {
        return true;
    }
    ta_event_type type = record->event_type;
    bool ok = tah_kv_string(buf, field, LIT("scxml.event.name"), ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_NAME)) &&
              tah_kv_string(buf, field, LIT("scxml.event.type"),
                            tah_cstr(tah_event_types[type <= TA_EVENT_PLATFORM ? type : 0]));
    ta_str send_id = ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_SEND_ID);
    if (ok && send_id.len != 0) {
        ok = tah_kv_string(buf, field, LIT("scxml.event.sendid"), send_id);
    }
    ta_str invoke_id = ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_INVOKE_ID);
    if (ok && invoke_id.len != 0) {
        ok = tah_kv_string(buf, field, LIT("scxml.event.invokeid"), invoke_id);
    }
    ta_str data = otlp->config.values ? ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_DATA) : (ta_str){ 0 };
    if (ok && data.len != 0) {
        ok = tah_kv_string(buf, field, LIT("scxml.event.data"), data) &&
             (!record->event_data_cut || tah_kv_bool(buf, field, LIT("scxml.event.data.truncated"), true));
    }
    return ok;
}

/* The attributes of a step's span: the session, step and actor. */
static bool tah_step_attributes(tah_otlp_buf *buf, const ta_trace_record *record) {
    char actor[48];
    ta_str slot = tah_decimal(ta_actor_id_slot(record->actor), actor);
    actor[slot.len] = ':';
    ta_str generation = tah_decimal(ta_actor_id_generation(record->actor), actor + slot.len + 1u);
    return tah_kv_string(buf, F_SPAN_ATTRIBUTES, LIT("scxml.session_id"),
                         ta_trace_record_text(record, TA_TRACE_TEXT_SESSION_ID)) &&
           tah_kv_int(buf, F_SPAN_ATTRIBUTES, LIT("scxml.macrostep"), (int64_t)record->key.macrostep) &&
           tah_kv_string(buf, F_SPAN_ATTRIBUTES, LIT("scxml.actor"),
                         (ta_str){ actor, slot.len + 1u + generation.len }) &&
           tah_kv_int(buf, F_SPAN_ATTRIBUTES, LIT("scxml.definition"), (int64_t)record->definition) &&
           tah_kv_int(buf, F_SPAN_ATTRIBUTES, LIT("scxml.time"), (int64_t)record->now);
}

/* A cause, as three attributes (the span parent says the same, lossily). */
static bool tah_cause_attributes(tah_otlp_buf *buf, uint32_t field, const ta_trace_record *record) {
    const ta_step_key *cause = &record->cause;
    if (tah_key_empty(cause)) {
        return true;
    }
    return tah_kv_string(buf, field, LIT("scxml.cause.session_id"),
                         ta_trace_record_text(record, TA_TRACE_TEXT_CAUSE_SESSION_ID)) &&
           tah_kv_int(buf, field, LIT("scxml.cause.macrostep"), (int64_t)cause->macrostep) &&
           tah_kv_int(buf, field, LIT("scxml.cause.microstep"), (int64_t)cause->microstep);
}

/* A list of a record's states: names, and IDs (names are unique within a chart). */
typedef struct {
    ta_str key;
    ta_str id_key;
    uint32_t list;
    const ta_state_id *states;
    uint32_t count;
} tah_states_params;

static bool tah_states_attribute(tah_otlp_buf *buf, const ta_trace_record *record, const tah_states_params *params) {
    /* Two passes over the names: sizes first, then bytes. */
    size_t array = 0;
    tah_list_names names = tah_list_names_at(record, params->list);
    for (uint32_t at = 0; at < params->count; at += 1u) {
        ta_str name = tah_list_name(&names);
        array += tah_len_field_size(F_ARRAY_VALUES, tah_any_string_size(name.len));
    }
    size_t any = tah_len_field_size(F_ANY_ARRAY, array);
    if (!tah_buf_room(buf, tah_len_field_size(F_SPAN_ATTRIBUTES, tah_kv_size(params->key, any)))) {
        return false;
    }
    tah_put_kv_head(buf, F_SPAN_ATTRIBUTES, params->key, any);
    tah_put_len_header(buf, F_ANY_ARRAY, array);
    names = tah_list_names_at(record, params->list);
    for (uint32_t at = 0; at < params->count; at += 1u) {
        ta_str name = tah_list_name(&names);
        tah_put_len_header(buf, F_ARRAY_VALUES, tah_any_string_size(name.len));
        tah_put_len_header(buf, F_ANY_STRING, name.len);
        tah_put_bytes(buf, name.ptr, name.len);
    }
    return tah_kv_int_array(buf, F_SPAN_ATTRIBUTES, params->id_key, params->states, params->count);
}

static bool tah_transitions_attribute(tah_otlp_buf *buf, const ta_trace_record *record) {
    size_t array = 0;
    tah_list_names names = tah_list_names_at(record, LIST_TRANSITIONS);
    for (uint32_t at = 0; at < record->transition_count; at += 1u) {
        ta_str name = tah_list_name(&names);
        array += tah_len_field_size(F_ARRAY_VALUES, tah_any_string_size(name.len));
    }
    size_t any = tah_len_field_size(F_ANY_ARRAY, array);
    ta_str key = LIT("scxml.transition");
    if (!tah_buf_room(buf, tah_len_field_size(F_SPAN_ATTRIBUTES, tah_kv_size(key, any)))) {
        return false;
    }
    tah_put_kv_head(buf, F_SPAN_ATTRIBUTES, key, any);
    tah_put_len_header(buf, F_ANY_ARRAY, array);
    names = tah_list_names_at(record, LIST_TRANSITIONS);
    for (uint32_t at = 0; at < record->transition_count; at += 1u) {
        ta_str name = tah_list_name(&names);
        tah_put_len_header(buf, F_ARRAY_VALUES, tah_any_string_size(name.len));
        tah_put_len_header(buf, F_ANY_STRING, name.len);
        tah_put_bytes(buf, name.ptr, name.len);
    }
    return tah_kv_int_array(buf, F_SPAN_ATTRIBUTES, LIT("scxml.transition.id"), record->transitions,
                            record->transition_count);
}

/* Opens `span` for a record's step. */
static bool tah_span_open(tah_otlp *otlp, tah_otlp_span *span, const ta_trace_record *record, ta_step_key key,
                          uint64_t now) {
    tah_span_reset(span);
    tah_trace_id_of(otlp, &record->context, span->trace_id);
    span->span_id = ta_step_key_span_id(key);
    span->definition = record->definition;
    span->start = now;
    if (record->kind == TA_TRACE_MACROSTEP_BEGIN) {
        otlp->last_macrostep = tah_exemplar_of(&record->context, span->span_id, now);
    }
    span->microstep = key.microstep;
    return tah_step_attributes(&span->attributes, record);
}

/* Parents `span` under its cause, or links it there. */
static void tah_span_parent(tah_otlp *otlp, tah_otlp_span *span, const ta_step_key *cause) {
    if (tah_key_empty(cause)) {
        return;
    }
    uint64_t parent = ta_step_key_span_id(*cause);
    if (parent == 0) {
        return;
    }
    if (otlp->config.link_parents) {
        tah_mem_copy(span->link_trace_id, span->trace_id, 16);
        span->link_span_id = parent;
    } else {
        span->parent_span_id = parent;
    }
}

/* What ending a session's open spans marks on them. */
typedef struct {
    uint64_t now;
    bool incomplete;
    /* Records lost to a full tel ring (core ADR 0098): 0, or the count, which
     * also marks the spans as errors. */
    uint64_t dropped;
} tah_open_end_params;

/* One span's marks, then its end. */
static bool tah_span_finish(tah_otlp *otlp, tah_otlp_span *span, const tah_open_end_params *params) {
    if (params->dropped != 0) {
        span->error = true;
    }
    return (!params->incomplete || tah_kv_bool(&span->attributes, F_SPAN_ATTRIBUTES, LIT("scxml.incomplete"), true)) &&
           (params->dropped == 0 || tah_kv_int(&span->attributes, F_SPAN_ATTRIBUTES,
                                               LIT("tinyactors.telemetry.dropped"), (int64_t)params->dropped)) &&
           tah_span_end(otlp, span, params->now);
}

/* Ends a session's open spans, marked as `params` says. */
static bool tah_open_end(tah_otlp *otlp, tah_otlp_open *open, const tah_open_end_params *params) {
    bool ok = true;
    if (open->has_micro) {
        ok = tah_span_finish(otlp, &open->micro, params);
        open->has_micro = false;
    }
    ok = ok && tah_span_finish(otlp, &open->macro, params);
    tah_open_remove(otlp, open);
    return ok;
}

/* The session's open macrostep, opened as "continued" when a record comes
 * for a macrostep whose BEGIN this encoder never saw (a sink installed
 * mid-macrostep): no record is dropped. */
static tah_otlp_open *tah_open_for(tah_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tah_otlp_open *open = tah_open_find(otlp, record->key.session);
    if (open != NULL) {
        return open;
    }
    open = tah_open_add(otlp, record->key.session);
    if (open == NULL) {
        return NULL;
    }
    open->macrostep = record->key.macrostep;
    ta_step_key begin = record->key;
    begin.microstep = 0;
    if (!tah_span_open(otlp, &open->macro, record, begin, now) ||
        !tah_span_name(&open->macro, LIT("scxml.macrostep"), LIT(" (continued)")) ||
        !tah_kv_bool(&open->macro.attributes, F_SPAN_ATTRIBUTES, LIT("scxml.continued"), true)) {
        tah_open_remove(otlp, open);
        return NULL;
    }
    return open;
}

static bool tah_begin(tah_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tah_otlp_open *open = tah_open_find(otlp, record->key.session);
    if (open != NULL && !tah_open_end(otlp, open, &(tah_open_end_params){ .now = now, .incomplete = true })) {
        return false;
    }
    open = tah_open_add(otlp, record->key.session);
    if (open == NULL) {
        return false;
    }
    open->macrostep = record->key.macrostep;
    tah_otlp_span *span = &open->macro;
    ta_str what = record->has_event          ? ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_NAME)
                  : record->key.macrostep == 1 ? LIT("bootstrap")
                                               : LIT("(no event)");
    bool ok = tah_span_open(otlp, span, record, record->key, now) &&
              tah_span_name(span, LIT("scxml.macrostep "), what) &&
              tah_event_attributes(otlp, &span->attributes, F_SPAN_ATTRIBUTES, record) &&
              tah_cause_attributes(&span->attributes, F_SPAN_ATTRIBUTES, record) &&
              tah_kv_int(&span->attributes, F_SPAN_ATTRIBUTES, LIT("scxml.trace.flags"), record->context.flags);
    if (!ok) {
        tah_open_remove(otlp, open);
        return false;
    }
    tah_span_parent(otlp, span, &record->cause);
    return true;
}

static bool tah_microstep(tah_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tah_otlp_open *open = tah_open_for(otlp, record, now);
    if (open == NULL) {
        return false;
    }
    if (open->has_micro) {
        if (!tah_span_end(otlp, &open->micro, now)) {
            return false;
        }
        open->has_micro = false;
    }
    tah_otlp_span *span = &open->micro;
    ta_str what = record->has_event ? ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_NAME) : LIT("(eventless)");
    bool ok = tah_span_open(otlp, span, record, record->key, now) &&
              tah_span_name(span, LIT("scxml.microstep "), what) &&
              tah_kv_int(&span->attributes, F_SPAN_ATTRIBUTES, LIT("scxml.microstep"), record->key.microstep) &&
              tah_event_attributes(otlp, &span->attributes, F_SPAN_ATTRIBUTES, record) &&
              tah_transitions_attribute(&span->attributes, record) &&
              tah_states_attribute(&span->attributes, record,
                                   &(tah_states_params){ .key = LIT("scxml.state.exited"),
                                                         .id_key = LIT("scxml.state.exited.id"),
                                                         .list = LIST_EXITED, .states = record->exited,
                                                         .count = record->exited_count }) &&
              tah_states_attribute(&span->attributes, record,
                                   &(tah_states_params){ .key = LIT("scxml.state.entered"),
                                                         .id_key = LIT("scxml.state.entered.id"),
                                                         .list = LIST_ENTERED, .states = record->entered,
                                                         .count = record->entered_count }) &&
              (record->history_count == 0 ||
               tah_states_attribute(&span->attributes, record,
                                    &(tah_states_params){ .key = LIT("scxml.state.history"),
                                                          .id_key = LIT("scxml.state.history.id"),
                                                          .list = LIST_HISTORY, .states = record->history,
                                                          .count = record->history_count }));
    if (!ok) {
        return false;
    }
    span->parent_span_id = open->macro.span_id;
    tah_mem_copy(span->trace_id, open->macro.trace_id, 16);
    open->has_micro = true;
    return true;
}

static bool tah_end(tah_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tah_otlp_open *open = tah_open_for(otlp, record, now);
    if (open == NULL) {
        return false;
    }
    if (!tah_states_attribute(&open->macro.attributes, record,
                              &(tah_states_params){ .key = LIT("scxml.state"), .id_key = LIT("scxml.state.id"),
                                                    .list = LIST_STATES, .states = record->states,
                                                    .count = record->state_count })) {
        return false;
    }
    return tah_open_end(otlp, open, &(tah_open_end_params){ .now = now });
}

/* Where in the macrostep a record was written. */
static bool tah_position_attributes(tah_otlp_buf *buf, const ta_trace_record *record) {
    const ta_trace_position *at = &record->position;
    bool ok = true;
    if (at->at != TA_TRACE_AT_NONE) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.position"),
                           tah_cstr(tah_at_names[(uint32_t)at->at <= TA_TRACE_AT_INVOKE ? at->at : 0]));
    }
    if (ok && at->state != TA_STATE_NONE) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.state"),
                           ta_trace_record_text(record, TA_TRACE_TEXT_POSITION_STATE)) &&
             tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.state.id"), at->state);
    }
    if (ok && at->transition != TA_TRANSITION_NONE) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.transition"),
                           ta_trace_record_text(record, TA_TRACE_TEXT_POSITION_TRANSITION)) &&
             tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.transition.id"), at->transition);
    }
    if (ok && at->action != TA_ACTION_NONE) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action"),
                           ta_trace_record_text(record, TA_TRACE_TEXT_POSITION_ACTION)) &&
             tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action.id"), at->action);
    }
    if (ok && (at->iteration != 0 || record->kind == TA_TRACE_FOREACH)) {
        ok = tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.iteration"), at->iteration);
    }
    return ok;
}

/*
 * What went wrong, in words: the text of the value the failure carries
 * (for napi, a thrown Error as "Error: message"), else the fault's own
 * message. Written whatever `values` says: at 3am the reason is the one
 * value an operator cannot do without. Core cut it at telemetryValueBytes.
 */
static ta_str tah_fault_text(const ta_trace_record *record) {
    ta_str data = ta_trace_record_text(record, TA_TRACE_TEXT_FAULT_DATA);
    return data.len != 0 ? data : ta_trace_record_text(record, TA_TRACE_TEXT_FAULT_MESSAGE);
}

/* The fields of a record that becomes a span event. */
static bool tah_record_attributes(tah_otlp *otlp, tah_otlp_buf *buf, const ta_trace_record *record) {
    bool ok = tah_position_attributes(buf, record) &&
              tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.microstep"), record->key.microstep) &&
              tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.time"), (int64_t)record->now) &&
              tah_event_attributes(otlp, buf, F_EVENT_ATTRIBUTES, record);
    if (ok && record->expr != TA_EXPR_ID_NONE) {
        ok = tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.expression.id"), record->expr);
    }
    if (ok && record->kind == TA_TRACE_CONDITION && !record->has_fault) {
        ok = tah_kv_bool(buf, F_EVENT_ATTRIBUTES, LIT("scxml.result"), record->result);
    }
    ta_str name = ta_trace_record_text(record, TA_TRACE_TEXT_NAME);
    if (ok && name.len != 0) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.data.name"), name);
    }
    if (ok && record->action != TA_ACTION_NONE && record->action != record->position.action) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action"),
                           ta_trace_record_text(record, TA_TRACE_TEXT_ACTION)) &&
             tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.action.id"), record->action);
    }
    if (ok && record->iteration != 0 && record->iteration != record->position.iteration) {
        ok = tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.iteration"), record->iteration);
    }
    ta_str send_id = ta_trace_record_text(record, TA_TRACE_TEXT_SEND_ID);
    if (ok && send_id.len != 0) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.send_id"), send_id);
    }
    ta_str invoke_id = ta_trace_record_text(record, TA_TRACE_TEXT_INVOKE_ID);
    if (ok && invoke_id.len != 0) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.invoke_id"), invoke_id);
    }
    if (ok && record->has_fault) {
        ok = tah_kv_int(buf, F_EVENT_ATTRIBUTES, LIT("scxml.fault.code"), record->fault_code);
        ta_str detail = ta_trace_record_text(record, TA_TRACE_TEXT_FAULT_DETAIL);
        if (ok && detail.len != 0) {
            ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.fault.detail"), detail);
        }
        /* What went wrong, as the failure's log says it: a debugger shows it
         * where the fault stopped the actor. */
        ta_str reason = tah_fault_text(record);
        if (ok && reason.len != 0) {
            ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.fault.message"), reason);
        }
    }
    ta_str value = otlp->config.values && record->has_value ? ta_trace_record_text(record, TA_TRACE_TEXT_VALUE)
                                                             : (ta_str){ 0 };
    if (ok && value.len != 0) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.value"), value) &&
             (!record->value_cut || tah_kv_bool(buf, F_EVENT_ATTRIBUTES, LIT("scxml.value.truncated"), true));
    }
    if (ok && record->state != TA_STATE_NONE) {
        ok = tah_kv_string(buf, F_EVENT_ATTRIBUTES, LIT("scxml.final_state"),
                           ta_trace_record_text(record, TA_TRACE_TEXT_STATE));
    }
    return ok && tah_cause_attributes(buf, F_EVENT_ATTRIBUTES, record);
}

/* A record inside a macrostep: a span event on the step it happened in. */
static bool tah_span_event(tah_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tah_otlp_open *open = tah_open_for(otlp, record, now);
    if (open == NULL) {
        return false;
    }
    tah_otlp_span *target = open->has_micro && record->key.microstep != 0 && record->key.microstep == open->micro.microstep
                                ? &open->micro
                                : &open->macro;
    otlp->scratch.len = 0;
    if (!tah_record_attributes(otlp, &otlp->scratch, record)) {
        return false;
    }
    char name[40] = "scxml.";
    const char *kind = tah_kind_names[record->kind];
    size_t kind_len = tah_str_length(kind);
    tah_mem_copy(name + 6, kind, kind_len);
    ta_str event_name = { name, (uint32_t)(6u + kind_len) };
    size_t body = 9u + tah_len_field_size(F_EVENT_NAME, event_name.len) + otlp->scratch.len;
    if (!tah_buf_room(&target->events, tah_len_field_size(F_SPAN_EVENTS, body))) {
        return false;
    }
    tah_put_len_header(&target->events, F_SPAN_EVENTS, body);
    tah_put_tag(&target->events, F_EVENT_TIME, WIRE_I64);
    tah_put_fixed64(&target->events, now);
    tah_put_len_header(&target->events, F_EVENT_NAME, event_name.len);
    tah_put_bytes(&target->events, event_name.ptr, event_name.len);
    tah_put_bytes(&target->events, otlp->scratch.data, otlp->scratch.len);
    if (record->has_fault || record->kind == TA_TRACE_ACTION_FAILED) {
        target->error = true;
    }
    return true;
}

/* A lifecycle record: a span of no duration. */
static bool tah_lifecycle(tah_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    if (record->kind == TA_TRACE_UNLOADED || record->kind == TA_TRACE_DESTROYED) {
        tah_otlp_open *open = tah_open_find(otlp, record->key.session);
        if (open != NULL && !tah_open_end(otlp, open, &(tah_open_end_params){ .now = now, .incomplete = true })) {
            return false;
        }
    }
    uint32_t wanted = (record->context.detail & TA_TRACE_DETAIL_WATCHED) != 0 ? otlp->config.wanted_watched_kinds
                                                                              : otlp->config.wanted_kinds;
    if ((wanted & TA_TRACE_BIT(record->kind)) == 0) {
        return true;
    }
    tah_otlp_span *span = &otlp->lifecycle;
    tah_span_reset(span);
    bool ok = tah_span_open(otlp, span, record, record->key, now) &&
              tah_span_name(span, LIT("scxml."), tah_cstr(tah_kind_names[record->kind])) &&
              tah_cause_attributes(&span->attributes, F_SPAN_ATTRIBUTES, record) &&
              (record->state == TA_STATE_NONE ||
               tah_kv_string(&span->attributes, F_SPAN_ATTRIBUTES, LIT("scxml.final_state"),
                             ta_trace_record_text(record, TA_TRACE_TEXT_STATE)));
    if (ok) {
        tah_span_parent(otlp, span, &record->cause);
        ok = tah_span_end(otlp, span, now);
    }
    return ok;
}


/* ------------------------------------------------------------------------
 * Logs (napi ADR 0017 section 7, core ADR 0072 section 2)
 */

typedef struct {
    uint64_t now;
    uint32_t severity;
    ta_str severity_text;
    ta_str event_name;          /* LogRecord.event_name: what happened */
    ta_str body;
    const ta_trace_context *context;
    ta_step_key key;             /* the step the log belongs to */
    uint64_t definition;
    ta_str key_name;             /* what the rate limit counts by, with the definition */
} tah_log_line;

static uint64_t tah_log_key(uint64_t definition, ta_str name) {
    uint64_t hash = definition * 0x9e3779b97f4a7c15ull;
    for (uint32_t at = 0; at < name.len; at += 1u) {
        hash = (hash ^ (uint8_t)name.ptr[at]) * 0x100000001b3ull;
    }
    return hash == 0 ? 1u : hash;
}

/* Whether a log passes the rate limit; `*suppressed` is how many were dropped before it. */
static bool tah_log_allowed(tah_otlp *otlp, const tah_log_line *line, uint32_t *suppressed) {
    uint64_t key = tah_log_key(line->definition, line->key_name);
    uint64_t second = line->now / 1000000000ull;
    uint32_t slot = (uint32_t)(key & 255u);
    if (otlp->log_rates[slot].key != key || otlp->log_rates[slot].window != second) {
        uint32_t carried = otlp->log_rates[slot].key == key ? otlp->log_rates[slot].suppressed : 0u;
        otlp->log_rates[slot].key = key;
        otlp->log_rates[slot].window = second;
        otlp->log_rates[slot].count = 0;
        otlp->log_rates[slot].suppressed = carried;
    }
    if (otlp->log_rates[slot].count >= otlp->config.log_limit) {
        otlp->log_rates[slot].suppressed += 1u;
        return false;
    }
    otlp->log_rates[slot].count += 1u;
    *suppressed = otlp->log_rates[slot].suppressed;
    otlp->log_rates[slot].suppressed = 0;
    return true;
}

/* Appends one LogRecord whose attributes are already in `otlp->scratch`. */
static bool tah_log_append(tah_otlp *otlp, const tah_log_line *line) {
    uint32_t suppressed = 0;
    if (!tah_log_allowed(otlp, line, &suppressed)) {
        return true;
    }
    tah_otlp_buf *attributes = &otlp->scratch;
    if (suppressed != 0 &&
        !tah_kv_int(attributes, F_LOG_ATTRIBUTES, LIT("tinyactors.suppressed"), (int64_t)suppressed)) {
        return false;
    }
    bool traced = !tah_context_empty(line->context);
    size_t body = tah_len_field_size(F_ANY_STRING, line->body.len);
    size_t size = 9u + 9u /* time, observed time */ + 2u + tah_len_field_size(F_LOG_SEVERITY_TEXT, line->severity_text.len) +
                  tah_len_field_size(F_LOG_BODY, body) + attributes->len +
                  tah_len_field_size(F_LOG_EVENT_NAME, line->event_name.len) +
                  (traced ? 5u + tah_len_field_size(F_LOG_TRACE_ID, 16) + tah_len_field_size(F_LOG_SPAN_ID, 8) : 0u);
    if (!tah_buf_room(&otlp->logs, tah_len_field_size(F_SL_LOG_RECORDS, size))) {
        return false;
    }
    tah_otlp_buf *out = &otlp->logs;
    tah_put_len_header(out, F_SL_LOG_RECORDS, size);
    tah_put_tag(out, F_LOG_TIME, WIRE_I64);
    tah_put_fixed64(out, line->now);
    tah_put_tag(out, F_LOG_OBSERVED_TIME, WIRE_I64);
    tah_put_fixed64(out, line->now);
    tah_put_tag(out, F_LOG_SEVERITY_NUMBER, WIRE_VARINT);
    tah_put_varint(out, line->severity);
    tah_put_len_header(out, F_LOG_SEVERITY_TEXT, line->severity_text.len);
    tah_put_bytes(out, line->severity_text.ptr, line->severity_text.len);
    tah_put_len_header(out, F_LOG_BODY, body);
    tah_put_len_header(out, F_ANY_STRING, line->body.len);
    tah_put_bytes(out, line->body.ptr, line->body.len);
    tah_put_bytes(out, attributes->data, attributes->len);
    tah_put_len_header(out, F_LOG_EVENT_NAME, line->event_name.len);
    tah_put_bytes(out, line->event_name.ptr, line->event_name.len);
    if (traced) {
        uint8_t trace_id[16];
        tah_trace_id_of(otlp, line->context, trace_id);
        tah_put_tag(out, F_LOG_FLAGS, 5u /* I32 */);
        uint32_t flags = line->context->flags & TA_TRACE_FLAG_SAMPLED;
        uint8_t le[4] = { (uint8_t)flags, 0, 0, 0 };
        tah_put_bytes(out, le, 4);
        tah_put_len_header(out, F_LOG_TRACE_ID, 16);
        tah_put_bytes(out, trace_id, 16);
        tah_put_len_header(out, F_LOG_SPAN_ID, 8);
        tah_put_id64(out, ta_step_key_span_id(line->key));
    }
    otlp->log_count += 1u;
    return true;
}

/* The attributes every log of a record carries, into `otlp->scratch`. */
static bool tah_log_attributes(tah_otlp *otlp, const ta_trace_record *record) {
    tah_otlp_buf *buf = &otlp->scratch;
    buf->len = 0;
    bool ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.session_id"),
                            ta_trace_record_text(record, TA_TRACE_TEXT_SESSION_ID)) &&
              tah_kv_int(buf, F_LOG_ATTRIBUTES, LIT("scxml.macrostep"), (int64_t)record->key.macrostep) &&
              tah_kv_int(buf, F_LOG_ATTRIBUTES, LIT("scxml.microstep"), record->key.microstep);
    ta_str family = { 0 };
    ta_str revision = { 0 };
    if (ok && tah_fact(otlp, record->definition, FACT_FAMILY, &family) &&
        tah_fact(otlp, record->definition, FACT_REVISION, &revision)) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.family"), family) &&
             tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.revision"), revision);
    }
    if (ok && record->position.state != TA_STATE_NONE) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.state"),
                           ta_trace_record_text(record, TA_TRACE_TEXT_POSITION_STATE));
    }
    bool own_action = record->action != TA_ACTION_NONE;
    ta_action_id action = own_action ? record->action : record->position.action;
    if (ok && action != TA_ACTION_NONE) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.action"),
                           ta_trace_record_text(record, own_action ? TA_TRACE_TEXT_ACTION
                                                                   : TA_TRACE_TEXT_POSITION_ACTION)) &&
             tah_kv_int(buf, F_LOG_ATTRIBUTES, LIT("scxml.action.id"), (int64_t)action);
    }
    /* A long or runaway macrostep (core ADR 0075): where the actor was, and
     * the transition it was looping through. */
    if (ok && record->state_count != 0 && record->position.state == TA_STATE_NONE) {
        tah_list_names names = tah_list_names_at(record, LIST_STATES);
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.state"), tah_list_name(&names));
    }
    if (ok && record->transition_count != 0 &&
        (record->kind == TA_TRACE_LONG_MACROSTEP || record->kind == TA_TRACE_RUNAWAY)) {
        tah_list_names names = tah_list_names_at(record, LIST_TRANSITIONS);
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.transition"), tah_list_name(&names)) &&
             tah_kv_int(buf, F_LOG_ATTRIBUTES, LIT("scxml.transition.id"), (int64_t)record->transitions[0]);
    }
    if (ok && record->has_fault) {
        ta_str message = ta_trace_record_text(record, TA_TRACE_TEXT_FAULT_MESSAGE);
        ok = tah_kv_int(buf, F_LOG_ATTRIBUTES, LIT("scxml.fault.code"), (int64_t)record->fault_code) &&
             (message.len == 0 || tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.fault.message"), message));
    }
    return ok;
}

/* `log_body` = "<prefix>: <reason>", or the prefix alone when there is no reason. */
static bool tah_log_body(tah_otlp *otlp, ta_str prefix, ta_str reason) {
    otlp->log_body.len = 0;
    if (!tah_buf_room(&otlp->log_body, prefix.len + 2u + reason.len)) {
        return false;
    }
    tah_put_bytes(&otlp->log_body, prefix.ptr, prefix.len);
    if (reason.len != 0) {
        tah_put_bytes(&otlp->log_body, ": ", 2);
        tah_put_bytes(&otlp->log_body, reason.ptr, reason.len);
    }
    return true;
}

static bool tah_starts_with(ta_str text, ta_str prefix) {
    return text.len >= prefix.len && tah_mem_equal(text.ptr, prefix.ptr, prefix.len);
}

/*
 * A failure record as a WARN log (core ADR 0072 section 5). Other records
 * log nothing. The body says what failed and why, in words, because the
 * OTLP log data model makes the body the human-readable message; the same
 * facts are attributes, for filtering:
 *
 *   tinyactors.action.failed  "<script> in ready on reserve failed: Error: cannot reserve 50 items"
 *   tinyactors.error.raised   "error.execution: Error: cannot reserve 50 items"
 *
 * `exception.message` (OpenTelemetry's semantic convention) carries the
 * reason alone.
 */
static bool tah_log_failure(tah_otlp *otlp, const ta_trace_record *record, uint64_t now) {
    tah_log_line line = { .now = now, .severity = SEVERITY_WARN, .severity_text = LIT("WARN"),
                          .context = &record->context, .key = record->key, .definition = record->definition };
    ta_str reason = { 0 };
    size_t name_len = 0; /* RAISED: the event name's length at the start of the body */
    bool ok;
    switch (record->kind) {
    case TA_TRACE_ACTION_FAILED: {
        line.event_name = LIT("tinyactors.action.failed");
        reason = record->has_fault ? tah_fault_text(record) : (ta_str){ 0 };
        ta_str what = record->action != TA_ACTION_NONE ? ta_trace_record_text(record, TA_TRACE_TEXT_ACTION)
                                                       : LIT("an action");
        otlp->log_body.len = 0;
        ok = tah_buf_room(&otlp->log_body, what.len + 9u + reason.len);
        if (ok) {
            tah_put_bytes(&otlp->log_body, what.ptr, what.len);
            tah_put_bytes(&otlp->log_body, " failed", 7);
            if (reason.len != 0) {
                tah_put_bytes(&otlp->log_body, ": ", 2);
                tah_put_bytes(&otlp->log_body, reason.ptr, reason.len);
            }
        }
        break;
    }
    case TA_TRACE_STOPPED:
        line.event_name = LIT("tinyactors.run.stopped");
        ok = tah_log_body(otlp, LIT("the run stopped: an allocation failed (ADR 0070)"), (ta_str){ 0 });
        break;
    case TA_TRACE_LONG_MACROSTEP:
    case TA_TRACE_RUNAWAY: {
        /* Core ADR 0075: how many microsteps, and through which transition. */
        bool runaway = record->kind == TA_TRACE_RUNAWAY;
        line.event_name = runaway ? LIT("tinyactors.actor.runaway") : LIT("tinyactors.macrostep.long");
        char digits[24];
        ta_str count = tah_decimal(record->key.microstep, digits);
        ta_str intro = LIT(" microsteps in one macrostep, the last through ");
        tah_list_names names = tah_list_names_at(record, LIST_TRANSITIONS);
        ta_str through = record->transition_count != 0 ? tah_list_name(&names) : LIT("the initial transition");
        ta_str outro = runaway ? LIT(": the microstep limit, so the run stopped") : LIT(": a tenth of the microstep limit");
        otlp->log_body.len = 0;
        ok = tah_buf_room(&otlp->log_body, count.len + intro.len + through.len + outro.len);
        if (ok) {
            tah_put_bytes(&otlp->log_body, count.ptr, count.len);
            tah_put_bytes(&otlp->log_body, intro.ptr, intro.len);
            tah_put_bytes(&otlp->log_body, through.ptr, through.len);
            tah_put_bytes(&otlp->log_body, outro.ptr, outro.len);
        }
        break;
    }
    case TA_TRACE_RAISED: {
        if (!record->has_event) {
            return true;
        }
        ta_str name = ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_NAME);
        if (!tah_starts_with(name, LIT("error."))) {
            return true;
        }
        line.event_name = LIT("tinyactors.error.raised");
        otlp->log_body.len = 0;
        ok = tah_buf_room(&otlp->log_body, name.len);
        if (ok) {
            tah_put_bytes(&otlp->log_body, name.ptr, name.len);
            name_len = name.len;
            reason = ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_DATA);
            ok = tah_buf_room(&otlp->log_body, 2u + reason.len);
            if (ok && reason.len != 0) {
                tah_put_bytes(&otlp->log_body, ": ", 2);
                tah_put_bytes(&otlp->log_body, reason.ptr, reason.len);
            }
        }
        break;
    }
    default:
        return true;
    }
    if (!ok) {
        return false;
    }
    line.key_name = line.event_name;
    line.body = (ta_str){ (const char *)otlp->log_body.data, (uint32_t)otlp->log_body.len };
    /* The reason and the event name, as attributes, point into log_body. */
    ta_str kept_reason = { line.body.ptr + (line.body.len - reason.len), reason.len };
    return tah_log_attributes(otlp, record) &&
           (reason.len == 0 ||
            tah_kv_string(&otlp->scratch, F_LOG_ATTRIBUTES, LIT("exception.message"), kept_reason)) &&
           (record->kind != TA_TRACE_RAISED ||
            tah_kv_string(&otlp->scratch, F_LOG_ATTRIBUTES, LIT("scxml.event.name"),
                          (ta_str){ line.body.ptr, (uint32_t)name_len })) &&
           tah_log_append(otlp, &line);
}

void tah_otlp_host_log(tah_otlp *otlp, const tah_otlp_host_log_params *log) {
    if (otlp->config.log_limit == 0) {
        return;
    }
    static const ta_trace_context untraced = { 0 };
    tah_log_line line = { .now = otlp->config.host.now(otlp->config.host.self), .severity = SEVERITY_WARN,
                          .severity_text = LIT("WARN"), .event_name = log->event_name, .context = &untraced,
                          .definition = log->definition, .key_name = log->event_name };
    ta_str event = log->has_event ? tah_event_name(otlp, log->event) : (ta_str){ 0 };
    otlp->log_body.len = 0;
    bool ok = tah_buf_room(&otlp->log_body, event.len + 1u + log->body.len);
    if (ok) {
        tah_put_bytes(&otlp->log_body, event.ptr, event.len);
        if (event.len != 0) {
            tah_put_bytes(&otlp->log_body, " ", 1);
        }
        tah_put_bytes(&otlp->log_body, log->body.ptr, log->body.len);
    }
    line.body = (ta_str){ (const char *)otlp->log_body.data, (uint32_t)otlp->log_body.len };
    tah_otlp_buf *buf = &otlp->scratch;
    buf->len = 0;
    char decimal[TAH_SESSION_TEXT];
    ta_definition_inspection view;
    bool defined = log->definition != 0 && ta_definition_inspect(otlp->config.core, log->definition, &view) == TA_OK;
    if (ok && log->session != 0) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.session_id"),
                           tah_session_text(otlp, log->session, defined ? view.identity.family.name : (ta_str){ 0 },
                                            decimal));
    }
    if (ok && defined) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.family"), view.identity.family.name) &&
             tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.revision"),
                           (ta_str){ (const char *)view.identity.revision.ptr, view.identity.revision.len });
    }
    if (ok && event.len != 0) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.event.name"), (ta_str){ line.body.ptr, event.len });
    }
    if (ok && log->key.len != 0) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, log->key, log->value);
    }
    if (!ok || !tah_log_append(otlp, &line)) {
        otlp->dropped += 1u;
    }
}

void tah_otlp_definition_log(tah_otlp *otlp, const tah_otlp_definition_log_params *log) {
    if (otlp->config.log_limit == 0) {
        return;
    }
    static const ta_trace_context untraced = { 0 };
    static const ta_str names[] = { LIT("tinyactors.definition.registered"), LIT("tinyactors.definition.retired"),
                                    LIT("tinyactors.definition.freed") };
    static const ta_str verbs[] = { LIT("registered"), LIT("retired"), LIT("freed") };
    const ta_definition_event *event = log->event;
    uint32_t kind = event->kind <= TA_DEFINITION_EVENT_FREED ? (uint32_t)event->kind : 0u;
    ta_str family = event->identity.family.name;
    ta_str revision = { (const char *)event->identity.revision.ptr, event->identity.revision.len };
    /* Registering and freeing are routine (DEBUG); retiring is a host's decision (INFO). */
    bool retired = event->kind == TA_DEFINITION_EVENT_RETIRED;
    /*
     * Rate-limited per family and event, not per definition: an invoke that
     * compiles new chart text every time registers a new definition for each
     * child, and a per-definition limit would never engage.
     */
    tah_log_line line = { .now = otlp->config.host.now(otlp->config.host.self),
                          .severity = retired ? SEVERITY_INFO : SEVERITY_DEBUG,
                          .severity_text = retired ? LIT("INFO") : LIT("DEBUG"), .event_name = names[kind],
                          .context = &untraced, .definition = tah_log_key(0, family), .key_name = names[kind] };
    otlp->log_body.len = 0;
    bool ok = tah_buf_room(&otlp->log_body, 12u + family.len + 1u + revision.len + 3u + log->origin.len + 1u);
    if (ok) {
        tah_put_bytes(&otlp->log_body, verbs[kind].ptr, verbs[kind].len);
        tah_put_bytes(&otlp->log_body, " ", 1);
        tah_put_bytes(&otlp->log_body, family.ptr, family.len);
        if (revision.len != 0) {
            tah_put_bytes(&otlp->log_body, " ", 1);
            tah_put_bytes(&otlp->log_body, revision.ptr, revision.len);
        }
        tah_put_bytes(&otlp->log_body, " (", 2);
        tah_put_bytes(&otlp->log_body, log->origin.ptr, log->origin.len);
        tah_put_bytes(&otlp->log_body, ")", 1);
    }
    line.body = (ta_str){ (const char *)otlp->log_body.data, (uint32_t)otlp->log_body.len };
    tah_otlp_buf *buf = &otlp->scratch;
    buf->len = 0;
    ok = ok && tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.family"), family) &&
         tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.revision"), revision) &&
         tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.origin"), log->origin) &&
         tah_kv_int(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.id"), (int64_t)event->definition);
    if (ok) {
        /* Read while core still has the definition: not after it is freed. */
        ta_str document = { 0 };
        ta_str source = { 0 };
        tah_definition_facts(otlp, event->definition, &document, &source);
        ok = (document.len == 0 || tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.name"), document)) &&
             (source.len == 0 || tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("tinyactors.definition.source"), source));
    }
    if (!ok || !tah_log_append(otlp, &line)) {
        otlp->dropped += 1u;
    }
}

void tah_otlp_log(tah_otlp *otlp, const ta_log_record *record) {
    if (otlp->config.log_limit == 0) {
        return;
    }
    ta_str text = { 0 };
    if (record->has_value && otlp->config.host.value_text != NULL &&
        !otlp->config.host.value_text(otlp->config.host.self, &record->value, &text, &(bool){ false })) {
        text = (ta_str){ 0 };
    }
    uint64_t now = otlp->config.host.now(otlp->config.host.self);
    /* The body: "label: value", the label, or the value. */
    otlp->log_body.len = 0;
    bool ok = tah_buf_room(&otlp->log_body, record->label.len + 2u + text.len);
    if (ok) {
        tah_put_bytes(&otlp->log_body, record->label.ptr, record->label.len);
        if (record->label.len != 0 && text.len != 0) {
            tah_put_bytes(&otlp->log_body, ": ", 2);
        }
        tah_put_bytes(&otlp->log_body, text.ptr, text.len);
    }
    tah_otlp_buf *buf = &otlp->scratch;
    buf->len = 0;
    char decimal[TAH_SESSION_TEXT];
    ok = ok && tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.session_id"), tah_key_session(otlp, &record->key, decimal));
    if (ok && record->label.len != 0) {
        ok = tah_kv_string(buf, F_LOG_ATTRIBUTES, LIT("scxml.log.label"), record->label);
    }
    tah_log_line line = { .now = now, .severity = SEVERITY_INFO, .severity_text = LIT("INFO"),
                          .event_name = LIT("scxml.log"), .context = &record->context, .key = record->key,
                          .key_name = record->label.len != 0 ? record->label : LIT("scxml.log"),
                          .body = { (const char *)otlp->log_body.data, (uint32_t)otlp->log_body.len } };
    ta_actor_inspection view;
    if (record->source.kind == TA_ACTOR_REF_RUNTIME &&
        ta_actor_inspect((ta_system *)otlp->config.core, record->source.as.runtime, &view) == TA_OK) {
        line.definition = view.definition_id;
    }
    if (!ok || !tah_log_append(otlp, &line)) {
        otlp->dropped += 1u;
    }
}

/* The sizes a logs request's header is made of; its records take `records` bytes. */
typedef struct {
    size_t scope;
    size_t scope_logs;
    size_t resource_logs;
} tah_logs_layout;

static tah_logs_layout tah_logs_layout_for(const tah_otlp *otlp, size_t records) {
    ta_str version = otlp->config.version;
    tah_logs_layout layout;
    layout.scope = tah_len_field_size(F_SCOPE_NAME, sizeof "tinyactors" - 1u) +
                   (version.len != 0 ? tah_len_field_size(F_SCOPE_VERSION, version.len) : 0);
    layout.scope_logs = tah_len_field_size(F_SL_SCOPE, layout.scope) + records;
    layout.resource_logs = tah_len_field_size(F_RL_RESOURCE, otlp->resource.len) +
                           tah_len_field_size(F_RL_SCOPE_LOGS, layout.scope_logs);
    return layout;
}

size_t tah_otlp_logs_size(const tah_otlp *otlp) {
    if (otlp->log_count == 0) {
        return 0;
    }
    return tah_len_field_size(F_REQUEST_RESOURCE_LOGS, tah_logs_layout_for(otlp, otlp->logs.len).resource_logs);
}

size_t tah_otlp_put_logs(tah_otlp *otlp, uint8_t *out, size_t size) {
    if (size == 0 || size != tah_otlp_logs_size(otlp)) {
        return 0;
    }
    ta_str name = LIT("tinyactors");
    ta_str version = otlp->config.version;
    tah_logs_layout layout = tah_logs_layout_for(otlp, otlp->logs.len);
    tah_otlp_buf view = { .data = out, .cap = size };
    tah_put_len_header(&view, F_REQUEST_RESOURCE_LOGS, layout.resource_logs);
    tah_put_len_header(&view, F_RL_RESOURCE, otlp->resource.len);
    tah_put_bytes(&view, otlp->resource.data, otlp->resource.len);
    tah_put_len_header(&view, F_RL_SCOPE_LOGS, layout.scope_logs);
    tah_put_len_header(&view, F_SL_SCOPE, layout.scope);
    tah_put_len_header(&view, F_SCOPE_NAME, name.len);
    tah_put_bytes(&view, name.ptr, name.len);
    if (version.len != 0) {
        tah_put_len_header(&view, F_SCOPE_VERSION, version.len);
        tah_put_bytes(&view, version.ptr, version.len);
    }
    tah_put_bytes(&view, otlp->logs.data, otlp->logs.len);
    otlp->turn_logs += otlp->logs.len;
    otlp->logs.len = 0;
    otlp->log_count = 0;
    return view.len;
}

static bool tah_skipped(tah_otlp *otlp, const ta_trace_record *record);

void tah_otlp_write(tah_otlp *otlp, const ta_trace_record *record) {
    if ((uint32_t)record->kind > (uint32_t)TA_TRACE_WITHDRAWN) {
        otlp->dropped += 1u;
        return;
    }
    /* The records that open a run name the definition's scope from here on. */
    tah_facts_keep(otlp, record);
    /* Definition records (core ADR 0080) have one OTLP path already: the
     * definition sink writes them as the tinyactors.definition.* logs (napi
     * ADR 0017 section 7), with the origin core does not know. A sink that
     * asks for the kinds gets nothing more here, and nothing twice. */
    if (record->kind >= TA_TRACE_DEFINITION_REGISTERED && record->kind <= TA_TRACE_DEFINITION_FREED) {
        return;
    }
    /* Every record carries its own time (core ADR 0098); the encoder reads no clock for it. */
    uint64_t now = tah_record_time(record);
    otlp->last_now = now;
    if (otlp->config.log_limit != 0 && !tah_log_failure(otlp, record, now)) {
        otlp->dropped += 1u;
    }
    if (tah_skipped(otlp, record)) {
        return;
    }
    /* A record of an unsampled trace came for its log only, and a kind the
     * user did not ask for was asked of core for logs or for ending spans. */
    bool sampled = tah_context_empty(&record->context) || (record->context.flags & TA_TRACE_FLAG_SAMPLED) != 0;
    uint32_t wanted = !sampled ? otlp->config.wanted_unsampled_kinds
                      : (record->context.detail & TA_TRACE_DETAIL_WATCHED) != 0 ? otlp->config.wanted_watched_kinds
                                                                               : otlp->config.wanted_kinds;
    bool lifecycle = record->kind <= TA_TRACE_DESTROYED;
    if (!lifecycle && (wanted & TA_TRACE_BIT(record->kind)) == 0) {
        return;
    }
    size_t spans_before = otlp->spans.len;
    uint32_t count_before = otlp->span_count;
    uint32_t entries_before = otlp->entry_count;
    uint32_t runs_before = otlp->run_count;
    size_t scopes_before = otlp->scopes.len;
    bool ok;
    switch (record->kind) {
        case TA_TRACE_SPAWNED:
        case TA_TRACE_LOADED:
        case TA_TRACE_UNLOADED:
        case TA_TRACE_FINISHED:
        case TA_TRACE_DESTROYED:
        case TA_TRACE_WITHDRAWN:
            ok = tah_lifecycle(otlp, record, now);
            break;
        case TA_TRACE_MACROSTEP_BEGIN:
            ok = tah_begin(otlp, record, now);
            break;
        case TA_TRACE_MACROSTEP_END:
            ok = tah_end(otlp, record, now);
            break;
        case TA_TRACE_MICROSTEP:
            ok = tah_microstep(otlp, record, now);
            break;
        default:
            ok = tah_span_event(otlp, record, now);
            break;
    }
    if (!ok) {
        /* A span half-appended to `spans` would corrupt the request. */
        otlp->spans.len = spans_before;
        otlp->span_count = count_before;
        tah_runs_rollback(otlp, entries_before, runs_before, scopes_before);
        otlp->dropped += 1u;
    }
}

/*
 * Records were dropped on tel (core ADR 0098 section 9): every open span
 * ends at the time of the last record read, as an error, with the count as
 * `tinyactors.telemetry.dropped`, so none claims a duration it did not
 * have. The rest of each of their macrosteps is skipped (tah_skipped).
 */
void tah_otlp_dropped(tah_otlp *otlp, uint64_t count) {
    otlp->telemetry_dropped += count;
    while (otlp->open_count != 0) {
        tah_otlp_open *open = &otlp->open[otlp->open_count - 1u];
        if (tah_array_room(otlp->config.allocator, (void **)&otlp->skips, &otlp->skip_capacity, otlp->skip_count,
                           sizeof *otlp->skips, _Alignof(tah_otlp_skip), otlp->skip_count + 1u)) {
            otlp->skips[otlp->skip_count++] = (tah_otlp_skip){ .session = open->session,
                                                                .macrostep = open->macrostep };
        }
        if (!tah_open_end(otlp, open, &(tah_open_end_params){ .now = otlp->last_now, .dropped = count })) {
            otlp->dropped += 1u;
        }
    }
}

/* Whether `record` belongs to a macrostep a drop ended: it starts nothing,
 * and is counted. Its session's next macrostep, or a lifecycle record, ends
 * the skipping. */
static bool tah_skipped(tah_otlp *otlp, const ta_trace_record *record) {
    for (uint32_t at = 0; at < otlp->skip_count; at += 1u) {
        tah_otlp_skip *skip = &otlp->skips[at];
        if (skip->session != record->key.session) {
            continue;
        }
        bool same = record->key.macrostep == skip->macrostep && record->kind != TA_TRACE_MACROSTEP_BEGIN &&
                    record->key.macrostep != 0;
        if (!same || record->kind == TA_TRACE_MACROSTEP_END) {
            otlp->skips[at] = otlp->skips[otlp->skip_count - 1u];
            otlp->skip_count -= 1u;
        }
        if (same) {
            otlp->skipped += 1u;
        }
        return same;
    }
    return false;
}

/* The size of a whole trace request: its resource, then a ScopeSpans per run. */
static size_t tah_request_size(const tah_otlp *otlp) {
    size_t resource_spans = tah_len_field_size(F_RS_RESOURCE, otlp->resource.len) + otlp->scope_spans;
    return tah_len_field_size(F_REQUEST_RESOURCE_SPANS, resource_spans);
}

size_t tah_otlp_traces_size(const tah_otlp *otlp) {
    return otlp->span_count == 0 ? 0 : tah_request_size(otlp);
}

size_t tah_otlp_put_traces(tah_otlp *otlp, uint8_t *out, size_t size) {
    if (size == 0 || size != tah_otlp_traces_size(otlp)) {
        return 0;
    }
    tah_otlp_buf view = { .data = out, .cap = size };
    tah_put_len_header(&view, F_REQUEST_RESOURCE_SPANS,
                       tah_len_field_size(F_RS_RESOURCE, otlp->resource.len) + otlp->scope_spans);
    tah_put_len_header(&view, F_RS_RESOURCE, otlp->resource.len);
    tah_put_bytes(&view, otlp->resource.data, otlp->resource.len);
    for (uint32_t at = 0; at < otlp->run_count; at += 1u) {
        const tah_otlp_run *run = &otlp->runs[at];
        tah_put_len_header(&view, F_RS_SCOPE_SPANS, tah_len_field_size(F_SS_SCOPE, run->scope_len) + run->spans);
        tah_put_len_header(&view, F_SS_SCOPE, run->scope_len);
        tah_put_bytes(&view, otlp->scopes.data + run->scope_at, run->scope_len);
        for (uint32_t index = run->first; index != 0; index = otlp->entries[index - 1u].next) {
            const tah_otlp_entry *entry = &otlp->entries[index - 1u];
            tah_put_bytes(&view, otlp->spans.data + entry->at, entry->len);
        }
    }
    otlp->turn_spans += otlp->spans.len;
    /* Reset: the capacities serve the next turn (tah_otlp_end_turn). */
    otlp->spans.len = 0;
    otlp->span_count = 0;
    otlp->entry_count = 0;
    otlp->run_count = 0;
    otlp->scopes.len = 0;
    otlp->scope_spans = 0;
    if (otlp->run_index_capacity != 0) {
        tah_mem_set(otlp->run_index, 0u, (size_t)otlp->run_index_capacity * sizeof *otlp->run_index);
    }
    return view.len;
}

/* The typical use after a turn that used `used` bytes: idle turns leave it. */
static size_t tah_typical_use(size_t typical, size_t used) {
    if (used == 0) {
        return typical;
    }
    return used >= typical ? typical + (used - typical) / 8u : typical - (typical - used) / 8u;
}

void tah_otlp_end_turn(tah_otlp *otlp) {
    size_t keep = tah_keep_bytes(otlp);
    bool burst = otlp->spans.len == 0 && otlp->spans.cap > keep && otlp->spans.cap / 4u > otlp->typical_spans;
    tah_spans_settle(&otlp->spans, otlp->typical_spans, keep);
    if (burst && otlp->entry_count == 0) {
        /* The entries, runs and scopes of a burst go with its spans. */
        ta_allocator allocator = otlp->config.allocator;
        tah_array_free(allocator, (void **)&otlp->entries, &otlp->entry_capacity, sizeof *otlp->entries,
                       _Alignof(tah_otlp_entry));
        tah_array_free(allocator, (void **)&otlp->runs, &otlp->run_capacity, sizeof *otlp->runs,
                       _Alignof(tah_otlp_run));
        tah_array_free(allocator, (void **)&otlp->run_index, &otlp->run_index_capacity, sizeof *otlp->run_index,
                       _Alignof(uint32_t));
        tah_buf_free(&otlp->scopes);
    }
    tah_buf_settle(&otlp->logs, otlp->typical_logs, tah_keep_bytes(otlp));
    otlp->typical_spans = tah_typical_use(otlp->typical_spans, otlp->turn_spans);
    otlp->typical_logs = tah_typical_use(otlp->typical_logs, otlp->turn_logs);
    otlp->turn_spans = 0;
    otlp->turn_logs = 0;
}

void tah_otlp_end_all(tah_otlp *otlp) {
    uint64_t now = otlp->config.host.now(otlp->config.host.self);
    while (otlp->open_count != 0) {
        if (!tah_open_end(otlp, &otlp->open[otlp->open_count - 1u],
                          &(tah_open_end_params){ .now = now, .incomplete = true })) {
            otlp->dropped += 1u;
        }
    }
}

void tah_otlp_destroy(tah_otlp *otlp) {
    ta_allocator allocator = otlp->config.allocator;
    for (uint32_t at = 0; at < otlp->open_capacity; at += 1u) {
        tah_span_free(&otlp->open[at].macro);
        tah_span_free(&otlp->open[at].micro);
    }
    if (otlp->open != NULL) {
        allocator.v->free(allocator.self,
                          &(ta_allocator_free_params){ .ptr = otlp->open,
                                                       .size = (size_t)otlp->open_capacity * sizeof *otlp->open,
                                                       .alignment = _Alignof(tah_otlp_open) });
    }
    if (otlp->name_slots != NULL) {
        allocator.v->free(allocator.self,
                          &(ta_allocator_free_params){ .ptr = otlp->name_slots,
                                                       .size = (size_t)otlp->name_capacity * sizeof *otlp->name_slots,
                                                       .alignment = _Alignof(tah_otlp_name) });
    }
    tah_open_index_free(otlp);
    tah_span_free(&otlp->lifecycle);
    tah_buf_free(&otlp->logs);
    tah_buf_free(&otlp->log_body);
    tah_buf_free(&otlp->names);
    tah_buf_free(&otlp->resource);
    tah_buf_free(&otlp->spans);
    tah_array_free(allocator, (void **)&otlp->entries, &otlp->entry_capacity, sizeof *otlp->entries,
                   _Alignof(tah_otlp_entry));
    tah_array_free(allocator, (void **)&otlp->runs, &otlp->run_capacity, sizeof *otlp->runs, _Alignof(tah_otlp_run));
    tah_array_free(allocator, (void **)&otlp->run_index, &otlp->run_index_capacity, sizeof *otlp->run_index,
                   _Alignof(uint32_t));
    tah_array_free(allocator, (void **)&otlp->skips, &otlp->skip_capacity, sizeof *otlp->skips,
                   _Alignof(tah_otlp_skip));
    tah_buf_free(&otlp->scopes);
    tah_buf_free(&otlp->scratch);
    tah_buf_free(&otlp->label);
    tah_buf_free(&otlp->text);
    *otlp = (tah_otlp){ 0 };
}

void tah_otlp_set_wanted(tah_otlp *otlp, const ta_trace_masks *masks) {
    otlp->config.wanted_kinds = masks->kinds;
    otlp->config.wanted_watched_kinds = masks->watched_kinds;
    otlp->config.wanted_unsampled_kinds = masks->unsampled_kinds;
}

uint32_t tah_otlp_staged_records(const tah_otlp *otlp) {
    return otlp->span_count + otlp->log_count;
}

bool tah_otlp_logs_enabled(const tah_otlp *otlp) {
    return otlp->config.log_limit != 0;
}

bool tah_otlp_values(const tah_otlp *otlp) {
    return otlp->config.values;
}

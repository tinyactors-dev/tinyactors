/* otlp.c — trace records to OTLP protobuf (tinyactors/otlp.h).
 *
 * Reading a record goes in two halves. First what the record adds is encoded
 * into the scratch area (the attributes and events it gives an open span, a
 * lifecycle span whole, a log record) without changing anything. Then the
 * room it needs is checked: an open-span slot, chunks for what it adds, and
 * staging for the spans it ends. Only if all of it is there does the record
 * change the encoder, so a record is read whole or not at all, and a full
 * staging (TA_OTLP_FLUSH) or a full table (TA_OTLP_GROW) loses nothing. */
#include "otlp/otlp_internal.h"

#include <tinyactors/cbor.h>

#include "base/arena.h"
#include "base/format.h"
#include "base/memory.h"
#include "image/records.h"

#define OTLP_POINTER(otlp, otlp_relative, type)                                                    \
    ((type *)ta_pointer_add((otlp), (otlp)->otlp_relative))

#define OTLP_STRINGIFY_(x) #x
#define OTLP_STRINGIFY(x)  OTLP_STRINGIFY_(x)
#define OTLP_VERSION                                                                               \
    OTLP_STRINGIFY(TA_VERSION_MAJOR)                                                               \
    "." OTLP_STRINGIFY(TA_VERSION_MINOR) "." OTLP_STRINGIFY(TA_VERSION_PATCH)

#define TEXT(literal) ((ta_string_view){(literal), (uint32_t)(sizeof(literal) - 1u)})

enum { SPAN_KIND_INTERNAL = 1, STATUS_CODE_ERROR = 2 };
enum { SEVERITY_INFO = 9, SEVERITY_WARN = 13 };

/* ---- writing protobuf ------------------------------------------------------------------- */

/* A otlp_place to write: the bytes between `at` and `end`. Callers check the room
 * (otlp_room) before writing; the writers below do not. */
typedef struct otlp_out {
    unsigned char *at, *end;
} otlp_out;

static int otlp_room(const otlp_out *out, uint64_t bytes) {
    return (uint64_t)(out->end - out->at) >= bytes;
}

static void otlp_put_varint(otlp_out *out, uint64_t value) {
    while (value >= 0x80u) {
        *out->at++ = (unsigned char)(value | 0x80u);
        value >>= 7;
    }
    *out->at++ = (unsigned char)value;
}

static void otlp_put_tag(otlp_out *out, uint32_t field, uint32_t wire) {
    otlp_put_varint(out, ((uint64_t)field << 3) | wire);
}

static void otlp_put_bytes(otlp_out *out, const void *bytes, uint32_t length) {
    if (length) ta_memcpy(out->at, bytes, length);
    out->at += length;
}

static void otlp_put_fixed64(otlp_out *out, uint64_t value) {
    for (int i = 0; i < 8; i++)
        *out->at++ = (unsigned char)(value >> (8 * i));
}

/* IDs are big-endian bytes, so their hex form reads the same. */
static void otlp_put_id64(otlp_out *out, uint64_t value) {
    for (int i = 7; i >= 0; i--)
        *out->at++ = (unsigned char)(value >> (8 * i));
}

static void otlp_put_length(otlp_out *out, uint32_t field, uint32_t length) {
    otlp_put_tag(out, field, WIRE_LENGTH);
    otlp_put_varint(out, length);
}

/* ---- attributes: KeyValue { key, AnyValue } --------------------------------------------- */

static uint32_t otlp_any_string_size(uint32_t length) {
    return otlp_field_size(F_ANY_STRING, length);
}
static uint32_t otlp_any_int_size(int64_t value) {
    return 1u + otlp_varint_size((uint64_t)value);
}
static uint32_t otlp_key_value_size(ta_string_view key, uint32_t any) {
    return otlp_field_size(F_KEY_VALUE_KEY, key.length) + otlp_field_size(F_KEY_VALUE_VALUE, any);
}

/* The head of an attribute (`field` = the repeated attributes field) whose
 * AnyValue is `any` bytes; 0 when it does not fit, after counting it. */
static int otlp_put_key_value_head(ta_otlp *otlp, otlp_out *out, uint32_t field, ta_string_view key,
                                   uint32_t any) {
    uint32_t size = otlp_key_value_size(key, any);
    if (!otlp_room(out, otlp_field_size(field, size))) {
        otlp->truncated++;
        return 0;
    }
    otlp_put_length(out, field, size);
    otlp_put_length(out, F_KEY_VALUE_KEY, key.length);
    otlp_put_bytes(out, key.pointer, key.length);
    otlp_put_length(out, F_KEY_VALUE_VALUE, any);
    return 1;
}

static void otlp_kv_string(ta_otlp *otlp, otlp_out *out, uint32_t field, ta_string_view key,
                           ta_string_view value) {
    if (!otlp_put_key_value_head(otlp, out, field, key, otlp_any_string_size(value.length))) return;
    otlp_put_length(out, F_ANY_STRING, value.length);
    otlp_put_bytes(out, value.pointer, value.length);
}

static void otlp_kv_int(ta_otlp *otlp, otlp_out *out, uint32_t field, ta_string_view key,
                        int64_t value) {
    if (!otlp_put_key_value_head(otlp, out, field, key, otlp_any_int_size(value))) return;
    otlp_put_tag(out, F_ANY_INT, WIRE_VARINT);
    otlp_put_varint(out, (uint64_t)value);
}

static void otlp_kv_bool(ta_otlp *otlp, otlp_out *out, uint32_t field, ta_string_view key,
                         int value) {
    if (!otlp_put_key_value_head(otlp, out, field, key, 2u)) return;
    otlp_put_tag(out, F_ANY_BOOL, WIRE_VARINT);
    otlp_put_varint(out, value ? 1u : 0u);
}

/* An array of integers. */
static void otlp_kv_ints(ta_otlp *otlp, otlp_out *out, uint32_t field, ta_string_view key,
                         const uint32_t *values, uint32_t count) {
    uint32_t array = 0;
    for (uint32_t i = 0; i < count; i++)
        array += otlp_field_size(F_ARRAY_VALUES, otlp_any_int_size(values[i]));
    if (!otlp_put_key_value_head(otlp, out, field, key, otlp_field_size(F_ANY_ARRAY, array)))
        return;
    otlp_put_length(out, F_ANY_ARRAY, array);
    for (uint32_t i = 0; i < count; i++) {
        otlp_put_length(out, F_ARRAY_VALUES, otlp_any_int_size(values[i]));
        otlp_put_tag(out, F_ANY_INT, WIRE_VARINT);
        otlp_put_varint(out, values[i]);
    }
}

/* ---- names from the image ---------------------------------------------------------------- */

/* What one record is read with. */
typedef struct otlp_reading {
    ta_otlp *otlp;
    const ta_otlp_input *input;
    const ta_image_view *view;
} otlp_reading;

static ta_string_view otlp_reading_string(const otlp_reading *reading, uint32_t reference) {
    ta_string_view none = {"", 0};
    if (reference == TA_NIL) return none;
    if (reference & TA_TRACE_STRING_DYNAMIC)
        return ta_trace_dynamic_string(&reading->input->view, reference);
    return ta_image_string(reading->input->image, reference);
}

static ta_string_view otlp_state_name(const otlp_reading *reading, uint32_t chart, uint32_t state) {
    return ta_image_state_name(reading->input->image, chart, state);
}

/* An array of a chart's state names. */
static void otlp_kv_states(const otlp_reading *reading, otlp_out *out, uint32_t field,
                           ta_string_view key, uint32_t chart, const uint32_t *states,
                           uint32_t count) {
    uint32_t array = 0;
    for (uint32_t i = 0; i < count; i++)
        array += otlp_field_size(
            F_ARRAY_VALUES,
            otlp_any_string_size(otlp_state_name(reading, chart, states[i]).length));
    if (!otlp_put_key_value_head(reading->otlp, out, field, key,
                                 otlp_field_size(F_ANY_ARRAY, array)))
        return;
    otlp_put_length(out, F_ANY_ARRAY, array);
    for (uint32_t i = 0; i < count; i++) {
        ta_string_view name = otlp_state_name(reading, chart, states[i]);
        otlp_put_length(out, F_ARRAY_VALUES, otlp_any_string_size(name.length));
        otlp_put_length(out, F_ANY_STRING, name.length);
        otlp_put_bytes(out, name.pointer, name.length);
    }
}

/* A transition as text: "a → b on e", "a on e" (targetless), "a → b"
 * (eventless); targets and events separated by spaces. */
static uint32_t otlp_transition_text(const otlp_reading *reading, uint32_t chart,
                                     uint32_t transition, char *buffer, uint32_t capacity) {
    const ta_image_view *view = reading->view;
    ta_text text = ta_text_init(buffer, capacity);
    if (chart >= view->chart_count || transition >= view->charts[chart].transition_count) return 0;
    const ta_transition_record *record =
        &ta_chart_transitions(view, &view->charts[chart])[transition];
    ta_text_append(&text, otlp_state_name(reading, chart, record->source));
    for (uint32_t i = 0; i < record->target_count; i++) {
        ta_text_append(&text, i ? TEXT(" ") : TEXT(" \xe2\x86\x92 "));
        ta_text_append(&text,
                       otlp_state_name(reading, chart, view->u32[record->targets_offset + i]));
    }
    for (uint32_t i = 0; i < record->event_count; i++) {
        ta_text_append(&text, i ? TEXT(" ") : TEXT(" on "));
        ta_text_append(&text, ta_image_event_name(reading->input->image,
                                                  view->u32[record->events_offset + 2 * i]));
    }
    return text.overflow ? 0 : text.length;
}

static void otlp_kv_transitions(const otlp_reading *reading, otlp_out *out, uint32_t chart,
                                const uint32_t *transitions, uint32_t count) {
    char text[512];
    uint32_t array = 0;
    for (uint32_t i = 0; i < count; i++)
        array += otlp_field_size(F_ARRAY_VALUES,
                                 otlp_any_string_size(otlp_transition_text(
                                     reading, chart, transitions[i], text, sizeof text)));
    if (otlp_put_key_value_head(reading->otlp, out, F_SPAN_ATTRIBUTES, TEXT("scxml.transition"),
                                otlp_field_size(F_ANY_ARRAY, array))) {
        otlp_put_length(out, F_ANY_ARRAY, array);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t length =
                otlp_transition_text(reading, chart, transitions[i], text, sizeof text);
            otlp_put_length(out, F_ARRAY_VALUES, otlp_any_string_size(length));
            otlp_put_length(out, F_ANY_STRING, length);
            otlp_put_bytes(out, text, length);
        }
    }
    otlp_kv_ints(reading->otlp, out, F_SPAN_ATTRIBUTES, TEXT("scxml.transition.id"), transitions,
                 count);
}

/* A value as text: what the datamodel's to_wire writes, in CBOR diagnostic
 * notation (ta_cbor_diagnostic), cut at value_limit (at a character
 * boundary). Returns its length, *cut = 1 if it was cut; 0 without a value
 * or a datamodel that can write it. */
static uint32_t otlp_render_value(const otlp_reading *reading, uint32_t datamodel, uint64_t payload,
                                  const char **text, int *cut) {
    ta_otlp *otlp = reading->otlp;
    const ta_otlp_input *input = reading->input;
    *cut = 0;
    if (!payload || datamodel >= input->datamodel_count || !input->datamodels) return 0;
    const ta_datamodel_vtable *vtable = input->datamodels[datamodel].vtable;
    if (vtable->size < offsetof(ta_datamodel_vtable, to_wire) + sizeof vtable->to_wire ||
        !vtable->to_wire)
        return 0;
    ta_buffer cbor = {OTLP_POINTER(otlp, render_relative, char), otlp->config.render_bytes, 0};
    ta_status status = vtable->to_wire(input->datamodels[datamodel].datamodel, payload, &cbor);
    if (status != TA_OK || cbor.length > cbor.capacity) {
        if (status == TA_ERROR_NO_MEMORY) *cut = 1; /* too big to write at all */
        return 0;
    }
    char *rendered = OTLP_POINTER(otlp, text_relative, char);
    ta_buffer out = {rendered, otlp->config.value_limit, 0};
    status = ta_cbor_diagnostic((ta_bytes){cbor.pointer, cbor.length}, &out);
    if (status != TA_OK && status != TA_ERROR_NO_MEMORY) return 0;
    *text = rendered;
    size_t length = out.length;
    if (status == TA_ERROR_NO_MEMORY) {
        length = otlp->config.value_limit;
        while (length && ((unsigned char)rendered[length] & 0xC0u) == 0x80u) /* not mid-character */
            length--;
        *cut = 1;
    }
    return (uint32_t)length;
}

static void otlp_kv_value(const otlp_reading *reading, otlp_out *out, uint32_t field,
                          ta_string_view key, ta_string_view cut_key, uint32_t datamodel,
                          uint64_t payload) {
    if (!(reading->otlp->config.flags & TA_OTLP_VALUES)) return;
    const char *text = "";
    int cut = 0;
    uint32_t length = otlp_render_value(reading, datamodel, payload, &text, &cut);
    if (length) otlp_kv_string(reading->otlp, out, field, key, (ta_string_view){text, length});
    if (cut) otlp_kv_bool(reading->otlp, out, field, cut_key, 1);
}

/* ---- time --------------------------------------------------------------------------------- */

void ta_otlp_mark(ta_otlp *otlp, uint64_t fuel, uint64_t wall_ns) {
    if (otlp->mark_count) {
        uint32_t newest = (otlp->mark_head + OTLP_MARKS - 1u) % OTLP_MARKS;
        if (fuel < otlp->mark_fuel[newest]) fuel = otlp->mark_fuel[newest]; /* never back */
        if (wall_ns < otlp->mark_wall[newest]) wall_ns = otlp->mark_wall[newest];
    }
    otlp->mark_fuel[otlp->mark_head] = fuel;
    otlp->mark_wall[otlp->mark_head] = wall_ns;
    otlp->mark_head = (otlp->mark_head + 1u) % OTLP_MARKS;
    if (otlp->mark_count < OTLP_MARKS) otlp->mark_count++;
}

static uint32_t otlp_bits(uint64_t value) {
    uint32_t bits = 0;
    while (value) {
        bits++;
        value >>= 1;
    }
    return bits;
}

/* a * b / c for a <= c without overflowing 64 bits: a and c lose their low
 * bits together until the product fits (no 128-bit arithmetic, which a
 * freestanding target may not have) */
static uint64_t otlp_scale(uint64_t a, uint64_t b, uint64_t c) {
    while (otlp_bits(a) + otlp_bits(b) > 64) {
        a >>= 1;
        c >>= 1;
    }
    return c ? a * b / c : b;
}

/* The wall time of a fuel stamp: interpolated between the marks around it.
 * A run's records are stamped after its first mark (a run moves the clock
 * on before any work), so a stamp belongs to the newest interval that
 * starts strictly below it, and a stamp equal to a run's last mark (its
 * last record, or one written between runs) to that run. */
static uint64_t otlp_wall_of(const ta_otlp *otlp, uint64_t fuel) {
    if (!otlp->mark_count) return 0;
    for (uint32_t k = 0; k < otlp->mark_count; k++) {
        uint32_t i = (otlp->mark_head + OTLP_MARKS - 1u - k) % OTLP_MARKS;
        if (otlp->mark_fuel[i] >= fuel) continue;
        if (k == 0) return otlp->mark_wall[i]; /* past the newest mark */
        uint32_t next = (i + 1u) % OTLP_MARKS;
        uint64_t span = otlp->mark_fuel[next] - otlp->mark_fuel[i];
        return otlp->mark_wall[i] + otlp_scale(fuel - otlp->mark_fuel[i],
                                               otlp->mark_wall[next] - otlp->mark_wall[i], span);
    }
    return otlp->mark_wall[(otlp->mark_head + OTLP_MARKS - otlp->mark_count) % OTLP_MARKS];
}

/* ---- chunks: what open spans gather --------------------------------------------------------- */

static otlp_chunk *otlp_chunk_at(const ta_otlp *otlp, uint32_t index) {
    return &OTLP_POINTER(otlp, chunks_relative, otlp_chunk)[index];
}

static uint32_t otlp_chunks_free(const ta_otlp *otlp) {
    return otlp->config.chunks - otlp->chunks_used;
}

/* Chunks that appending `bytes` to `chain` takes. */
static uint32_t otlp_chunks_for(const ta_otlp *otlp, const otlp_chain *chain, uint32_t bytes) {
    uint32_t room =
        chain->last == TA_NIL ? 0 : OTLP_CHUNK_PAYLOAD - otlp_chunk_at(otlp, chain->last)->used;
    if (bytes <= room) return 0;
    return (bytes - room + OTLP_CHUNK_PAYLOAD - 1u) / OTLP_CHUNK_PAYLOAD;
}

static uint32_t otlp_chunk_take(ta_otlp *otlp) {
    uint32_t index = otlp->chunk_free;
    if (index != TA_NIL)
        otlp->chunk_free = otlp_chunk_at(otlp, index)->next;
    else
        index = otlp->chunk_high++;
    otlp_chunk *chunk = otlp_chunk_at(otlp, index);
    chunk->next = TA_NIL;
    chunk->used = 0;
    otlp->chunks_used++;
    return index;
}

static void otlp_chain_init(otlp_chain *chain) {
    chain->first = chain->last = TA_NIL;
    chain->bytes = 0;
    chain->pad = 0;
}

/* Append (the room was checked with otlp_chunks_for). */
static void otlp_chain_append(ta_otlp *otlp, otlp_chain *chain, const unsigned char *bytes,
                              uint32_t length) {
    while (length) {
        if (chain->last == TA_NIL || otlp_chunk_at(otlp, chain->last)->used == OTLP_CHUNK_PAYLOAD) {
            uint32_t index = otlp_chunk_take(otlp);
            if (chain->last == TA_NIL)
                chain->first = index;
            else
                otlp_chunk_at(otlp, chain->last)->next = index;
            chain->last = index;
        }
        otlp_chunk *chunk = otlp_chunk_at(otlp, chain->last);
        uint32_t n = OTLP_CHUNK_PAYLOAD - chunk->used;
        if (n > length) n = length;
        ta_memcpy(chunk->data + chunk->used, bytes, n);
        chunk->used += n;
        chain->bytes += n;
        bytes += n;
        length -= n;
    }
}

static void otlp_chain_write(const ta_otlp *otlp, const otlp_chain *chain, otlp_out *out) {
    for (uint32_t index = chain->first; index != TA_NIL; index = otlp_chunk_at(otlp, index)->next)
        otlp_put_bytes(out, otlp_chunk_at(otlp, index)->data, otlp_chunk_at(otlp, index)->used);
}

static void otlp_chain_free(ta_otlp *otlp, otlp_chain *chain) {
    for (uint32_t index = chain->first; index != TA_NIL;) {
        otlp_chunk *chunk = otlp_chunk_at(otlp, index);
        uint32_t next = chunk->next;
        chunk->next = otlp->chunk_free;
        chunk->used = 0;
        otlp->chunk_free = index;
        otlp->chunks_used--;
        index = next;
    }
    otlp_chain_init(chain);
}

/* ---- open spans: a table by session ----------------------------------------------------- */

static otlp_open *otlp_open_at(const ta_otlp *otlp, uint32_t slot) {
    return &OTLP_POINTER(otlp, open_relative, otlp_open)[slot];
}

static uint32_t *otlp_index_of(const ta_otlp *otlp) {
    return OTLP_POINTER(otlp, index_relative, uint32_t);
}

static uint32_t otlp_session_hash(uint64_t session, uint32_t capacity) {
    return (uint32_t)((session * 0x9E3779B97F4A7C15ull) >> 32) & (capacity - 1u);
}

static otlp_open *otlp_open_find(const ta_otlp *otlp, uint64_t session) {
    const uint32_t *index = otlp_index_of(otlp);
    for (uint32_t i = otlp_session_hash(session, otlp->index_capacity);;
         i = (i + 1u) & (otlp->index_capacity - 1u)) {
        if (!index[i]) return (otlp_open *)0;
        otlp_open *open = otlp_open_at(otlp, index[i] - 1u);
        if (open->session == session) return open;
    }
}

static otlp_open *otlp_open_add(ta_otlp *otlp, uint64_t session, uint32_t source, uint32_t chart) {
    uint32_t slot = 0;
    while (otlp_open_at(otlp, slot)->used)
        slot++;
    otlp_open *open = otlp_open_at(otlp, slot);
    ta_memset(open, 0, sizeof *open);
    open->used = 1;
    open->session = session;
    open->source = source;
    open->chart = chart;
    otlp_chain_init(&open->common);
    otlp_chain_init(&open->macro.fields);
    otlp_chain_init(&open->micro.fields);
    uint32_t *index = otlp_index_of(otlp);
    uint32_t i = otlp_session_hash(session, otlp->index_capacity);
    while (index[i])
        i = (i + 1u) & (otlp->index_capacity - 1u);
    index[i] = slot + 1u;
    otlp->open_count++;
    return open;
}

/* Remove from the index by backward shifting (no tombstones), and free. */
static void otlp_open_remove(ta_otlp *otlp, otlp_open *open) {
    uint32_t *index = otlp_index_of(otlp), mask = otlp->index_capacity - 1u;
    uint32_t slot = (uint32_t)(open - otlp_open_at(otlp, 0)) + 1u;
    uint32_t i = otlp_session_hash(open->session, otlp->index_capacity);
    while (index[i] != slot)
        i = (i + 1u) & mask;
    index[i] = 0;
    for (uint32_t j = (i + 1u) & mask; index[j]; j = (j + 1u) & mask) {
        uint32_t home =
            otlp_session_hash(otlp_open_at(otlp, index[j] - 1u)->session, otlp->index_capacity);
        /* move j back into i when its home is not in (i, j] */
        if (((j - home) & mask) >= ((j - i) & mask)) {
            index[i] = index[j];
            index[j] = 0;
            i = j;
        }
    }
    otlp_chain_free(otlp, &open->common);
    otlp_chain_free(otlp, &open->macro.fields);
    otlp_chain_free(otlp, &open->micro.fields);
    ta_memset(open, 0, sizeof *open);
    otlp->open_count--;
}

/* ---- the staging: finished spans, by scope ----------------------------------------------------
 */

static otlp_scope *otlp_scope_at(const ta_otlp *otlp, uint32_t i) {
    return &OTLP_POINTER(otlp, scopes_relative, otlp_scope)[i];
}

static otlp_entry *otlp_entry_at(const ta_otlp *otlp, uint32_t i) {
    return &OTLP_POINTER(otlp, entries_relative, otlp_entry)[i];
}

static otlp_scope *otlp_scope_find(const ta_otlp *otlp, uint32_t source, uint32_t chart) {
    for (uint32_t i = 0; i < otlp->scope_count; i++)
        if (otlp_scope_at(otlp, i)->source == source && otlp_scope_at(otlp, i)->chart == chart)
            return otlp_scope_at(otlp, i);
    return (otlp_scope *)0;
}

/* The scope of (source, chart), begun when it has none yet; its
 * InstrumentationScope names the chart (napi ADR 0028). The room was
 * checked: a scope slot and OTLP_SCOPE_BYTES. */
static otlp_scope *otlp_scope_for(const otlp_reading *reading, uint32_t source, uint32_t chart) {
    ta_otlp *otlp = reading->otlp;
    otlp_scope *scope = otlp_scope_find(otlp, source, chart);
    if (scope) return scope;
    scope = otlp_scope_at(otlp, otlp->scope_count++);
    ta_memset(scope, 0, sizeof *scope);
    scope->source = source;
    scope->chart = chart;
    scope->bytes_at = otlp->scope_bytes_used;
    unsigned char *bytes = OTLP_POINTER(otlp, scope_bytes_relative, unsigned char);
    otlp_out out = {bytes + otlp->scope_bytes_used,
                    bytes + otlp->scope_bytes_used + OTLP_SCOPE_BYTES};
    ta_string_view name = TEXT("tinyactors"), version = TEXT(OTLP_VERSION);
    otlp_put_length(&out, F_SCOPE_NAME, name.length);
    otlp_put_bytes(&out, name.pointer, name.length);
    otlp_put_length(&out, F_SCOPE_VERSION, version.length);
    otlp_put_bytes(&out, version.pointer, version.length);
    otlp_kv_string(otlp, &out, F_SCOPE_ATTRIBUTES, TEXT("tinyactors.chart.name"),
                   ta_image_chart_name(reading->input->image, chart));
    otlp_kv_string(otlp, &out, F_SCOPE_ATTRIBUTES, TEXT("tinyactors.chart.source"),
                   ta_image_chart_source(reading->input->image, chart));
    otlp_kv_int(otlp, &out, F_SCOPE_ATTRIBUTES, TEXT("tinyactors.chart.index"), chart);
    otlp_kv_int(otlp, &out, F_SCOPE_ATTRIBUTES, TEXT("tinyactors.system"), source);
    scope->bytes_length = (uint32_t)(out.at - (bytes + otlp->scope_bytes_used));
    otlp->scope_bytes_used += OTLP_SCOPE_BYTES;
    return scope;
}

/* ---- staging a span ------------------------------------------------------------------------ */

/* Encoded attribute bytes written after a span's chain: what ending it adds. */
typedef struct otlp_bytes {
    const unsigned char *bytes;
    uint32_t length;
    uint32_t pad;
} otlp_bytes;

static uint32_t otlp_span_body_size(const otlp_span *span, const otlp_chain *common, uint32_t extra,
                                    ta_string_view kind) {
    uint32_t body =
        otlp_field_size(F_SPAN_TRACE_ID, 16) + otlp_field_size(F_SPAN_SPAN_ID, 8) +
        (span->parent_id ? otlp_field_size(F_SPAN_PARENT_SPAN_ID, 8) : 0) + 2u /* kind */ + 9u +
        9u /* start, end */ + 6u /* flags: field 16 takes a 2-byte tag */ +
        (common ? common->bytes : 0) + span->fields.bytes + extra +
        otlp_field_size(F_SPAN_ATTRIBUTES, otlp_key_value_size(TEXT("tinyactors.span.kind"),
                                                               otlp_any_string_size(kind.length))) +
        (span->error ? otlp_field_size(F_SPAN_STATUS, 2) : 0);
    if (span->link_id)
        body += otlp_field_size(F_SPAN_LINKS, otlp_field_size(F_LINK_TRACE_ID, 16) +
                                                  otlp_field_size(F_LINK_SPAN_ID, 8));
    return body;
}

static uint32_t otlp_span_entry_size(const otlp_span *span, const otlp_chain *common,
                                     uint32_t extra, ta_string_view kind) {
    return otlp_field_size(F_SS_ITEMS, otlp_span_body_size(span, common, extra, kind));
}

/* Write a finished span into the staging, in its scope (the room was
 * checked), ending at `end`, at least a nanosecond after its start. */
static void otlp_span_stage(const otlp_reading *reading, const otlp_span *span,
                            const otlp_chain *common, otlp_bytes extra, ta_string_view kind,
                            uint32_t source, uint32_t chart, uint64_t end) {
    ta_otlp *otlp = reading->otlp;
    otlp_scope *scope = otlp_scope_for(reading, source, chart);
    unsigned char *spans = OTLP_POINTER(otlp, spans_relative, unsigned char);
    otlp_out out = {spans + otlp->spans_used, spans + otlp->config.staging_bytes};
    unsigned char *begin = out.at;
    otlp_put_length(&out, F_SS_ITEMS, otlp_span_body_size(span, common, extra.length, kind));
    otlp_put_length(&out, F_SPAN_TRACE_ID, 16);
    otlp_put_id64(&out, span->trace.high);
    otlp_put_id64(&out, span->trace.low);
    otlp_put_length(&out, F_SPAN_SPAN_ID, 8);
    otlp_put_id64(&out, span->span_id);
    if (span->parent_id) {
        otlp_put_length(&out, F_SPAN_PARENT_SPAN_ID, 8);
        otlp_put_id64(&out, span->parent_id);
    }
    otlp_put_tag(&out, F_SPAN_KIND, WIRE_VARINT);
    otlp_put_varint(&out, SPAN_KIND_INTERNAL);
    otlp_put_tag(&out, F_SPAN_START, WIRE_I64);
    otlp_put_fixed64(&out, span->start);
    otlp_put_tag(&out, F_SPAN_END, WIRE_I64);
    otlp_put_fixed64(&out, end > span->start ? end : span->start + 1u);
    /* Span.flags: the W3C flags, and "the parent is not remote" known (bit 8) */
    otlp_put_tag(&out, F_SPAN_FLAGS, WIRE_I32);
    uint32_t flags = (span->flags & 0xFFu) | 0x100u;
    for (int i = 0; i < 4; i++)
        *out.at++ = (unsigned char)(flags >> (8 * i));
    if (common) otlp_chain_write(otlp, common, &out);
    otlp_chain_write(otlp, &span->fields, &out);
    otlp_put_bytes(&out, extra.bytes, extra.length);
    otlp_kv_string(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("tinyactors.span.kind"), kind);
    if (span->link_id) {
        otlp_put_length(&out, F_SPAN_LINKS,
                        otlp_field_size(F_LINK_TRACE_ID, 16) + otlp_field_size(F_LINK_SPAN_ID, 8));
        otlp_put_length(&out, F_LINK_TRACE_ID, 16);
        otlp_put_id64(&out, span->trace.high);
        otlp_put_id64(&out, span->trace.low);
        otlp_put_length(&out, F_LINK_SPAN_ID, 8);
        otlp_put_id64(&out, span->link_id);
    }
    if (span->error) {
        otlp_put_length(&out, F_SPAN_STATUS, 2);
        otlp_put_tag(&out, F_STATUS_CODE, WIRE_VARINT);
        otlp_put_varint(&out, STATUS_CODE_ERROR);
    }
    uint32_t length = (uint32_t)(out.at - begin);
    TA_ASSERT(length == otlp_span_entry_size(span, common, extra.length, kind));
    otlp_entry *entry = otlp_entry_at(otlp, otlp->entry_count++);
    entry->at = otlp->spans_used;
    entry->length = length;
    entry->next = 0;
    if (scope->last)
        otlp_entry_at(otlp, scope->last - 1u)->next = otlp->entry_count;
    else
        scope->first = otlp->entry_count;
    scope->last = otlp->entry_count;
    scope->spans += length;
    otlp->spans_used += length;
    otlp->span_count++;
}

/* ---- what one record needs ------------------------------------------------------------------ */

typedef struct otlp_needs {
    uint32_t open; /* slots */
    uint32_t chunks;
    uint32_t staging; /* bytes */
    uint32_t entries;
    uint32_t scopes; /* charts not in the request yet */
    uint32_t logs;   /* bytes */
    uint32_t skips;
    uint32_t pad;
} otlp_needs;

/* TA_OTLP_READ when the needs fit, else what the host must do: flush, when
 * the staging has something to flush; grow otherwise. */
static uint32_t otlp_needs_check(const ta_otlp *otlp, const otlp_needs *needs) {
    if (otlp->open_count + needs->open > otlp->config.open_spans) return TA_OTLP_GROW;
    if (needs->chunks > otlp_chunks_free(otlp)) return TA_OTLP_GROW;
    if (needs->skips > otlp->config.open_spans - otlp->skip_count) return TA_OTLP_GROW;
    if (needs->staging > otlp->config.staging_bytes - otlp->spans_used ||
        needs->entries > otlp->entry_capacity - otlp->entry_count ||
        needs->scopes > otlp->config.scopes - otlp->scope_count ||
        needs->logs > otlp->config.log_bytes - otlp->logs_used)
        return otlp->spans_used || otlp->logs_used ? TA_OTLP_FLUSH : TA_OTLP_GROW;
    return TA_OTLP_READ;
}

/* A span of (source, chart) that the record ends. Scopes are counted apart
 * (otlp_needs_scope): a record's spans are all of one chart. */
static void otlp_needs_span(otlp_needs *needs, const otlp_span *span, const otlp_chain *common,
                            uint32_t extra, ta_string_view kind) {
    needs->staging += otlp_span_entry_size(span, common, extra, kind);
    needs->entries++;
}

static void otlp_needs_scope(const ta_otlp *otlp, otlp_needs *needs, uint32_t source,
                             uint32_t chart) {
    if (!otlp_scope_find(otlp, source, chart)) needs->scopes = 1;
}

/* ---- the scratch: where a record's bytes are put together ------------------------------------ */

typedef struct otlp_scratch {
    unsigned char *base;
    uint32_t capacity, used;
} otlp_scratch;

static otlp_out otlp_scratch_part(otlp_scratch *scratch) {
    otlp_out out = {scratch->base + scratch->used, scratch->base + scratch->capacity};
    return out;
}

/* What was written into a part since otlp_scratch_part. */
static otlp_bytes otlp_scratch_close(otlp_scratch *scratch, const otlp_out *out) {
    otlp_bytes bytes = {scratch->base + scratch->used,
                        (uint32_t)(out->at - (scratch->base + scratch->used)), 0};
    scratch->used += bytes.length;
    return bytes;
}

/* ---- the attributes of an actor's spans --------------------------------------------------- */

/* scxml.session_id, scxml.actor, scxml.definition, scxml.macrostep,
 * scxml.time, tinyactors.actor.name and tinyactors.chart.name. */
static void otlp_common_attributes(const otlp_reading *reading, otlp_out *out,
                                   const ta_trace_record *record, uint32_t actor_name) {
    ta_otlp *otlp = reading->otlp;
    char actor[2 * TA_FORMAT_U64_MAX + 2];
    ta_text text = ta_text_init(actor, sizeof actor);
    ta_text_append_u64(&text, (record->actor & 0xFFFFFFu) - 1u);
    ta_text_append(&text, TEXT(":"));
    ta_text_append_u64(&text, record->actor >> 24);
    otlp_kv_int(otlp, out, F_SPAN_ATTRIBUTES, TEXT("scxml.session_id"),
                (int64_t)record->key.session);
    otlp_kv_string(otlp, out, F_SPAN_ATTRIBUTES, TEXT("scxml.actor"),
                   (ta_string_view){actor, text.length});
    otlp_kv_int(otlp, out, F_SPAN_ATTRIBUTES, TEXT("scxml.definition"), record->chart);
    otlp_kv_int(otlp, out, F_SPAN_ATTRIBUTES, TEXT("scxml.macrostep"),
                (int64_t)record->key.macrostep);
    otlp_kv_int(otlp, out, F_SPAN_ATTRIBUTES, TEXT("scxml.time"), (int64_t)record->now);
    if (actor_name != TA_NIL)
        otlp_kv_string(otlp, out, F_SPAN_ATTRIBUTES, TEXT("tinyactors.actor.name"),
                       otlp_reading_string(reading, actor_name));
    otlp_kv_string(otlp, out, F_SPAN_ATTRIBUTES, TEXT("tinyactors.chart.name"),
                   ta_image_chart_name(reading->input->image, record->chart));
}

/* A span's name field, if it fits. */
static void otlp_put_name(otlp_out *out, ta_string_view prefix, ta_string_view rest) {
    if (!otlp_room(out, otlp_field_size(F_SPAN_NAME, prefix.length + rest.length))) return;
    otlp_put_length(out, F_SPAN_NAME, prefix.length + rest.length);
    otlp_put_bytes(out, prefix.pointer, prefix.length);
    otlp_put_bytes(out, rest.pointer, rest.length);
}

static ta_string_view otlp_event_type_name(uint32_t type) {
    return type == TA_EVENT_TYPE_EXTERNAL   ? TEXT("external")
           : type == TA_EVENT_TYPE_INTERNAL ? TEXT("internal")
                                            : TEXT("platform");
}

/* ---- span events and log records -------------------------------------------------------- */

typedef void (*otlp_attributes)(const otlp_reading *, otlp_out *, const ta_trace_record *);

/* A Span.events field into the scratch: name, time and attributes. Empty if
 * it does not fit. */
static otlp_bytes otlp_event_encode(const otlp_reading *reading, otlp_scratch *scratch,
                                    ta_string_view name, uint64_t time, otlp_attributes attributes,
                                    const ta_trace_record *record) {
    otlp_bytes none = {(const unsigned char *)0, 0, 0};
    otlp_out tail = otlp_scratch_part(scratch);
    uint32_t head = 6u + 9u + otlp_field_size(F_EVENT_NAME, name.length); /* tag + length <= 6 */
    if (!otlp_room(&tail, head)) return none;
    unsigned char *begin = tail.at;
    tail.at += head;
    unsigned char *fields = tail.at;
    attributes(reading, &tail, record);
    uint32_t attributes_length = (uint32_t)(tail.at - fields);
    otlp_out out = {begin, fields};
    otlp_put_length(&out, F_SPAN_EVENTS,
                    9u + otlp_field_size(F_EVENT_NAME, name.length) + attributes_length);
    otlp_put_tag(&out, F_EVENT_TIME, WIRE_I64);
    otlp_put_fixed64(&out, time);
    otlp_put_length(&out, F_EVENT_NAME, name.length);
    otlp_put_bytes(&out, name.pointer, name.length);
    ta_memmove(out.at, fields, attributes_length);
    out.at += attributes_length;
    return otlp_scratch_close(scratch, &out);
}

static void otlp_condition_attributes(const otlp_reading *reading, otlp_out *out,
                                      const ta_trace_record *record) {
    const ta_trace_condition *condition = (const ta_trace_condition *)ta_trace_body(record);
    char text[512];
    uint32_t length =
        otlp_transition_text(reading, record->chart, condition->transition, text, sizeof text);
    otlp_kv_string(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.transition"),
                   (ta_string_view){text, length});
    otlp_kv_int(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.transition.id"),
                condition->transition);
    otlp_kv_bool(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.result"),
                 (int)condition->result);
}

static void otlp_raised_attributes(const otlp_reading *reading, otlp_out *out,
                                   const ta_trace_record *record) {
    const ta_trace_raised *raised = (const ta_trace_raised *)ta_trace_body(record);
    otlp_kv_string(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.event.name"),
                   otlp_reading_string(reading, raised->name));
}

static void otlp_sent_attributes(const otlp_reading *reading, otlp_out *out,
                                 const ta_trace_record *record) {
    ta_otlp *otlp = reading->otlp;
    const ta_trace_sent *sent = (const ta_trace_sent *)ta_trace_body(record);
    otlp_kv_string(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.event.name"),
                   otlp_reading_string(reading, sent->name));
    if (sent->target != TA_NIL)
        otlp_kv_string(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.send.target"),
                       otlp_reading_string(reading, sent->target));
    if (sent->sendid != TA_NIL)
        otlp_kv_string(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.event.sendid"),
                       otlp_reading_string(reading, sent->sendid));
    if (sent->delay)
        otlp_kv_int(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.send.delay_ns"),
                    (int64_t)sent->delay);
}

static void otlp_invoked_attributes(const otlp_reading *reading, otlp_out *out,
                                    const ta_trace_record *record) {
    ta_otlp *otlp = reading->otlp;
    const ta_trace_invoked *invoked = (const ta_trace_invoked *)ta_trace_body(record);
    otlp_kv_string(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.invoke.id"),
                   otlp_reading_string(reading, invoked->invokeid));
    if (invoked->type != TA_NIL)
        otlp_kv_string(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.invoke.type"),
                       otlp_reading_string(reading, invoked->type));
    if (invoked->source != TA_NIL)
        otlp_kv_string(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.invoke.src"),
                       otlp_reading_string(reading, invoked->source));
    if (invoked->child_session)
        otlp_kv_int(otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.invoke.session_id"),
                    (int64_t)invoked->child_session);
}

static void otlp_cancelled_attributes(const otlp_reading *reading, otlp_out *out,
                                      const ta_trace_record *record) {
    const ta_trace_cancelled *cancelled = (const ta_trace_cancelled *)ta_trace_body(record);
    otlp_kv_string(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.event.sendid"),
                   otlp_reading_string(reading, cancelled->sendid));
}

static void otlp_assign_attributes(const otlp_reading *reading, otlp_out *out,
                                   const ta_trace_record *record) {
    const ta_trace_assign *assign = (const ta_trace_assign *)ta_trace_body(record);
    ta_expression_info info;
    if (ta_image_expression(reading->input->image, (ta_chart_item){record->chart, assign->location},
                            &info) == TA_OK)
        otlp_kv_string(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.location"), info.source);
    otlp_kv_value(reading, out, F_EVENT_ATTRIBUTES, TEXT("scxml.value"),
                  TEXT("scxml.value.truncated"), assign->datamodel, assign->payload);
}

static void otlp_log_attributes(const otlp_reading *reading, otlp_out *out,
                                const ta_trace_record *record) {
    const ta_trace_log *log = (const ta_trace_log *)ta_trace_body(record);
    if (log->label != TA_NIL)
        otlp_kv_string(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.log.label"),
                       otlp_reading_string(reading, log->label));
    if (log->text != TA_NIL)
        otlp_kv_string(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.log.text"),
                       otlp_reading_string(reading, log->text));
}

static void otlp_failed_attributes(const otlp_reading *reading, otlp_out *out,
                                   const ta_trace_record *record) {
    const ta_trace_action_failed *failed = (const ta_trace_action_failed *)ta_trace_body(record);
    if (failed->opcode != TA_NIL)
        otlp_kv_int(reading->otlp, out, F_EVENT_ATTRIBUTES, TEXT("scxml.action.opcode"),
                    failed->opcode);
}

/* A LogRecord (ScopeLogs.log_records field) into the scratch, for a <log>
 * or a failed action; empty with TA_OTLP_NO_LOGS or when it does not fit. */
static otlp_bytes otlp_log_encode(const otlp_reading *reading, otlp_scratch *scratch,
                                  const ta_trace_record *record, uint64_t time,
                                  uint32_t actor_name) {
    otlp_bytes none = {(const unsigned char *)0, 0, 0};
    ta_otlp *otlp = reading->otlp;
    if (otlp->config.flags & TA_OTLP_NO_LOGS) return none;
    int failure = record->kind == TA_TRACE_ACTION_FAILED;
    const ta_trace_log *log = (const ta_trace_log *)ta_trace_body(record);
    ta_string_view body = failure ? TEXT("action failed") : otlp_reading_string(reading, log->text);
    ta_string_view severity = failure ? TEXT("WARN") : TEXT("INFO");
    ta_string_view event_name = failure ? TEXT("tinyactors.action.failed") : TEXT("scxml.log");
    otlp_out tail = otlp_scratch_part(scratch);
    uint32_t fixed = 6u + 9u + 9u + 2u + otlp_field_size(F_LOG_SEVERITY_TEXT, severity.length) +
                     otlp_field_size(F_LOG_BODY, otlp_any_string_size(body.length)) +
                     otlp_field_size(F_LOG_EVENT_NAME, event_name.length) +
                     otlp_field_size(F_LOG_TRACE_ID, 16) + otlp_field_size(F_LOG_SPAN_ID, 8) + 5u;
    if (!otlp_room(&tail, fixed)) return none;
    unsigned char *begin = tail.at;
    tail.at += 6u;
    unsigned char *fields = tail.at;
    otlp_put_tag(&tail, F_LOG_TIME, WIRE_I64);
    otlp_put_fixed64(&tail, time);
    otlp_put_tag(&tail, F_LOG_OBSERVED_TIME, WIRE_I64);
    otlp_put_fixed64(&tail, time);
    otlp_put_tag(&tail, F_LOG_SEVERITY_NUMBER, WIRE_VARINT);
    otlp_put_varint(&tail, failure ? SEVERITY_WARN : SEVERITY_INFO);
    otlp_put_length(&tail, F_LOG_SEVERITY_TEXT, severity.length);
    otlp_put_bytes(&tail, severity.pointer, severity.length);
    otlp_put_length(&tail, F_LOG_BODY, otlp_any_string_size(body.length));
    otlp_put_length(&tail, F_ANY_STRING, body.length);
    otlp_put_bytes(&tail, body.pointer, body.length);
    otlp_put_length(&tail, F_LOG_EVENT_NAME, event_name.length);
    otlp_put_bytes(&tail, event_name.pointer, event_name.length);
    if (record->trace.high || record->trace.low) {
        otlp_put_length(&tail, F_LOG_TRACE_ID, 16);
        otlp_put_id64(&tail, record->trace.high);
        otlp_put_id64(&tail, record->trace.low);
        otlp_put_length(&tail, F_LOG_SPAN_ID, 8);
        otlp_put_id64(&tail, record->span);
        otlp_put_tag(&tail, F_LOG_FLAGS, WIRE_I32);
        uint32_t flags = record->flags & 0xFFu;
        for (int i = 0; i < 4; i++)
            *tail.at++ = (unsigned char)(flags >> (8 * i));
    }
    otlp_kv_int(otlp, &tail, F_LOG_ATTRIBUTES, TEXT("scxml.session_id"),
                (int64_t)record->key.session);
    if (actor_name != TA_NIL)
        otlp_kv_string(otlp, &tail, F_LOG_ATTRIBUTES, TEXT("tinyactors.actor.name"),
                       otlp_reading_string(reading, actor_name));
    otlp_kv_string(otlp, &tail, F_LOG_ATTRIBUTES, TEXT("tinyactors.chart.name"),
                   ta_image_chart_name(reading->input->image, record->chart));
    if (!failure && log->label != TA_NIL)
        otlp_kv_string(otlp, &tail, F_LOG_ATTRIBUTES, TEXT("scxml.log.label"),
                       otlp_reading_string(reading, log->label));
    uint32_t length = (uint32_t)(tail.at - fields);
    otlp_out out = {begin, fields};
    otlp_put_length(&out, F_SS_ITEMS, length);
    ta_memmove(out.at, fields, length);
    out.at += length;
    return otlp_scratch_close(scratch, &out);
}

static void otlp_log_stage(ta_otlp *otlp, otlp_bytes log) {
    unsigned char *logs = OTLP_POINTER(otlp, logs_relative, unsigned char);
    ta_memcpy(logs + otlp->logs_used, log.bytes, log.length);
    otlp->logs_used += log.length;
    otlp->log_count++;
}

/* ---- reading records --------------------------------------------------------------------- */

static ta_string_view otlp_kind_name(uint32_t kind) {
    switch (kind) {
    case TA_TRACE_SPAWNED:
        return TEXT("spawned");
    case TA_TRACE_FINISHED:
        return TEXT("finished");
    case TA_TRACE_DESTROYED:
        return TEXT("destroyed");
    case TA_TRACE_UNLOADED:
        return TEXT("unloaded");
    case TA_TRACE_LOADED:
        return TEXT("loaded");
    case TA_TRACE_MICROSTEP:
        return TEXT("microstep");
    default:
        return TEXT("macrostep");
    }
}

static otlp_skip *otlp_skips_of(const ta_otlp *otlp) {
    return OTLP_POINTER(otlp, skips_relative, otlp_skip);
}

static int otlp_skipped(const ta_otlp *otlp, uint64_t session, uint64_t macrostep) {
    for (uint32_t i = 0; i < otlp->skip_count; i++)
        if (otlp_skips_of(otlp)[i].session == session &&
            otlp_skips_of(otlp)[i].macrostep == macrostep)
            return 1;
    return 0;
}

static void otlp_skip_forget(ta_otlp *otlp, uint64_t session) {
    for (uint32_t i = 0; i < otlp->skip_count;)
        if (otlp_skips_of(otlp)[i].session == session)
            otlp_skips_of(otlp)[i] = otlp_skips_of(otlp)[--otlp->skip_count];
        else
            i++;
}

/* `marks`: attributes every span the record ends early gets */
static void otlp_needs_open_end(otlp_needs *needs, const otlp_open *open, uint32_t marks) {
    if (open->has_micro)
        otlp_needs_span(needs, &open->micro, &open->common, marks, TEXT("microstep"));
    otlp_needs_span(needs, &open->macro, &open->common, marks, TEXT("macrostep"));
}

/* End the open spans of a session early (incomplete, or lost to a drop),
 * with `marks`, and forget the session. */
static void otlp_open_end(const otlp_reading *reading, otlp_open *open, uint64_t end,
                          otlp_bytes marks, int error) {
    if (open->has_micro) {
        open->micro.error |= (uint32_t)error;
        otlp_span_stage(reading, &open->micro, &open->common, marks, TEXT("microstep"),
                        open->source, open->chart, end);
    }
    open->macro.error |= (uint32_t)error;
    otlp_span_stage(reading, &open->macro, &open->common, marks, TEXT("macrostep"), open->source,
                    open->chart, end);
    otlp_open_remove(reading->otlp, open);
}

static otlp_bytes otlp_incomplete_mark(otlp_scratch *scratch, ta_otlp *otlp) {
    otlp_out out = otlp_scratch_part(scratch);
    otlp_kv_bool(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.incomplete"), 1);
    return otlp_scratch_close(scratch, &out);
}

/* The span a record of a step adds to: a macrostep (microstep 0) or a
 * microstep. When the session has no open macrostep (the encoder started
 * after it began), one is opened as "continued". */
typedef struct otlp_target {
    otlp_open *open;         /* the open session, NULL: it is opened as continued */
    otlp_bytes common;       /* continued: its common attributes */
    otlp_bytes macro_fields; /* continued: the macrostep's name and mark */
    int end_micro;           /* an open microstep of another number ends first (incomplete) */
    int new_micro;           /* the record opens its microstep */
    otlp_bytes incomplete;
} otlp_target;

static void otlp_target_plan(const otlp_reading *reading, otlp_scratch *scratch,
                             const ta_trace_record *record, otlp_target *target,
                             otlp_needs *needs) {
    ta_otlp *otlp = reading->otlp;
    target->open = otlp_open_find(otlp, record->key.session);
    target->end_micro = target->new_micro = 0;
    target->incomplete = otlp_incomplete_mark(scratch, otlp);
    if (!target->open) {
        otlp_out out = otlp_scratch_part(scratch);
        otlp_common_attributes(reading, &out, record, TA_NIL);
        target->common = otlp_scratch_close(scratch, &out);
        out = otlp_scratch_part(scratch);
        otlp_put_name(&out, TEXT("scxml.macrostep"), TEXT(" (continued)"));
        otlp_kv_bool(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.continued"), 1);
        target->macro_fields = otlp_scratch_close(scratch, &out);
        needs->open++;
        needs->chunks +=
            2u + (target->common.length + target->macro_fields.length) / OTLP_CHUNK_PAYLOAD;
    }
    if (!record->key.microstep) return;
    if (target->open && target->open->has_micro &&
        target->open->micro.microstep != record->key.microstep) {
        target->end_micro = 1;
        otlp_needs_span(needs, &target->open->micro, &target->open->common,
                        target->incomplete.length, TEXT("microstep"));
    }
    if (!target->open || !target->open->has_micro || target->end_micro) target->new_micro = 1;
}

/* Carry out the plan: the session's open entry, and its microstep when the
 * record is about one; returns the span the record adds to. */
static otlp_span *otlp_target_take(const otlp_reading *reading, const ta_trace_record *record,
                                   otlp_target *target, uint64_t time, uint32_t source) {
    ta_otlp *otlp = reading->otlp;
    otlp_open *open = target->open;
    if (!open) {
        open = otlp_open_add(otlp, record->key.session, source, record->chart);
        open->macrostep = record->key.macrostep;
        otlp_chain_append(otlp, &open->common, target->common.bytes, target->common.length);
        otlp_chain_append(otlp, &open->macro.fields, target->macro_fields.bytes,
                          target->macro_fields.length);
        open->macro.trace = record->trace;
        open->macro.flags = record->flags;
        open->macro.start = time;
        open->macro.span_id = record->key.microstep ? record->parent : record->span;
        open->macro.parent_id = record->key.microstep ? 0 : record->parent;
        target->open = open;
    }
    if (!record->key.microstep) return &open->macro;
    if (target->end_micro) {
        otlp_span_stage(reading, &open->micro, &open->common, target->incomplete, TEXT("microstep"),
                        open->source, open->chart, time);
        otlp_chain_free(otlp, &open->micro.fields);
        open->has_micro = 0;
    }
    if (target->new_micro) {
        ta_memset(&open->micro, 0, sizeof open->micro);
        otlp_chain_init(&open->micro.fields);
        open->micro.trace = record->trace;
        open->micro.flags = record->flags;
        open->micro.span_id = record->span;
        open->micro.parent_id = record->parent;
        open->micro.start = time;
        open->micro.microstep = record->key.microstep;
        open->has_micro = 1;
    }
    return &open->micro;
}

/* The chunks appending `bytes` to the target's span takes. */
static uint32_t otlp_target_chunks(const ta_otlp *otlp, const otlp_target *target,
                                   const ta_trace_record *record, uint32_t bytes) {
    if (!target->open || target->new_micro)
        return (bytes + OTLP_CHUNK_PAYLOAD - 1u) / OTLP_CHUNK_PAYLOAD;
    const otlp_span *span = record->key.microstep ? &target->open->micro : &target->open->macro;
    return otlp_chunks_for(otlp, &span->fields, bytes);
}

static uint32_t otlp_read_dropped(const otlp_reading *reading, otlp_scratch *scratch,
                                  const ta_trace_record *record, uint64_t time) {
    ta_otlp *otlp = reading->otlp;
    uint64_t count = ((const ta_trace_dropped *)ta_trace_body(record))->count;
    otlp_out out = otlp_scratch_part(scratch);
    otlp_kv_int(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("tinyactors.telemetry.dropped"),
                (int64_t)count);
    otlp_bytes marks = otlp_scratch_close(scratch, &out);
    otlp_needs needs;
    ta_memset(&needs, 0, sizeof needs);
    for (uint32_t i = 0; i < otlp->config.open_spans; i++) {
        const otlp_open *open = otlp_open_at(otlp, i);
        if (!open->used) continue;
        otlp_needs_open_end(&needs, open, marks.length);
        otlp_needs_scope(otlp, &needs, open->source, open->chart);
        needs.skips++;
    }
    if (needs.scopes) needs.scopes = 0; /* counted per chart below */
    for (uint32_t i = 0; i < otlp->config.open_spans; i++) {
        const otlp_open *open = otlp_open_at(otlp, i);
        if (!open->used || otlp_scope_find(otlp, open->source, open->chart)) continue;
        int seen = 0;
        for (uint32_t j = 0; j < i && !seen; j++)
            seen = otlp_open_at(otlp, j)->used && otlp_open_at(otlp, j)->source == open->source &&
                   otlp_open_at(otlp, j)->chart == open->chart;
        needs.scopes += seen ? 0u : 1u;
    }
    if (needs.skips > otlp->config.open_spans - otlp->skip_count)
        needs.skips = otlp->config.open_spans - otlp->skip_count; /* the oldest are forgotten */
    uint32_t status = otlp_needs_check(otlp, &needs);
    if (status != TA_OTLP_READ) return status;
    otlp->dropped += count;
    for (uint32_t i = 0; i < otlp->config.open_spans; i++) {
        otlp_open *open = otlp_open_at(otlp, i);
        if (!open->used) continue;
        if (otlp->skip_count < otlp->config.open_spans) {
            otlp_skips_of(otlp)[otlp->skip_count].session = open->session;
            otlp_skips_of(otlp)[otlp->skip_count].macrostep = open->macrostep;
            otlp->skip_count++;
        }
        otlp_open_end(reading, open, time, marks, 1);
    }
    return TA_OTLP_READ;
}

static uint32_t otlp_read_lifecycle(const otlp_reading *reading, otlp_scratch *scratch,
                                    const ta_trace_record *record, uint64_t time, uint32_t source) {
    ta_otlp *otlp = reading->otlp;
    const void *body = ta_trace_body(record);
    uint32_t name = record->kind == TA_TRACE_SPAWNED    ? ((const ta_trace_spawned *)body)->name
                    : record->kind == TA_TRACE_FINISHED ? ((const ta_trace_finished *)body)->name
                                                        : ((const ta_trace_lifecycle *)body)->name;
    ta_string_view kind = otlp_kind_name(record->kind);
    otlp_span span;
    ta_memset(&span, 0, sizeof span);
    otlp_chain_init(&span.fields);
    span.trace = record->trace;
    span.span_id = record->span;
    span.parent_id = record->parent;
    span.flags = record->flags;
    span.start = time;
    otlp_out out = otlp_scratch_part(scratch);
    otlp_put_name(&out, TEXT("scxml."), kind);
    otlp_common_attributes(reading, &out, record, name);
    if (record->kind == TA_TRACE_FINISHED &&
        ((const ta_trace_finished *)body)->final_state != TA_NIL)
        otlp_kv_string(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.final_state"),
                       otlp_state_name(reading, record->chart,
                                       ((const ta_trace_finished *)body)->final_state));
    otlp_bytes fields = otlp_scratch_close(scratch, &out);
    otlp_needs needs;
    ta_memset(&needs, 0, sizeof needs);
    /* a session that goes ends what it had open */
    otlp_open *open = record->kind == TA_TRACE_DESTROYED || record->kind == TA_TRACE_UNLOADED
                          ? otlp_open_find(otlp, record->key.session)
                          : (otlp_open *)0;
    otlp_bytes incomplete = otlp_incomplete_mark(scratch, otlp);
    if (open) otlp_needs_open_end(&needs, open, incomplete.length);
    otlp_needs_span(&needs, &span, (const otlp_chain *)0, fields.length, kind);
    otlp_needs_scope(otlp, &needs, source, record->chart);
    uint32_t status = otlp_needs_check(otlp, &needs);
    if (status != TA_OTLP_READ) return status;
    if (open) otlp_open_end(reading, open, time, incomplete, 0);
    otlp_span_stage(reading, &span, (const otlp_chain *)0, fields, kind, source, record->chart,
                    time + 1u);
    return TA_OTLP_READ;
}

static uint32_t otlp_read_begin(const otlp_reading *reading, otlp_scratch *scratch,
                                const ta_trace_record *record, uint64_t time, uint32_t source) {
    ta_otlp *otlp = reading->otlp;
    const ta_trace_macrostep_begin *begin = (const ta_trace_macrostep_begin *)ta_trace_body(record);
    otlp_out out = otlp_scratch_part(scratch);
    otlp_common_attributes(reading, &out, record, begin->actor_name);
    otlp_bytes common = otlp_scratch_close(scratch, &out);
    out = otlp_scratch_part(scratch);
    ta_string_view event = otlp_reading_string(reading, begin->name);
    otlp_put_name(&out, TEXT("scxml.macrostep "),
                  begin->name == TA_NIL ? TEXT("bootstrap") : event);
    if (begin->name != TA_NIL) {
        otlp_kv_string(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.event.name"), event);
        otlp_kv_string(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.event.type"),
                       otlp_event_type_name(begin->type));
        if (begin->sendid != TA_NIL)
            otlp_kv_string(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.event.sendid"),
                           otlp_reading_string(reading, begin->sendid));
        if (begin->origin != TA_NIL)
            otlp_kv_string(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.event.origin"),
                           otlp_reading_string(reading, begin->origin));
        else if (begin->origin_session)
            otlp_kv_int(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.event.origin_session_id"),
                        (int64_t)begin->origin_session);
        otlp_kv_value(reading, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.event.data"),
                      TEXT("scxml.event.data.truncated"), begin->datamodel, begin->payload);
    }
    if (begin->cause.session) {
        otlp_kv_int(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.cause.session_id"),
                    (int64_t)begin->cause.session);
        otlp_kv_int(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.cause.macrostep"),
                    (int64_t)begin->cause.macrostep);
        otlp_kv_int(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.cause.microstep"),
                    begin->cause.microstep);
    }
    otlp_bytes fields = otlp_scratch_close(scratch, &out);
    otlp_needs needs;
    ta_memset(&needs, 0, sizeof needs);
    otlp_open *open = otlp_open_find(otlp, record->key.session);
    otlp_bytes incomplete = otlp_incomplete_mark(scratch, otlp);
    if (open) {
        otlp_needs_open_end(&needs, open, incomplete.length);
        otlp_needs_scope(otlp, &needs, open->source, open->chart);
    } else {
        needs.open = 1;
    }
    needs.chunks = 2u + common.length / OTLP_CHUNK_PAYLOAD + fields.length / OTLP_CHUNK_PAYLOAD;
    uint32_t status = otlp_needs_check(otlp, &needs);
    if (status != TA_OTLP_READ) return status;
    otlp_skip_forget(otlp, record->key.session);
    if (open) otlp_open_end(reading, open, time, incomplete, 0);
    open = otlp_open_add(otlp, record->key.session, source, record->chart);
    open->macrostep = record->key.macrostep;
    otlp_chain_append(otlp, &open->common, common.bytes, common.length);
    otlp_chain_append(otlp, &open->macro.fields, fields.bytes, fields.length);
    open->macro.trace = record->trace;
    open->macro.flags = record->flags;
    open->macro.span_id = record->span;
    open->macro.start = time;
    if (otlp->config.flags & TA_OTLP_LINK_PARENTS)
        open->macro.link_id = record->parent; /* a root, linked to its cause */
    else
        open->macro.parent_id = record->parent;
    return TA_OTLP_READ;
}

/* ---- macrostep durations (TA_OTLP_SPAN_METRICS, docs/adr/0007) ------------------------------ */

static const double OTLP_DURATION_BOUNDS[OTLP_DURATION_BUCKETS - 1u] = {
    0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.05, 0.25, 1.0};

static otlp_duration *otlp_durations(const ta_otlp *otlp) {
    return OTLP_POINTER(otlp, durations_relative, otlp_duration);
}

/* A macrostep span that ended at `end` (wall ns): into its chart's histogram;
 * a chart past the table's capacity is not kept. */
static void otlp_duration_add(const otlp_reading *reading, const otlp_open *open, uint64_t end) {
    ta_otlp *otlp = reading->otlp;
    otlp_duration *table = otlp_durations(otlp), *entry = (otlp_duration *)0;
    for (uint32_t i = 0; i < otlp->duration_count && !entry; i++)
        if (table[i].source == open->source && table[i].chart == open->chart) entry = &table[i];
    if (!entry) {
        if (otlp->duration_count == otlp->config.span_metric_charts) return;
        entry = &table[otlp->duration_count++];
        ta_memset(entry, 0, sizeof *entry);
        entry->source = open->source;
        entry->chart = open->chart;
        ta_string_view name = {"", 0};
        if (open->chart < reading->view->chart_count)
            name = ta_image_view_string(reading->view, reading->view->charts[open->chart].name);
        entry->name_length =
            name.length < OTLP_CHART_NAME_BYTES ? name.length : OTLP_CHART_NAME_BYTES;
        if (entry->name_length) ta_memcpy(entry->name, name.pointer, entry->name_length);
    }
    double seconds = (double)(end > open->macro.start ? end - open->macro.start : 1u) / 1e9;
    uint32_t bucket = 0;
    while (bucket + 1u < OTLP_DURATION_BUCKETS && seconds > OTLP_DURATION_BOUNDS[bucket])
        bucket++;
    entry->buckets[bucket]++;
    entry->min = entry->count ? (seconds < entry->min ? seconds : entry->min) : seconds;
    entry->max = entry->count ? (seconds > entry->max ? seconds : entry->max) : seconds;
    entry->count++;
    entry->sum += seconds;
    ta_otlp_exemplar *exemplar = &entry->exemplars[bucket]; /* the latest span in the bucket */
    exemplar->trace = open->macro.trace;
    exemplar->span = open->macro.span_id;
    exemplar->time = end;
    exemplar->value = seconds;
}

static uint32_t otlp_read_end(const otlp_reading *reading, otlp_scratch *scratch,
                              const ta_trace_record *record, uint64_t time, uint32_t source) {
    ta_otlp *otlp = reading->otlp;
    const ta_trace_macrostep_end *end = (const ta_trace_macrostep_end *)ta_trace_body(record);
    otlp_target target;
    otlp_needs needs;
    ta_memset(&needs, 0, sizeof needs);
    otlp_target_plan(reading, scratch, record, &target, &needs);
    otlp_out out = otlp_scratch_part(scratch);
    const uint32_t *states = ta_trace_list(record);
    otlp_kv_states(reading, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.state"), record->chart, states,
                   end->state_count);
    otlp_kv_ints(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.state.id"), states, end->state_count);
    otlp_bytes fields = otlp_scratch_close(scratch, &out);
    if (target.open) {
        if (target.open->has_micro)
            otlp_needs_span(&needs, &target.open->micro, &target.open->common,
                            target.incomplete.length, TEXT("microstep"));
        otlp_needs_span(&needs, &target.open->macro, &target.open->common, fields.length,
                        TEXT("macrostep"));
    } else {
        otlp_span span;
        ta_memset(&span, 0, sizeof span);
        otlp_chain common = {TA_NIL, TA_NIL, target.common.length, 0};
        span.fields.bytes = target.macro_fields.length;
        span.parent_id = record->parent;
        otlp_needs_span(&needs, &span, &common, fields.length, TEXT("macrostep"));
    }
    otlp_needs_scope(otlp, &needs, source, record->chart);
    uint32_t status = otlp_needs_check(otlp, &needs);
    if (status != TA_OTLP_READ) return status;
    otlp_open *open = (otlp_target_take(reading, record, &target, time, source), target.open);
    if (open->has_micro)
        otlp_span_stage(reading, &open->micro, &open->common, target.incomplete, TEXT("microstep"),
                        open->source, open->chart, time);
    otlp_span_stage(reading, &open->macro, &open->common, fields, TEXT("macrostep"), open->source,
                    open->chart, time);
    if (otlp->config.flags & TA_OTLP_SPAN_METRICS) otlp_duration_add(reading, open, time);
    otlp_open_remove(otlp, open);
    return TA_OTLP_READ;
}

static uint32_t otlp_read_microstep(const otlp_reading *reading, otlp_scratch *scratch,
                                    const ta_trace_record *record, uint64_t time, uint32_t source) {
    ta_otlp *otlp = reading->otlp;
    const ta_trace_microstep *micro = (const ta_trace_microstep *)ta_trace_body(record);
    otlp_target target;
    otlp_needs needs;
    ta_memset(&needs, 0, sizeof needs);
    otlp_target_plan(reading, scratch, record, &target, &needs);
    const uint32_t *list = ta_trace_list(record);
    otlp_out out = otlp_scratch_part(scratch);
    otlp_put_name(&out, TEXT("scxml.microstep "),
                  micro->name == TA_NIL ? TEXT("(eventless)")
                                        : otlp_reading_string(reading, micro->name));
    otlp_kv_int(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.microstep"), record->key.microstep);
    if (micro->name != TA_NIL)
        otlp_kv_string(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.event.name"),
                       otlp_reading_string(reading, micro->name));
    otlp_kv_transitions(reading, &out, record->chart, list, micro->transition_count);
    list += micro->transition_count;
    otlp_kv_states(reading, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.state.exited"), record->chart,
                   list, micro->exited_count);
    otlp_kv_ints(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.state.exited.id"), list,
                 micro->exited_count);
    list += micro->exited_count;
    otlp_kv_states(reading, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.state.entered"), record->chart,
                   list, micro->entered_count);
    otlp_kv_ints(otlp, &out, F_SPAN_ATTRIBUTES, TEXT("scxml.state.entered.id"), list,
                 micro->entered_count);
    otlp_bytes fields = otlp_scratch_close(scratch, &out);
    /* the microstep's own span: what gathered for it, and these fields */
    otlp_span staged;
    ta_memset(&staged, 0, sizeof staged);
    otlp_chain_init(&staged.fields);
    if (target.open && target.open->has_micro && !target.end_micro)
        staged.fields.bytes = target.open->micro.fields.bytes;
    if (target.open && target.open->has_micro && !target.end_micro)
        staged.error = target.open->micro.error;
    staged.parent_id = record->parent;
    otlp_chain common = {TA_NIL, TA_NIL,
                         target.open ? target.open->common.bytes : target.common.length, 0};
    otlp_needs_span(&needs, &staged, &common, fields.length, TEXT("microstep"));
    otlp_needs_scope(otlp, &needs, source, record->chart);
    uint32_t status = otlp_needs_check(otlp, &needs);
    if (status != TA_OTLP_READ) return status;
    otlp_span *span = otlp_target_take(reading, record, &target, time, source);
    span->start = otlp_wall_of(otlp, micro->begin_fuel);
    otlp_open *open = target.open;
    otlp_span_stage(reading, span, &open->common, fields, TEXT("microstep"), open->source,
                    open->chart, time);
    otlp_chain_free(otlp, &open->micro.fields);
    open->has_micro = 0;
    return TA_OTLP_READ;
}

/* A record of something that happened in a step: a span event on it. */
static uint32_t otlp_read_event(const otlp_reading *reading, otlp_scratch *scratch,
                                const ta_trace_record *record, uint64_t time, uint32_t source) {
    ta_otlp *otlp = reading->otlp;
    otlp_attributes attributes = (otlp_attributes)0;
    ta_string_view name = TEXT("");
    switch (record->kind) {
    case TA_TRACE_CONDITION:
        attributes = otlp_condition_attributes, name = TEXT("scxml.condition");
        break;
    case TA_TRACE_RAISED:
        attributes = otlp_raised_attributes, name = TEXT("scxml.raised");
        break;
    case TA_TRACE_SENT:
        attributes = otlp_sent_attributes, name = TEXT("scxml.sent");
        break;
    case TA_TRACE_INVOKED:
        attributes = otlp_invoked_attributes, name = TEXT("scxml.invoked");
        break;
    case TA_TRACE_CANCELLED:
        attributes = otlp_cancelled_attributes, name = TEXT("scxml.cancelled");
        break;
    case TA_TRACE_ASSIGN:
        attributes = otlp_assign_attributes, name = TEXT("scxml.assign");
        break;
    case TA_TRACE_LOG:
        attributes = otlp_log_attributes, name = TEXT("scxml.log");
        break;
    default:
        attributes = otlp_failed_attributes, name = TEXT("scxml.action_failed");
        break;
    }
    otlp_target target;
    otlp_needs needs;
    ta_memset(&needs, 0, sizeof needs);
    otlp_target_plan(reading, scratch, record, &target, &needs);
    otlp_bytes event = otlp_event_encode(reading, scratch, name, time, attributes, record);
    otlp_bytes log = {(const unsigned char *)0, 0, 0};
    if (record->kind == TA_TRACE_LOG || record->kind == TA_TRACE_ACTION_FAILED)
        log = otlp_log_encode(reading, scratch, record, time, TA_NIL);
    needs.chunks += otlp_target_chunks(otlp, &target, record, event.length) + 1u;
    needs.logs = log.length;
    if (target.end_micro) otlp_needs_scope(otlp, &needs, source, record->chart);
    uint32_t status = otlp_needs_check(otlp, &needs);
    if (status != TA_OTLP_READ) return status;
    otlp_span *span = otlp_target_take(reading, record, &target, time, source);
    otlp_chain_append(otlp, &span->fields, event.bytes, event.length);
    if (record->kind == TA_TRACE_ACTION_FAILED) span->error = 1;
    if (log.length) otlp_log_stage(otlp, log);
    return TA_OTLP_READ;
}

static uint32_t otlp_read_record(const otlp_reading *reading, const ta_trace_record *record) {
    ta_otlp *otlp = reading->otlp;
    otlp_scratch scratch = {OTLP_POINTER(otlp, scratch_relative, unsigned char),
                            otlp->scratch_bytes, 0};
    uint64_t time = otlp_wall_of(otlp, record->fuel);
    uint32_t source = reading->input->source;
    if (record->kind != TA_TRACE_DROPPED && record->chart >= reading->view->chart_count)
        return TA_OTLP_READ; /* not of this image: nothing to name it with */
    switch (record->kind) {
    case TA_TRACE_DROPPED:
        return otlp_read_dropped(reading, &scratch, record, time);
    case TA_TRACE_SPAWNED:
    case TA_TRACE_FINISHED:
    case TA_TRACE_DESTROYED:
    case TA_TRACE_UNLOADED:
    case TA_TRACE_LOADED:
        return otlp_read_lifecycle(reading, &scratch, record, time, source);
    case TA_TRACE_MACROSTEP_BEGIN:
        return otlp_read_begin(reading, &scratch, record, time, source);
    default:
        break;
    }
    /* the rest belongs to a macrostep a drop may have ended */
    if (!otlp_open_find(otlp, record->key.session) &&
        otlp_skipped(otlp, record->key.session, record->key.macrostep)) {
        otlp->skipped++;
        return TA_OTLP_READ;
    }
    if (record->kind == TA_TRACE_MACROSTEP_END)
        return otlp_read_end(reading, &scratch, record, time, source);
    if (record->kind == TA_TRACE_MICROSTEP)
        return otlp_read_microstep(reading, &scratch, record, time, source);
    return otlp_read_event(reading, &scratch, record, time, source);
}

uint32_t ta_otlp_read(ta_otlp *otlp, const ta_otlp_input *input, uint32_t *cursor) {
    otlp_reading reading = {otlp, input, ta_image_view_of(input->image)};
    for (;;) {
        uint32_t at = *cursor;
        const ta_trace_record *record = ta_trace_next(&input->view, cursor);
        if (!record) return TA_OTLP_READ;
        uint32_t status = otlp_read_record(&reading, record);
        if (status != TA_OTLP_READ) {
            *cursor = at; /* not read: read it again after the flush or the growth */
            return status;
        }
        otlp->records++;
    }
}

void ta_otlp_end_all(ta_otlp *otlp, uint64_t wall_ns) {
    /* ends what fits; the host flushes and calls again for the rest */
    for (uint32_t i = 0; i < otlp->config.open_spans; i++) {
        otlp_open *open = otlp_open_at(otlp, i);
        if (!open->used) continue;
        otlp_scratch scratch = {OTLP_POINTER(otlp, scratch_relative, unsigned char),
                                otlp->scratch_bytes, 0};
        otlp_bytes incomplete = otlp_incomplete_mark(&scratch, otlp);
        otlp_needs needs;
        ta_memset(&needs, 0, sizeof needs);
        otlp_needs_open_end(&needs, open, incomplete.length);
        if (!otlp_scope_find(otlp, open->source, open->chart)) needs.scopes = 1;
        if (otlp_needs_check(otlp, &needs) != TA_OTLP_READ) return;
        /* the scope is named by the image the records came with: without one
         * now, it is named by its index alone */
        otlp_scope *scope = otlp_scope_find(otlp, open->source, open->chart);
        if (!scope) {
            scope = otlp_scope_at(otlp, otlp->scope_count++);
            ta_memset(scope, 0, sizeof *scope);
            scope->source = open->source;
            scope->chart = open->chart;
            scope->bytes_at = otlp->scope_bytes_used;
            unsigned char *bytes = OTLP_POINTER(otlp, scope_bytes_relative, unsigned char);
            otlp_out out = {bytes + scope->bytes_at, bytes + scope->bytes_at + OTLP_SCOPE_BYTES};
            otlp_put_length(&out, F_SCOPE_NAME, 10);
            otlp_put_bytes(&out, "tinyactors", 10);
            otlp_kv_int(otlp, &out, F_SCOPE_ATTRIBUTES, TEXT("tinyactors.chart.index"),
                        open->chart);
            scope->bytes_length = (uint32_t)(out.at - (bytes + scope->bytes_at));
            otlp->scope_bytes_used += OTLP_SCOPE_BYTES;
        }
        otlp_reading reading = {otlp, (const ta_otlp_input *)0, (const ta_image_view *)0};
        otlp_open_end(&reading, open, wall_ns, incomplete, 0);
    }
}

/* ---- requests ------------------------------------------------------------------------------- */

static uint32_t otlp_resource_size(const ta_otlp *otlp) {
    return otlp_field_size(F_RS_RESOURCE, otlp->resource_used);
}

static uint32_t otlp_scope_size(const otlp_scope *scope) {
    return otlp_field_size(F_RS_SCOPES,
                           otlp_field_size(F_SS_SCOPE, scope->bytes_length) + scope->spans);
}

static uint64_t otlp_traces_body(const ta_otlp *otlp) {
    uint64_t body = otlp_resource_size(otlp);
    for (uint32_t i = 0; i < otlp->scope_count; i++)
        if (otlp_scope_at(otlp, i)->spans) body += otlp_scope_size(otlp_scope_at(otlp, i));
    return body;
}

static size_t otlp_traces_size(const ta_otlp *otlp) {
    if (!otlp->span_count) return 0;
    return otlp_field_size(F_REQUEST_RESOURCE, (uint32_t)otlp_traces_body(otlp));
}

static void otlp_put_resource(const ta_otlp *otlp, otlp_out *out) {
    otlp_put_length(out, F_RS_RESOURCE, otlp->resource_used);
    otlp_put_bytes(out, OTLP_POINTER(otlp, resource_relative, unsigned char), otlp->resource_used);
}

static void otlp_spans_forget(ta_otlp *otlp) {
    otlp->spans_used = otlp->entry_count = otlp->scope_count = otlp->scope_bytes_used = 0;
    otlp->span_count = 0;
}

ta_status ta_otlp_put_traces(ta_otlp *otlp, ta_buffer *destination) {
    size_t size = otlp_traces_size(otlp);
    destination->length = size;
    if (!size) return TA_OK;
    if (destination->capacity < size) return TA_ERROR_NO_MEMORY;
    unsigned char *buffer = (unsigned char *)destination->pointer;
    otlp_out out = {buffer, buffer + size};
    otlp_put_length(&out, F_REQUEST_RESOURCE, (uint32_t)otlp_traces_body(otlp));
    otlp_put_resource(otlp, &out);
    const unsigned char *spans = OTLP_POINTER(otlp, spans_relative, unsigned char);
    const unsigned char *scope_bytes = OTLP_POINTER(otlp, scope_bytes_relative, unsigned char);
    for (uint32_t i = 0; i < otlp->scope_count; i++) {
        const otlp_scope *scope = otlp_scope_at(otlp, i);
        if (!scope->spans) continue;
        otlp_put_length(&out, F_RS_SCOPES,
                        otlp_field_size(F_SS_SCOPE, scope->bytes_length) + scope->spans);
        otlp_put_length(&out, F_SS_SCOPE, scope->bytes_length);
        otlp_put_bytes(&out, scope_bytes + scope->bytes_at, scope->bytes_length);
        for (uint32_t e = scope->first; e; e = otlp_entry_at(otlp, e - 1u)->next)
            otlp_put_bytes(&out, spans + otlp_entry_at(otlp, e - 1u)->at,
                           otlp_entry_at(otlp, e - 1u)->length);
    }
    TA_ASSERT(out.at == out.end);
    otlp_spans_forget(otlp);
    return TA_OK;
}

/* the logs' one scope: tinyactors */
static uint32_t otlp_logs_scope_size(const ta_otlp *otlp) {
    uint32_t scope = otlp_field_size(F_SCOPE_NAME, 10) +
                     otlp_field_size(F_SCOPE_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    return otlp_field_size(F_RS_SCOPES, otlp_field_size(F_SS_SCOPE, scope) + otlp->logs_used);
}

static size_t otlp_logs_size(const ta_otlp *otlp) {
    if (!otlp->log_count) return 0;
    return otlp_field_size(F_REQUEST_RESOURCE,
                           otlp_resource_size(otlp) + otlp_logs_scope_size(otlp));
}

ta_status ta_otlp_put_logs(ta_otlp *otlp, ta_buffer *destination) {
    size_t size = otlp_logs_size(otlp);
    destination->length = size;
    if (!size) return TA_OK;
    if (destination->capacity < size) return TA_ERROR_NO_MEMORY;
    unsigned char *buffer = (unsigned char *)destination->pointer;
    otlp_out out = {buffer, buffer + size};
    otlp_put_length(&out, F_REQUEST_RESOURCE,
                    otlp_resource_size(otlp) + otlp_logs_scope_size(otlp));
    otlp_put_resource(otlp, &out);
    uint32_t scope = otlp_field_size(F_SCOPE_NAME, 10) +
                     otlp_field_size(F_SCOPE_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    otlp_put_length(&out, F_RS_SCOPES, otlp_field_size(F_SS_SCOPE, scope) + otlp->logs_used);
    otlp_put_length(&out, F_SS_SCOPE, scope);
    otlp_put_length(&out, F_SCOPE_NAME, 10);
    otlp_put_bytes(&out, "tinyactors", 10);
    otlp_put_length(&out, F_SCOPE_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    otlp_put_bytes(&out, OTLP_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    otlp_put_bytes(&out, OTLP_POINTER(otlp, logs_relative, unsigned char), otlp->logs_used);
    TA_ASSERT(out.at == out.end);
    otlp->logs_used = otlp->log_count = 0;
    return TA_OK;
}

ta_status ta_otlp_set_resource(ta_otlp *otlp, const ta_otlp_attribute *attributes, uint32_t count) {
    uint64_t need = 0;
    for (uint32_t i = 0; i < count; i++)
        need +=
            otlp_field_size(F_RESOURCE_ATTRIBUTES,
                            otlp_key_value_size(attributes[i].key,
                                                otlp_any_string_size(attributes[i].value.length)));
    if (need > otlp->config.resource_bytes) return TA_ERROR_NO_MEMORY;
    unsigned char *resource = OTLP_POINTER(otlp, resource_relative, unsigned char);
    otlp_out out = {resource, resource + otlp->config.resource_bytes};
    for (uint32_t i = 0; i < count; i++)
        otlp_kv_string(otlp, &out, F_RESOURCE_ATTRIBUTES, attributes[i].key, attributes[i].value);
    otlp->resource_used = (uint32_t)(out.at - resource);
    return TA_OK;
}

void ta_otlp_set_flags(ta_otlp *otlp, uint32_t flags) {
    otlp->config.flags = flags;
}

void ta_otlp_stats_get(const ta_otlp *otlp, ta_otlp_stats *out) {
    ta_otlp_stats stats;
    ta_memset(&stats, 0, sizeof stats);
    stats.size = sizeof stats;
    stats.open = otlp->open_count;
    stats.open_capacity = otlp->config.open_spans;
    stats.chunks = otlp->chunks_used;
    stats.chunks_capacity = otlp->config.chunks;
    stats.spans = otlp->span_count;
    stats.staging_bytes = otlp->spans_used;
    stats.staging_capacity = otlp->config.staging_bytes;
    stats.log_records = otlp->log_count;
    stats.log_bytes = otlp->logs_used;
    stats.log_capacity = otlp->config.log_bytes;
    stats.metrics = otlp->metric_count;
    stats.metric_bytes = otlp->metrics_used;
    stats.metric_capacity = otlp->config.metric_bytes;
    stats.span_metric_charts = otlp->duration_count;
    stats.span_metric_capacity = otlp->config.span_metric_charts;
    stats.records = otlp->records;
    stats.dropped = otlp->dropped;
    stats.skipped = otlp->skipped;
    stats.truncated = otlp->truncated;
    uint32_t n = out->size && out->size < sizeof stats ? out->size : (uint32_t)sizeof stats;
    ta_memcpy(out, &stats, n);
    out->size = n;
}

/* ---- the block ------------------------------------------------------------------------------ */

static void otlp_normalize(const ta_otlp_config *in, ta_otlp_config *config) {
    ta_memset(config, 0, sizeof *config);
    if (in)
        ta_memcpy(config, in, in->size && in->size < sizeof *config ? in->size : sizeof *config);
    config->size = sizeof *config;
    if (!config->staging_bytes) config->staging_bytes = TA_OTLP_STAGING_BYTES_DEFAULT;
    if (config->staging_bytes < TA_OTLP_STAGING_BYTES_MIN)
        config->staging_bytes = TA_OTLP_STAGING_BYTES_MIN;
    if (config->staging_bytes > TA_OTLP_STAGING_BYTES_MAX)
        config->staging_bytes = TA_OTLP_STAGING_BYTES_MAX;
    if (!config->log_bytes) config->log_bytes = config->staging_bytes / 4u;
    if (config->log_bytes > TA_OTLP_STAGING_BYTES_MAX)
        config->log_bytes = TA_OTLP_STAGING_BYTES_MAX;
    if (!config->open_spans) config->open_spans = 1024;
    if (config->open_spans > 0x1000000u) config->open_spans = 0x1000000u;
    if (!config->chunks) config->chunks = 8u * config->open_spans;
    if (config->chunks > 0x4000000u) config->chunks = 0x4000000u;
    if (!config->scopes) config->scopes = 64;
    if (config->scopes > 65536u) config->scopes = 65536u;
    if (!config->value_limit) config->value_limit = 256;
    if (!config->render_bytes) config->render_bytes = 16384;
    if (config->render_bytes < config->value_limit + 4u)
        config->render_bytes = config->value_limit + 4u;
    if (!config->resource_bytes) config->resource_bytes = 4096;
    if (!config->metric_bytes) config->metric_bytes = 65536;
    if (!config->span_metric_charts) config->span_metric_charts = 64;
    if (config->span_metric_charts > 65536u) config->span_metric_charts = 65536u;
    if (config->metric_bytes > TA_OTLP_STAGING_BYTES_MAX)
        config->metric_bytes = TA_OTLP_STAGING_BYTES_MAX;
}

typedef struct otlp_carved {
    size_t header, open, index, skips, chunks, spans, entries, scopes, scope_bytes, logs, resource,
        scratch, render, text, metrics, durations;
    uint32_t index_capacity, entry_capacity, scratch_bytes;
} otlp_carved;

static void otlp_carve(ta_arena *arena, const ta_otlp_config *config, otlp_carved *o) {
    o->index_capacity = 16;
    while (o->index_capacity < 2u * config->open_spans)
        o->index_capacity <<= 1;
    o->entry_capacity = config->staging_bytes / 64u + 16u;
    /* a record's bytes: values twice over, names and lists */
    o->scratch_bytes = 65536u + 4u * config->value_limit;
    o->header = ta_arena_alloc(arena, sizeof(ta_otlp), TA_ALIGN_MAX);
    o->open = ta_arena_alloc(arena, sizeof(otlp_open) * (size_t)config->open_spans, TA_ALIGN_MAX);
    o->index = ta_arena_alloc(arena, sizeof(uint32_t) * (size_t)o->index_capacity, 4);
    o->skips = ta_arena_alloc(arena, sizeof(otlp_skip) * (size_t)config->open_spans, TA_ALIGN_MAX);
    o->chunks = ta_arena_alloc(arena, sizeof(otlp_chunk) * (size_t)config->chunks, TA_ALIGN_MAX);
    o->spans = ta_arena_alloc(arena, config->staging_bytes, TA_ALIGN_MAX);
    o->entries =
        ta_arena_alloc(arena, sizeof(otlp_entry) * (size_t)o->entry_capacity, TA_ALIGN_MAX);
    o->scopes = ta_arena_alloc(arena, sizeof(otlp_scope) * (size_t)config->scopes, TA_ALIGN_MAX);
    o->scope_bytes = ta_arena_alloc(arena, (size_t)OTLP_SCOPE_BYTES * config->scopes, TA_ALIGN_MAX);
    o->logs = ta_arena_alloc(arena, config->log_bytes, TA_ALIGN_MAX);
    o->resource = ta_arena_alloc(arena, config->resource_bytes, TA_ALIGN_MAX);
    o->scratch = ta_arena_alloc(arena, o->scratch_bytes, TA_ALIGN_MAX);
    o->render = ta_arena_alloc(arena, config->render_bytes, TA_ALIGN_MAX);
    o->text = ta_arena_alloc(arena, (size_t)config->value_limit + 1u, TA_ALIGN_MAX);
    o->metrics = ta_arena_alloc(arena, config->metric_bytes, TA_ALIGN_MAX);
    o->durations =
        ta_arena_alloc(arena, sizeof(otlp_duration) * config->span_metric_charts, TA_ALIGN_MAX);
}

ta_status ta_otlp_layout(const ta_otlp_config *config, size_t *bytes) {
    ta_otlp_config normalized;
    otlp_normalize(config, &normalized);
    ta_arena arena;
    otlp_carved o;
    ta_arena_init_measure(&arena);
    otlp_carve(&arena, &normalized, &o);
    *bytes = ta_arena_required(&arena);
    return arena.failed ? TA_ERROR_INVALID : TA_OK;
}

static int64_t otlp_relative(const ta_otlp *otlp, const ta_arena *arena, size_t offset) {
    return (int64_t)((intptr_t)ta_arena_pointer(arena, offset) - (intptr_t)otlp);
}

/* Lay out a block and its header (empty). */
static ta_otlp *otlp_place(void *memory, size_t length, const ta_otlp_config *config) {
    ta_arena arena;
    otlp_carved o;
    ta_memset(memory, 0, length);
    ta_arena_init(&arena, memory, length);
    otlp_carve(&arena, config, &o);
    if (arena.failed) return (ta_otlp *)0;
    ta_otlp *otlp = (ta_otlp *)ta_arena_pointer(&arena, o.header);
    otlp->magic = OTLP_MAGIC;
    otlp->block_bytes = arena.offset;
    otlp->config = *config;
    otlp->index_capacity = o.index_capacity;
    otlp->entry_capacity = o.entry_capacity;
    otlp->scratch_bytes = o.scratch_bytes;
    otlp->open_relative = otlp_relative(otlp, &arena, o.open);
    otlp->index_relative = otlp_relative(otlp, &arena, o.index);
    otlp->skips_relative = otlp_relative(otlp, &arena, o.skips);
    otlp->chunks_relative = otlp_relative(otlp, &arena, o.chunks);
    otlp->spans_relative = otlp_relative(otlp, &arena, o.spans);
    otlp->entries_relative = otlp_relative(otlp, &arena, o.entries);
    otlp->scopes_relative = otlp_relative(otlp, &arena, o.scopes);
    otlp->scope_bytes_relative = otlp_relative(otlp, &arena, o.scope_bytes);
    otlp->logs_relative = otlp_relative(otlp, &arena, o.logs);
    otlp->resource_relative = otlp_relative(otlp, &arena, o.resource);
    otlp->scratch_relative = otlp_relative(otlp, &arena, o.scratch);
    otlp->render_relative = otlp_relative(otlp, &arena, o.render);
    otlp->text_relative = otlp_relative(otlp, &arena, o.text);
    otlp->metrics_relative = otlp_relative(otlp, &arena, o.metrics);
    otlp->durations_relative = otlp_relative(otlp, &arena, o.durations);
    otlp->chunk_free = TA_NIL;
    return otlp;
}

ta_status ta_otlp_init(ta_memory memory, const ta_otlp_config *config, ta_otlp **out) {
    *out = (ta_otlp *)0;
    if (!memory.pointer) return TA_ERROR_INVALID;
    ta_otlp_config normalized;
    otlp_normalize(config, &normalized);
    ta_otlp *otlp = otlp_place(memory.pointer, memory.length, &normalized);
    if (!otlp) return TA_ERROR_NO_MEMORY;
    *out = otlp;
    return TA_OK;
}

ta_status ta_otlp_relocate(ta_otlp **relocated, ta_memory memory, const ta_otlp_config *config) {
    ta_otlp *otlp = *relocated;
    ta_otlp_config normalized;
    otlp_normalize(config ? config : &otlp->config, &normalized);
    uint32_t entry_capacity = normalized.staging_bytes / 64u + 16u;
    if (normalized.open_spans < otlp->open_count || normalized.open_spans < otlp->skip_count ||
        normalized.chunks < otlp->chunk_high || normalized.staging_bytes < otlp->spans_used ||
        entry_capacity < otlp->entry_count || normalized.scopes < otlp->scope_count ||
        normalized.log_bytes < otlp->logs_used || normalized.resource_bytes < otlp->resource_used ||
        normalized.metric_bytes < otlp->metrics_used ||
        normalized.span_metric_charts < otlp->duration_count)
        return TA_ERROR_INVALID;
    ta_otlp *moved = otlp_place(memory.pointer, memory.length, &normalized);
    if (!moved) return TA_ERROR_NO_MEMORY;
    /* the staging, the scopes, the logs, the resource and the chunks move as
     * they are (indices stay); the open sessions are added again (the index
     * is sized by their capacity) */
    ta_memcpy(OTLP_POINTER(moved, spans_relative, void), OTLP_POINTER(otlp, spans_relative, void),
              otlp->spans_used);
    ta_memcpy(OTLP_POINTER(moved, entries_relative, void),
              OTLP_POINTER(otlp, entries_relative, void), sizeof(otlp_entry) * otlp->entry_count);
    ta_memcpy(OTLP_POINTER(moved, scopes_relative, void), OTLP_POINTER(otlp, scopes_relative, void),
              sizeof(otlp_scope) * otlp->scope_count);
    ta_memcpy(OTLP_POINTER(moved, scope_bytes_relative, void),
              OTLP_POINTER(otlp, scope_bytes_relative, void), otlp->scope_bytes_used);
    ta_memcpy(OTLP_POINTER(moved, logs_relative, void), OTLP_POINTER(otlp, logs_relative, void),
              otlp->logs_used);
    ta_memcpy(OTLP_POINTER(moved, resource_relative, void),
              OTLP_POINTER(otlp, resource_relative, void), otlp->resource_used);
    ta_memcpy(OTLP_POINTER(moved, metrics_relative, void),
              OTLP_POINTER(otlp, metrics_relative, void), otlp->metrics_used);
    ta_memcpy(OTLP_POINTER(moved, durations_relative, void),
              OTLP_POINTER(otlp, durations_relative, void),
              sizeof(otlp_duration) * otlp->duration_count);
    moved->duration_count = otlp->duration_count;
    moved->metrics_used = otlp->metrics_used;
    moved->metric_count = otlp->metric_count;
    ta_memcpy(OTLP_POINTER(moved, chunks_relative, void), OTLP_POINTER(otlp, chunks_relative, void),
              sizeof(otlp_chunk) * otlp->chunk_high);
    ta_memcpy(OTLP_POINTER(moved, skips_relative, void), OTLP_POINTER(otlp, skips_relative, void),
              sizeof(otlp_skip) * otlp->skip_count);
    moved->chunk_free = otlp->chunk_free;
    moved->chunk_high = otlp->chunk_high;
    moved->chunks_used = otlp->chunks_used;
    moved->spans_used = otlp->spans_used;
    moved->entry_count = otlp->entry_count;
    moved->scope_count = otlp->scope_count;
    moved->scope_bytes_used = otlp->scope_bytes_used;
    moved->logs_used = otlp->logs_used;
    moved->log_count = otlp->log_count;
    moved->resource_used = otlp->resource_used;
    moved->span_count = otlp->span_count;
    moved->skip_count = otlp->skip_count;
    moved->mark_head = otlp->mark_head;
    moved->mark_count = otlp->mark_count;
    ta_memcpy(moved->mark_fuel, otlp->mark_fuel, sizeof moved->mark_fuel);
    ta_memcpy(moved->mark_wall, otlp->mark_wall, sizeof moved->mark_wall);
    moved->records = otlp->records;
    moved->dropped = otlp->dropped;
    moved->skipped = otlp->skipped;
    moved->truncated = otlp->truncated;
    for (uint32_t i = 0; i < otlp->config.open_spans; i++) {
        const otlp_open *open = otlp_open_at(otlp, i);
        if (!open->used) continue;
        otlp_open *added = otlp_open_add(moved, open->session, open->source, open->chart);
        *added = *open; /* chains keep their chunk indices */
    }
    otlp->magic = 0; /* the old block is dead */
    *relocated = moved;
    return TA_OK;
}

/* ---- metrics (docs/adr/0007) ----------------------------------------------------------------
 * Each added metric is one encoded Metric message in the metrics staging;
 * a request puts them under one ScopeMetrics. Sizes are computed before
 * anything is written, so an add is whole or not at all. */

static void otlp_put_double(otlp_out *out, double value) {
    uint64_t bits;
    ta_memcpy(&bits, &value, sizeof bits);
    otlp_put_fixed64(out, bits);
}

#define OTLP_FIXED_FIELD    9u  /* a tag and 8 bytes */
#define OTLP_EXEMPLAR_BYTES 46u /* time, value, span ID (8), trace ID (16) */

static uint32_t otlp_string_attribute_size(const ta_otlp_attribute *attribute) {
    return otlp_key_value_size(attribute->key, otlp_any_string_size(attribute->value.length));
}

static uint64_t otlp_string_attributes_size(const ta_otlp_attribute *attributes, uint32_t count,
                                            uint32_t field) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < count; i++)
        size += otlp_field_size(field, otlp_string_attribute_size(&attributes[i]));
    return size;
}

static void otlp_string_attributes_put(otlp_out *out, uint32_t field,
                                       const ta_otlp_attribute *attributes, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const ta_otlp_attribute *attribute = &attributes[i];
        otlp_put_length(out, field, otlp_string_attribute_size(attribute));
        otlp_put_length(out, F_KEY_VALUE_KEY, attribute->key.length);
        otlp_put_bytes(out, attribute->key.pointer, attribute->key.length);
        otlp_put_length(out, F_KEY_VALUE_VALUE, otlp_any_string_size(attribute->value.length));
        otlp_put_length(out, F_ANY_STRING, attribute->value.length);
        otlp_put_bytes(out, attribute->value.pointer, attribute->value.length);
    }
}

static void otlp_exemplars_put(otlp_out *out, uint32_t field, const ta_otlp_exemplar *exemplars,
                               uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const ta_otlp_exemplar *exemplar = &exemplars[i];
        otlp_put_length(out, field, OTLP_EXEMPLAR_BYTES);
        otlp_put_tag(out, F_EXEMPLAR_TIME, WIRE_I64);
        otlp_put_fixed64(out, exemplar->time);
        otlp_put_tag(out, F_EXEMPLAR_DOUBLE, WIRE_I64);
        otlp_put_double(out, exemplar->value);
        otlp_put_length(out, F_EXEMPLAR_SPAN_ID, 8);
        otlp_put_id64(out, exemplar->span);
        otlp_put_length(out, F_EXEMPLAR_TRACE_ID, 16);
        otlp_put_id64(out, exemplar->trace.high);
        otlp_put_id64(out, exemplar->trace.low);
    }
}

/* The bytes of one data point (without its field head). */
static uint64_t otlp_point_size(const ta_otlp_metric *metric, const ta_otlp_point *point) {
    uint64_t size = 2u * OTLP_FIXED_FIELD; /* start, time (a gauge has no start) */
    if (metric->kind == TA_OTLP_METRIC_GAUGE) size -= OTLP_FIXED_FIELD;
    if (metric->kind == TA_OTLP_METRIC_HISTOGRAM) {
        size += 2u * OTLP_FIXED_FIELD; /* count, sum */
        size += otlp_field_size(F_HISTOGRAM_BUCKETS, 8u * point->bucket_count);
        if (point->bucket_count > 1)
            size += otlp_field_size(F_HISTOGRAM_BOUNDS, 8u * (point->bucket_count - 1u));
        if (point->count) size += 2u * OTLP_FIXED_FIELD; /* min, max */
        size += (uint64_t)point->exemplar_count *
                otlp_field_size(F_HISTOGRAM_EXEMPLARS, OTLP_EXEMPLAR_BYTES);
        return size + otlp_string_attributes_size(point->attributes, point->attribute_count,
                                                  F_HISTOGRAM_ATTRIBUTES);
    }
    size += OTLP_FIXED_FIELD; /* the value */
    size +=
        (uint64_t)point->exemplar_count * otlp_field_size(F_NUMBER_EXEMPLARS, OTLP_EXEMPLAR_BYTES);
    return size + otlp_string_attributes_size(point->attributes, point->attribute_count,
                                              F_NUMBER_ATTRIBUTES);
}

static void otlp_point_put(otlp_out *out, const ta_otlp_metric *metric,
                           const ta_otlp_point *point) {
    if (metric->kind == TA_OTLP_METRIC_HISTOGRAM) {
        otlp_put_tag(out, F_HISTOGRAM_START, WIRE_I64);
        otlp_put_fixed64(out, metric->start_ns);
        otlp_put_tag(out, F_HISTOGRAM_TIME, WIRE_I64);
        otlp_put_fixed64(out, metric->time_ns);
        otlp_put_tag(out, F_HISTOGRAM_COUNT, WIRE_I64);
        otlp_put_fixed64(out, point->count);
        otlp_put_tag(out, F_HISTOGRAM_SUM, WIRE_I64);
        otlp_put_double(out, point->sum);
        otlp_put_length(out, F_HISTOGRAM_BUCKETS, 8u * point->bucket_count);
        for (uint32_t i = 0; i < point->bucket_count; i++)
            otlp_put_fixed64(out, point->buckets[i]);
        if (point->bucket_count > 1) {
            otlp_put_length(out, F_HISTOGRAM_BOUNDS, 8u * (point->bucket_count - 1u));
            for (uint32_t i = 0; i + 1u < point->bucket_count; i++)
                otlp_put_double(out, point->bounds[i]);
        }
        otlp_exemplars_put(out, F_HISTOGRAM_EXEMPLARS, point->exemplars, point->exemplar_count);
        otlp_string_attributes_put(out, F_HISTOGRAM_ATTRIBUTES, point->attributes,
                                   point->attribute_count);
        if (point->count) {
            otlp_put_tag(out, F_HISTOGRAM_MIN, WIRE_I64);
            otlp_put_double(out, point->min);
            otlp_put_tag(out, F_HISTOGRAM_MAX, WIRE_I64);
            otlp_put_double(out, point->max);
        }
        return;
    }
    if (metric->kind == TA_OTLP_METRIC_SUM) {
        otlp_put_tag(out, F_NUMBER_START, WIRE_I64);
        otlp_put_fixed64(out, metric->start_ns);
    }
    otlp_put_tag(out, F_NUMBER_TIME, WIRE_I64);
    otlp_put_fixed64(out, metric->time_ns);
    if (point->flags & TA_OTLP_POINT_DOUBLE) {
        otlp_put_tag(out, F_NUMBER_DOUBLE, WIRE_I64);
        otlp_put_double(out, point->number);
    } else {
        otlp_put_tag(out, F_NUMBER_INT, WIRE_I64);
        otlp_put_fixed64(out, (uint64_t)point->integer);
    }
    otlp_exemplars_put(out, F_NUMBER_EXEMPLARS, point->exemplars, point->exemplar_count);
    otlp_string_attributes_put(out, F_NUMBER_ATTRIBUTES, point->attributes, point->attribute_count);
}

static uint32_t otlp_data_field(uint32_t kind) {
    return kind == TA_OTLP_METRIC_SUM     ? F_METRIC_SUM
           : kind == TA_OTLP_METRIC_GAUGE ? F_METRIC_GAUGE
                                          : F_METRIC_HISTOGRAM;
}

/* The bytes of the Gauge, Sum or Histogram message. */
static uint64_t otlp_data_size(const ta_otlp_metric *metric) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < metric->point_count; i++) {
        uint64_t point = otlp_point_size(metric, &metric->points[i]);
        if (point > 0x7FFFFFFFu) return UINT64_MAX;
        size += otlp_field_size(F_DATA_POINTS, (uint32_t)point);
    }
    if (metric->kind == TA_OTLP_METRIC_SUM) size += 4u; /* temporality, monotonic */
    if (metric->kind == TA_OTLP_METRIC_HISTOGRAM) size += 2u;
    return size;
}

static uint64_t otlp_metric_size(const ta_otlp_metric *metric, uint64_t data) {
    uint64_t size = otlp_field_size(F_METRIC_NAME, metric->name.length);
    if (metric->description.length)
        size += otlp_field_size(F_METRIC_DESCRIPTION, metric->description.length);
    if (metric->unit.length) size += otlp_field_size(F_METRIC_UNIT, metric->unit.length);
    return size + otlp_field_size(otlp_data_field(metric->kind), (uint32_t)data);
}

ta_status ta_otlp_add_metric(ta_otlp *otlp, const ta_otlp_metric *metric) {
    if (metric->kind < TA_OTLP_METRIC_SUM || metric->kind > TA_OTLP_METRIC_HISTOGRAM)
        return TA_ERROR_INVALID;
    for (uint32_t i = 0; i < metric->point_count; i++) {
        const ta_otlp_point *point = &metric->points[i];
        if ((point->attribute_count && !point->attributes) ||
            (point->exemplar_count && !point->exemplars) ||
            (metric->kind == TA_OTLP_METRIC_HISTOGRAM && point->bucket_count &&
             (!point->buckets || (point->bucket_count > 1 && !point->bounds))))
            return TA_ERROR_INVALID;
    }
    uint64_t data = otlp_data_size(metric);
    if (data > 0x7FFFFFFFu) return TA_ERROR_NO_MEMORY;
    uint64_t body = otlp_metric_size(metric, data);
    if (body > 0x7FFFFFFFu) return TA_ERROR_NO_MEMORY;
    uint64_t entry = otlp_field_size(F_SS_ITEMS, (uint32_t)body);
    if (entry > (uint64_t)otlp->config.metric_bytes - otlp->metrics_used) return TA_ERROR_NO_MEMORY;
    unsigned char *at = OTLP_POINTER(otlp, metrics_relative, unsigned char) + otlp->metrics_used;
    otlp_out out = {at, at + entry};
    otlp_put_length(&out, F_SS_ITEMS, (uint32_t)body);
    otlp_put_length(&out, F_METRIC_NAME, metric->name.length);
    otlp_put_bytes(&out, metric->name.pointer, metric->name.length);
    if (metric->description.length) {
        otlp_put_length(&out, F_METRIC_DESCRIPTION, metric->description.length);
        otlp_put_bytes(&out, metric->description.pointer, metric->description.length);
    }
    if (metric->unit.length) {
        otlp_put_length(&out, F_METRIC_UNIT, metric->unit.length);
        otlp_put_bytes(&out, metric->unit.pointer, metric->unit.length);
    }
    otlp_put_length(&out, otlp_data_field(metric->kind), (uint32_t)data);
    for (uint32_t i = 0; i < metric->point_count; i++) {
        otlp_put_length(&out, F_DATA_POINTS, (uint32_t)otlp_point_size(metric, &metric->points[i]));
        otlp_point_put(&out, metric, &metric->points[i]);
    }
    if (metric->kind != TA_OTLP_METRIC_GAUGE) {
        otlp_put_tag(&out, F_TEMPORALITY, WIRE_VARINT);
        otlp_put_varint(&out, TEMPORALITY_CUMULATIVE);
    }
    if (metric->kind == TA_OTLP_METRIC_SUM) {
        otlp_put_tag(&out, F_MONOTONIC, WIRE_VARINT);
        otlp_put_varint(&out, 1u);
    }
    TA_ASSERT(out.at == out.end);
    otlp->metrics_used += (uint32_t)entry;
    otlp->metric_count++;
    return TA_OK;
}

/* the metrics' one scope: tinyactors */
static uint32_t otlp_metrics_scope_size(const ta_otlp *otlp) {
    uint32_t scope = otlp_field_size(F_SCOPE_NAME, 10) +
                     otlp_field_size(F_SCOPE_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    return otlp_field_size(F_RS_SCOPES, otlp_field_size(F_SS_SCOPE, scope) + otlp->metrics_used);
}

static size_t otlp_metrics_size(const ta_otlp *otlp) {
    if (!otlp->metric_count) return 0;
    return otlp_field_size(F_REQUEST_RESOURCE,
                           otlp_resource_size(otlp) + otlp_metrics_scope_size(otlp));
}

ta_status ta_otlp_put_metrics(ta_otlp *otlp, ta_buffer *destination) {
    size_t size = otlp_metrics_size(otlp);
    destination->length = size;
    if (!size) return TA_OK;
    if (destination->capacity < size) return TA_ERROR_NO_MEMORY;
    unsigned char *buffer = (unsigned char *)destination->pointer;
    otlp_out out = {buffer, buffer + size};
    otlp_put_length(&out, F_REQUEST_RESOURCE,
                    otlp_resource_size(otlp) + otlp_metrics_scope_size(otlp));
    otlp_put_resource(otlp, &out);
    uint32_t scope = otlp_field_size(F_SCOPE_NAME, 10) +
                     otlp_field_size(F_SCOPE_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    otlp_put_length(&out, F_RS_SCOPES, otlp_field_size(F_SS_SCOPE, scope) + otlp->metrics_used);
    otlp_put_length(&out, F_SS_SCOPE, scope);
    otlp_put_length(&out, F_SCOPE_NAME, 10);
    otlp_put_bytes(&out, "tinyactors", 10);
    otlp_put_length(&out, F_SCOPE_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    otlp_put_bytes(&out, OTLP_VERSION, (uint32_t)sizeof OTLP_VERSION - 1u);
    otlp_put_bytes(&out, OTLP_POINTER(otlp, metrics_relative, unsigned char), otlp->metrics_used);
    TA_ASSERT(out.at == out.end);
    otlp->metrics_used = otlp->metric_count = 0;
    return TA_OK;
}

/* ---- the stats as metrics ---------------------------------------------------------------- */

/* Points gathered in the scratch area, with up to two attributes each. */
typedef struct otlp_points {
    ta_otlp_point *points;
    ta_otlp_attribute *attributes;
    uint32_t count, capacity;
    ta_otlp_metric metric;
} otlp_points;

static void otlp_points_begin(ta_otlp *otlp, otlp_points *gather, const char *name,
                              const char *unit, uint32_t kind, const ta_otlp_stats_input *input) {
    gather->points = OTLP_POINTER(otlp, scratch_relative, ta_otlp_point);
    gather->capacity =
        otlp->scratch_bytes / (uint32_t)(sizeof(ta_otlp_point) + 2u * sizeof(ta_otlp_attribute));
    gather->attributes = (ta_otlp_attribute *)(gather->points + gather->capacity);
    gather->count = 0;
    ta_memset(&gather->metric, 0, sizeof gather->metric);
    gather->metric.name.pointer = name;
    while (name[gather->metric.name.length])
        gather->metric.name.length++;
    gather->metric.unit.pointer = unit;
    while (unit[gather->metric.unit.length])
        gather->metric.unit.length++;
    gather->metric.kind = kind;
    gather->metric.start_ns = input->start_ns;
    gather->metric.time_ns = input->time_ns;
}

static ta_status otlp_points_end(ta_otlp *otlp, otlp_points *gather) {
    gather->metric.points = gather->points;
    gather->metric.point_count = gather->count;
    ta_status status = gather->count ? ta_otlp_add_metric(otlp, &gather->metric) : TA_OK;
    gather->count = 0;
    return status;
}

/* One more point; a full scratch area writes what it has as a metric first
 * (the same name twice: a reader merges them). */
static ta_status otlp_points_add(ta_otlp *otlp, otlp_points *gather, int64_t integer, double number,
                                 uint32_t flags, ta_string_view key, ta_string_view value,
                                 const char *key2, const char *value2) {
    if (gather->count == gather->capacity) {
        ta_status status = otlp_points_end(otlp, gather);
        if (status != TA_OK) return status;
    }
    ta_otlp_point *point = &gather->points[gather->count];
    ta_otlp_attribute *attributes = &gather->attributes[2u * gather->count];
    ta_memset(point, 0, sizeof *point);
    point->integer = integer;
    point->number = number;
    point->flags = flags;
    point->attributes = attributes;
    if (key.length) attributes[point->attribute_count++] = (ta_otlp_attribute){key, value};
    if (key2) {
        ta_string_view k = {key2, 0}, v = {value2, 0};
        while (key2[k.length])
            k.length++;
        while (value2[v.length])
            v.length++;
        attributes[point->attribute_count++] = (ta_otlp_attribute){k, v};
    }
    gather->count++;
    return TA_OK;
}

static uint64_t otlp_stats_field(const void *stats, uint32_t offset, uint32_t width) {
    if (width == 4) {
        uint32_t value;
        ta_memcpy(&value, (const unsigned char *)stats + offset, 4);
        return value;
    }
    uint64_t value;
    ta_memcpy(&value, (const unsigned char *)stats + offset, 8);
    return value;
}

#define OTLP_FIELD(type, field)                                                                    \
    (uint32_t)offsetof(type, field), (uint32_t)sizeof(((type *)0)->field)

/* A series of the stats: a metric name and, for one of several series of a
 * name, the attribute that tells them apart. Series of one name follow each
 * other. A width of 0 is a value computed from a system's stats: */
enum { OTLP_SLAB_USED = 1, OTLP_SLAB_LIMIT = 2, OTLP_OVERDUE = 3 };
typedef struct otlp_series {
    const char *name, *unit;
    uint32_t kind;
    uint32_t offset, width;
    const char *key, *value;
} otlp_series;

static const otlp_series OTLP_CHART_SERIES[] = {
    {"tinyactors.actors", "{actor}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_chart_stats, actors),
     "tinyactors.actor.state", "resident"},
    {"tinyactors.actors", "{actor}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_chart_stats, hibernated),
     "tinyactors.actor.state", "hibernated"},
    {"tinyactors.actors.spawned", "{actor}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_chart_stats, spawned), 0, 0},
    {"tinyactors.actors.ended", "{actor}", TA_OTLP_METRIC_SUM, OTLP_FIELD(ta_chart_stats, ended), 0,
     0},
    {"tinyactors.macrosteps", "{step}", TA_OTLP_METRIC_SUM, OTLP_FIELD(ta_chart_stats, macrosteps),
     0, 0},
    {"tinyactors.fuel", "{fuel}", TA_OTLP_METRIC_SUM, OTLP_FIELD(ta_chart_stats, fuel), 0, 0},
    {"tinyactors.errors", "{error}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_chart_stats, errors_execution), "scxml.error.type", "execution"},
    {"tinyactors.errors", "{error}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_chart_stats, errors_communication), "scxml.error.type", "communication"},
    {"tinyactors.errors", "{error}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_chart_stats, errors_platform), "scxml.error.type", "platform"},
    {"tinyactors.faults", "{actor}", TA_OTLP_METRIC_SUM, OTLP_FIELD(ta_chart_stats, faults), 0, 0},
};

static const otlp_series OTLP_SYSTEM_SERIES[] = {
    {"tinyactors.pool.usage", "{item}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_system_stats, actors),
     "tinyactors.pool", "actors"},
    {"tinyactors.pool.usage", "{item}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_system_stats, events),
     "tinyactors.pool", "events"},
    {"tinyactors.pool.usage", "{item}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_system_stats, timers),
     "tinyactors.pool", "timers"},
    {"tinyactors.pool.usage", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, workspaces), "tinyactors.pool", "workspaces"},
    {"tinyactors.pool.usage", "{item}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_system_stats, requests),
     "tinyactors.pool", "requests"},
    {"tinyactors.pool.usage", "{item}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_system_stats, outbox),
     "tinyactors.pool", "outbox"},
    {"tinyactors.pool.usage", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, trace_contexts), "tinyactors.pool", "trace.contexts"},
    {"tinyactors.pool.limit", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, actors_capacity), "tinyactors.pool", "actors"},
    {"tinyactors.pool.limit", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, events_capacity), "tinyactors.pool", "events"},
    {"tinyactors.pool.limit", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, timers_capacity), "tinyactors.pool", "timers"},
    {"tinyactors.pool.limit", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, workspaces_capacity), "tinyactors.pool", "workspaces"},
    {"tinyactors.pool.limit", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, requests_capacity), "tinyactors.pool", "requests"},
    {"tinyactors.pool.limit", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, outbox_capacity), "tinyactors.pool", "outbox"},
    {"tinyactors.pool.limit", "{item}", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, trace_contexts_capacity), "tinyactors.pool", "trace.contexts"},
    {"tinyactors.memory.usage", "By", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, region_bytes), "tinyactors.memory", "region"},
    {"tinyactors.memory.usage", "By", TA_OTLP_METRIC_GAUGE, OTLP_SLAB_USED, 0, "tinyactors.memory",
     "slab"},
    {"tinyactors.memory.usage", "By", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, trace_bytes), "tinyactors.memory", "trace.journal"},
    {"tinyactors.memory.limit", "By", TA_OTLP_METRIC_GAUGE, OTLP_SLAB_LIMIT, 0, "tinyactors.memory",
     "slab"},
    {"tinyactors.memory.limit", "By", TA_OTLP_METRIC_GAUGE,
     OTLP_FIELD(ta_system_stats, trace_bytes_capacity), "tinyactors.memory", "trace.journal"},
    {"tinyactors.timers.overdue", "s", TA_OTLP_METRIC_GAUGE, OTLP_OVERDUE, 0, 0, 0},
    {"tinyactors.runnable", "{actor}", TA_OTLP_METRIC_GAUGE, OTLP_FIELD(ta_system_stats, runnable),
     0, 0},
    {"tinyactors.events.delivered", "{event}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_system_stats, delivered), 0, 0},
    {"tinyactors.trace.records.lost", "{record}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_system_stats, trace_records_lost), 0, 0},
    {"tinyactors.trace.contexts.dropped", "{context}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_system_stats, trace_contexts_dropped), 0, 0},
    {"tinyactors.traces", "{trace}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_system_stats, traces_started), "tinyactors.trace.decision", "started"},
    {"tinyactors.traces", "{trace}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_system_stats, traces_sampled), "tinyactors.trace.decision", "sampled"},
};

static const otlp_series OTLP_HOST_SERIES[] = {
    {"tinyactors.messages.routed", "{message}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_host_stats, routed), 0, 0},
    {"tinyactors.actors.migrated", "{actor}", TA_OTLP_METRIC_SUM,
     OTLP_FIELD(ta_host_stats, migrated), 0, 0},
};

static int otlp_same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Series of the same name are one metric: write each when its last series is in. */
static ta_status otlp_series_put(ta_otlp *otlp, const ta_otlp_stats_input *input,
                                 const otlp_series *series, uint32_t count, const void *stats,
                                 uint32_t stats_count, uint32_t stats_size, int per_chart) {
    otlp_points gather;
    for (uint32_t s = 0; s < count; s++) {
        const otlp_series *one = &series[s];
        if (s == 0 || !otlp_same(series[s - 1].name, one->name))
            otlp_points_begin(otlp, &gather, one->name, one->unit, one->kind, input);
        if (per_chart) {
            for (uint32_t c = 0; c < stats_count; c++) {
                const ta_chart_stats *chart = &((const ta_chart_stats *)stats)[c];
                uint64_t value = otlp_stats_field(chart, one->offset, one->width);
                if (one->offset == offsetof(ta_chart_stats, actors)) value -= chart->hibernated;
                ta_status status = otlp_points_add(otlp, &gather, (int64_t)value, 0, 0,
                                                   TEXT("tinyactors.chart.name"), chart->name,
                                                   one->key, one->value);
                if (status != TA_OK) return status;
            }
        } else {
            uint64_t sum = 0, overdue = 0;
            for (uint32_t i = 0; i < stats_count; i++) {
                const void *one_stats = (const unsigned char *)stats + i * stats_size;
                if (one->width) {
                    sum += otlp_stats_field(one_stats, one->offset, one->width);
                    continue;
                }
                const ta_system_stats *system = (const ta_system_stats *)one_stats;
                if (one->offset == OTLP_SLAB_USED)
                    sum += (uint64_t)system->slab_pages * system->slab_page_bytes;
                else if (one->offset == OTLP_SLAB_LIMIT)
                    sum += (uint64_t)system->slab_pages_capacity * system->slab_page_bytes;
                else if (system->next_deadline != UINT64_MAX &&
                         system->now > system->next_deadline &&
                         system->now - system->next_deadline > overdue)
                    overdue = system->now - system->next_deadline; /* the latest of them */
            }
            ta_string_view none = {(const char *)0, 0};
            double seconds =
                input->ticks_per_second ? (double)overdue / (double)input->ticks_per_second : 0.0;
            ta_status status =
                one->offset == OTLP_OVERDUE && !one->width
                    ? otlp_points_add(otlp, &gather, 0, seconds, TA_OTLP_POINT_DOUBLE, none, none,
                                      one->key, one->value)
                    : otlp_points_add(otlp, &gather, (int64_t)sum, 0, 0, none, none, one->key,
                                      one->value);
            if (status != TA_OK) return status;
        }
        if (s + 1 == count || !otlp_same(series[s + 1].name, one->name)) {
            ta_status status = otlp_points_end(otlp, &gather);
            if (status != TA_OK) return status;
        }
    }
    return TA_OK;
}

static ta_status otlp_stats_put(ta_otlp *otlp, const ta_otlp_stats_input *input) {
    ta_status status =
        otlp_series_put(otlp, input, OTLP_CHART_SERIES,
                        (uint32_t)(sizeof OTLP_CHART_SERIES / sizeof OTLP_CHART_SERIES[0]),
                        input->charts, input->chart_count, (uint32_t)sizeof(ta_chart_stats), 1);
    if (status != TA_OK || !input->system_count) return status;
    status =
        otlp_series_put(otlp, input, OTLP_SYSTEM_SERIES,
                        (uint32_t)(sizeof OTLP_SYSTEM_SERIES / sizeof OTLP_SYSTEM_SERIES[0]),
                        input->systems, input->system_count, (uint32_t)sizeof(ta_system_stats), 0);
    if (status != TA_OK || !input->host) return status;
    return otlp_series_put(otlp, input, OTLP_HOST_SERIES,
                           (uint32_t)(sizeof OTLP_HOST_SERIES / sizeof OTLP_HOST_SERIES[0]),
                           input->host, 1, (uint32_t)sizeof(ta_host_stats), 0);
}

ta_status ta_otlp_add_stats(ta_otlp *otlp, const ta_otlp_stats_input *input) {
    if ((input->chart_count && !input->charts) || (input->system_count && !input->systems))
        return TA_ERROR_INVALID;
    uint32_t used = otlp->metrics_used, count = otlp->metric_count;
    ta_status status = otlp_stats_put(otlp, input);
    if (status != TA_OK) { /* whole or not at all */
        otlp->metrics_used = used;
        otlp->metric_count = count;
    }
    return status;
}

ta_status ta_otlp_add_span_metrics(ta_otlp *otlp, uint64_t start_ns, uint64_t time_ns) {
    uint32_t count = otlp->duration_count;
    if (!count) return TA_OK;
    /* points, attributes and exemplars in its scratch area */
    ta_otlp_exemplar *exemplars = OTLP_POINTER(otlp, scratch_relative, ta_otlp_exemplar);
    ta_otlp_point *points = (ta_otlp_point *)(exemplars + count * OTLP_DURATION_BUCKETS);
    ta_otlp_attribute *attributes = (ta_otlp_attribute *)(points + count);
    uint64_t need = (uint64_t)count * (sizeof *points + sizeof *attributes +
                                       OTLP_DURATION_BUCKETS * sizeof *exemplars);
    if (need > otlp->scratch_bytes) return TA_ERROR_NO_MEMORY;
    const otlp_duration *table = otlp_durations(otlp);
    for (uint32_t i = 0; i < count; i++) {
        const otlp_duration *entry = &table[i];
        ta_otlp_point *point = &points[i];
        ta_memset(point, 0, sizeof *point);
        attributes[i].key = TEXT("tinyactors.chart.name");
        attributes[i].value = (ta_string_view){entry->name, entry->name_length};
        point->attributes = &attributes[i];
        point->attribute_count = 1;
        point->count = entry->count;
        point->sum = entry->sum;
        point->min = entry->min;
        point->max = entry->max;
        point->bounds = OTLP_DURATION_BOUNDS;
        point->buckets = entry->buckets;
        point->bucket_count = OTLP_DURATION_BUCKETS;
        ta_otlp_exemplar *kept = &exemplars[i * OTLP_DURATION_BUCKETS];
        point->exemplars = kept;
        for (uint32_t b = 0; b < OTLP_DURATION_BUCKETS; b++)
            if (entry->exemplars[b].time) kept[point->exemplar_count++] = entry->exemplars[b];
    }
    ta_otlp_metric metric;
    ta_memset(&metric, 0, sizeof metric);
    metric.name = TEXT("tinyactors.macrostep.duration");
    metric.unit = TEXT("s");
    metric.kind = TA_OTLP_METRIC_HISTOGRAM;
    metric.points = points;
    metric.point_count = count;
    metric.start_ns = start_ns;
    metric.time_ns = time_ns;
    return ta_otlp_add_metric(otlp, &metric);
}

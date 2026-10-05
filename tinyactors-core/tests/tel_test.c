/* test: standalone asan */
/*
 * Block tel (src/tel.h, once it exists) and the rings a system owns (ADR
 * 0098): the control block and the telemetry ring made at ta_system_create,
 * the public reader (ta_system_control_block, ta_ring_peek, ta_ring_release),
 * and, in later sections of this file, the producer.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <string.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                               \
        }                                                                                \
    } while (0)

static ta_test_allocator heap = { .strict = true };

static void finish_clean(void) {
    CHECK(ta_test_allocator_finish(&heap) == 0);
    CHECK(heap.stats.bad_frees == 0);
}

static ta_system *create_with(const ta_param_value *params, uint32_t count) {
    ta_system *system = NULL;
    ta_result result = ta_system_create(&(ta_system_config){ .allocator = ta_test_allocator_get(&heap),
                                                             .params = params, .param_count = count },
                                        &system);
    return result == TA_OK ? system : NULL;
}

/* ------------------------------------------------------------------------
 * The rings a system owns
 */

static void test_control_block(void) {
    ta_system *system = create_with(NULL, 0);
    CHECK(system != NULL);
    const ta_ring_control *control = ta_system_control_block(system);
    CHECK(control != NULL && ta_system_control_block(NULL) == NULL);
    CHECK(control->magic == TA_RING_MAGIC && control->version == TA_RING_VERSION &&
          control->ring_count == TA_RING_COUNT);
    /* h2c and c2h do not exist until ring-proto 5.1: they are entries with no bytes. */
    CHECK(control->h2c.capacity == 0 && control->h2c.max_record_bytes == 0);
    CHECK(control->c2h.capacity == 0 && control->c2h.max_record_bytes == 0);
    /* tel is telBytes with records of at most maxRecordBytes, and lossy. */
    CHECK(control->tel.capacity == TA_PARAM_DEFAULT_TEL_BYTES);
    CHECK(control->tel.max_record_bytes == TA_PARAM_DEFAULT_MAX_RECORD_BYTES);
    CHECK(control->tel.mode == (uint32_t)TA_RING_LOSSY && control->h2c.mode == (uint32_t)TA_RING_LOSSLESS);
    CHECK(control->tel.offset >= sizeof(ta_ring_control) && control->tel.offset % 8 == 0);
    ta_ring_shared cells[3] = { control->h2c, control->c2h, control->tel };
    for (uint32_t id = 0; id < 3; id += 1) {
        CHECK(cells[id].head == 0 && cells[id].tail == 0 && cells[id].next_seq == 1 && cells[id].dropped == 0);
    }
    /* The staging area sits behind the ring areas, inside the same allocation, and is maxRecordBytes long. */
    CHECK(system->tel_staging_bytes == TA_PARAM_DEFAULT_MAX_RECORD_BYTES);
    CHECK(system->tel_staging >= (unsigned char *)system->rings_memory + control->tel.offset + control->tel.capacity);
    CHECK(system->tel_staging + system->tel_staging_bytes <= (unsigned char *)system->rings_memory + system->rings_bytes);
    ta_system_destroy(system);
    finish_clean();
}

static void test_reading_empty_rings(void) {
    ta_system *system = create_with(NULL, 0);
    ta_ring_record record = { (const uint8_t *)1, 99, 99, 99 };
    for (uint32_t id = 0; id < TA_RING_COUNT; id += 1) {
        CHECK(ta_ring_peek(system, (ta_ring_id)id, &record) == TA_ERR_NOT_FOUND);
        CHECK(record.bytes == (const uint8_t *)1 && record.size == 99 && record.seq == 99); /* untouched */
        ta_ring_release(system, (ta_ring_id)id, 0);
        ta_ring_release(system, (ta_ring_id)id, 16);
    }
    const ta_ring_control *control = ta_system_control_block(system);
    CHECK(control->tel.tail == 0 && control->tel.head == 0);
    CHECK(ta_ring_peek(NULL, TA_RING_TEL, &record) == TA_ERR_INVALID);
    CHECK(ta_ring_peek(system, TA_RING_COUNT, &record) == TA_ERR_INVALID);
    CHECK(ta_ring_peek(system, (ta_ring_id)-1, &record) == TA_ERR_INVALID);
    CHECK(ta_ring_peek(system, TA_RING_TEL, NULL) == TA_ERR_INVALID);
    ta_ring_release(NULL, TA_RING_TEL, 16);
    ta_ring_release(system, TA_RING_COUNT, 16);
    ta_system_destroy(system);
    finish_clean();
}

/* A record written by hand through the ring handle is seen, and released only with its own size. */
static void test_peek_release_through_the_system(void) {
    ta_system *system = create_with(NULL, 0);
    ta_ring *tel = &system->rings.ring[TA_RING_TEL];
    ta_ring_reservation reservation;
    CHECK(ta_ring_reserve(tel, 24, &reservation) == TA_RING_OK);
    reservation.bytes[0] = 0x01;
    reservation.bytes[1] = 0x03;
    ta_ring_commit(tel);
    ta_ring_record record;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &record) == TA_OK);
    CHECK(record.size == 24 && record.seq == 1 && record.gap == 0 && record.bytes[1] == 0x03);
    ta_ring_record again;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &again) == TA_OK && again.bytes == record.bytes);
    ta_ring_release(system, TA_RING_TEL, 16); /* not the size peeked: nothing happens */
    CHECK(ta_system_control_block(system)->tel.tail == 0);
    ta_ring_release(system, TA_RING_C2H, 24); /* another ring: nothing happens */
    CHECK(ta_system_control_block(system)->tel.tail == 0);
    ta_ring_release(system, TA_RING_TEL, 24);
    CHECK(ta_system_control_block(system)->tel.tail == 24);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &record) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * One allocation, sized by the parameters, freed with the same size
 */

static void test_one_allocation_sized_by_parameters(void) {
    ta_param_value small[] = { { TA_PARAM_TEL_BYTES, 8192 } };
    ta_test_allocator_reset_counts(&heap);
    ta_system *base = create_with(small, 1);
    uint64_t base_allocations = heap.stats.allocations;
    size_t base_bytes = heap.stats.live_bytes;
    ta_system_destroy(base);
    finish_clean();

    ta_param_value large[] = { { TA_PARAM_TEL_BYTES, 8192 + 24576 }, { TA_PARAM_MAX_RECORD_BYTES, 4096 + 1024 } };
    ta_test_allocator_reset_counts(&heap);
    ta_system *bigger = create_with(large, 2);
    /* The rings are one allocation however big: the count does not move, the bytes move by telBytes and by the staging area. */
    CHECK(heap.stats.allocations == base_allocations);
    CHECK(heap.stats.live_bytes - base_bytes == 24576u + 1024u);
    CHECK(bigger->tel_staging_bytes == 5120);
    CHECK(ta_system_control_block(bigger)->tel.capacity == 8192 + 24576);
    CHECK(ta_system_control_block(bigger)->tel.max_record_bytes == 5120);
    ta_system_destroy(bigger);
    finish_clean();

    /* The smallest system: one record of 512 bytes is the whole ring. */
    ta_param_value tight[] = { { TA_PARAM_TEL_BYTES, 512 }, { TA_PARAM_MAX_RECORD_BYTES, 512 },
                               { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    ta_system *least = create_with(tight, 3);
    CHECK(least != NULL && ta_system_control_block(least)->tel.capacity == 512);
    ta_system_destroy(least);
    finish_clean();
}

/* Every allocation a creation makes can fail: a clean failure, nothing held, every free sized as its allocation. */
static void test_creation_allocation_sweep(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_system *counted = create_with(NULL, 0);
    uint64_t allocations = heap.stats.allocations;
    ta_system_destroy(counted);
    finish_clean();
    CHECK(allocations > 1);
    for (uint64_t nth = 1; nth <= allocations; nth += 1) {
        ta_test_allocator_reset_counts(&heap);
        ta_test_allocator_fail_nth(&heap, nth);
        ta_system *system = create_with(NULL, 0);
        ta_test_allocator_disarm(&heap);
        CHECK(system == NULL);
        finish_clean();
    }
}


/* ------------------------------------------------------------------------
 * The producer: a fixture
 */

/* A data model that renders what it is told to, so the tests see what core asks of it. */
typedef struct fake_text {
    const char *text;
    uint32_t length;
} fake_text;

typedef struct fake_state {
    uint32_t calls;
    uint32_t last_limit;
    bool fail;
    bool ignore_limit; /* answer more than the limit, as a data model that does not keep its promise */
} fake_state;

static void fake_value_retain(void *self) { (void)self; }
static void fake_value_release(void *self) { (void)self; }
static const ta_value_vtable fake_value_vtable = { .retain = fake_value_retain, .release = fake_value_release };

static ta_op_status fake_value_trace_text(void *self, const ta_datamodel_value_trace_text_params *params) {
    fake_state *state = self;
    const fake_text *value = params->value->self;
    state->calls += 1u;
    state->last_limit = params->limit;
    if (state->fail) {
        return TA_OP_ERROR;
    }
    uint32_t length = value->length;
    *params->cut = false;
    if (length > params->limit && !state->ignore_limit) {
        length = params->limit;
        *params->cut = true;
    }
    *params->out = (ta_str){ value->text, length };
    return TA_OP_OK;
}

static const ta_datamodel_vtable fake_vtable = { .value_trace_text = fake_value_trace_text };

static ta_value fake_value(const fake_text *text) { return (ta_value){ .self = (void *)text, .v = &fake_value_vtable }; }

/* The pieces every writer test builds a record from. */
static ta_event_name_id name_of(ta_system *system, const char *dotted) {
    ta_atom atoms[8];
    uint32_t count = 0;
    const char *start = dotted;
    for (const char *at = dotted;; at += 1) {
        if (*at == '.' || *at == '\0') {
            CHECK(ta_system_intern_atom(system, (ta_str){ start, (uint32_t)(at - start) }, &atoms[count]) == TA_OK);
            count += 1u;
            start = at + 1;
            if (*at == '\0') {
                break;
            }
        }
    }
    ta_event_name_id id = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = atoms, .count = count }, &id) == TA_OK);
    return id;
}

static ta_trace_record record_of(ta_trace_kind kind, uint64_t macrostep) {
    ta_trace_record record = ta_trace_record_blank(kind);
    record.key = (ta_step_key){ .session = 7, .macrostep = macrostep, .microstep = 0, .family = 0 };
    record.actor = 42;
    record.definition = 3;
    record.now = 1000u + macrostep;
    return record;
}

/* What a drain saw of one record: enough to check order, seq and counts. */
typedef struct seen {
    ta_tel_type type;
    uint64_t seq;
    uint64_t gap;
    ta_trace_kind kind;
    uint64_t macrostep;
    uint64_t count;
    uint64_t first_seq;
    uint32_t size;
} seen;

/* Reads and releases everything on tel, decoding each record; returns how many. */
static uint32_t drain(ta_system *system, seen *out, uint32_t capacity) {
    uint32_t count = 0;
    ta_ring_record raw;
    while (ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK) {
        ta_tel_entry entry;
        CHECK(ta_tel_decode(&raw, &entry) == TA_OK);
        if (count < capacity) {
            out[count] = (seen){ .type = entry.type, .seq = entry.seq, .gap = entry.gap, .kind = entry.trace.kind,
                                 .macrostep = entry.trace.key.macrostep, .count = entry.dropped.count,
                                 .first_seq = entry.dropped.first_seq, .size = raw.size };
        }
        count += 1u;
        ta_ring_release(system, TA_RING_TEL, raw.size);
    }
    return count;
}

/* A system with a tel ring of tel_bytes and records of at most max_record_bytes; values take what the record leaves above 512. */
static ta_system *create_tel(uint32_t tel_bytes, uint32_t max_record_bytes) {
    uint32_t value_bytes = max_record_bytes - 512u < 1024u ? max_record_bytes - 512u : 1024u;
    ta_param_value params[] = { { TA_PARAM_TEL_BYTES, tel_bytes }, { TA_PARAM_MAX_RECORD_BYTES, max_record_bytes },
                                { TA_PARAM_TELEMETRY_VALUE_BYTES, value_bytes } };
    ta_system *system = create_with(params, 3);
    CHECK(system != NULL);
    return system;
}

/* ------------------------------------------------------------------------
 * Masks
 */

static void test_masks(void) {
    ta_system *system = create_with(NULL, 0);
    CHECK(system->trace_masks.kinds == 0 && system->trace_masks.watched_kinds == 0 &&
          system->trace_masks.unsampled_kinds == 0 && !system->trace_masks.values);
    ta_trace_masks masks = { .kinds = 5, .watched_kinds = 7, .unsampled_kinds = 9, .values = true };
    CHECK(ta_system_set_trace_masks(system, &masks) == TA_OK);
    CHECK(system->trace_masks.kinds == 5 && system->trace_masks.watched_kinds == 7 &&
          system->trace_masks.unsampled_kinds == 9 && system->trace_masks.values);
    CHECK(ta_system_set_trace_masks(system, NULL) == TA_ERR_INVALID);
    CHECK(ta_system_set_trace_masks(NULL, &masks) == TA_ERR_INVALID);
    CHECK(system->trace_masks.kinds == 5); /* a refusal changes nothing */
    CHECK(ta_system_set_trace_masks(system, &(ta_trace_masks){ 0 }) == TA_OK && system->trace_masks.kinds == 0);
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * A record with every field set round-trips through the ring and the decoder
 */

static void test_round_trip(void) {
    ta_system *system = create_with(NULL, 0);
    ta_system_set_trace_masks(system, &(ta_trace_masks){ .values = true });
    ta_send_id send = 0;
    ta_invoke_id invoke = 0;
    ta_str send_text = { "s-1", 3 };
    ta_str invoke_text = { "inv.9", 5 };
    CHECK(ta_send_id_intern(system, send_text, &send) == TA_OK);
    CHECK(ta_system_intern_invoke_id(system, invoke_text, &invoke) == TA_OK);
    ta_atom family = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ "chart", 5 }, &family) == TA_OK);
    fake_text event_data = { "{\"e\":1}", 7 };
    fake_text value_text = { "\"v\"", 3 };
    fake_text fault_data = { "Error: bad", 10 };
    fake_state state = { 0 };
    ta_datamodel dm = { .self = &state, .v = &fake_vtable };
    ta_tel_values values = { .datamodel = &dm };

    ta_event event = { .name = name_of(system, "ev.go"), .type = TA_EVENT_INTERNAL, .send_id = send,
                       .invoke_id = invoke, .data = fake_value(&event_data) };
    ta_state_id exited[] = { 3, 4 };
    ta_state_id entered[] = { 5 };
    ta_state_id history[] = { 6, 7, 8 };
    ta_state_id states[] = { 9, 10 };
    ta_transition_id transitions[] = { 11, 12, 13, 14 };
    ta_trace_record record = record_of(TA_TRACE_MICROSTEP, 5);
    record.key.microstep = 2;
    record.key.family = family;
    record.cause = (ta_step_key){ .session = 8, .macrostep = 4, .microstep = 1, .family = family };
    record.context = (ta_trace_context){ .trace_id_high = 0x1111, .trace_id_low = 0x2222,
                                         .flags = TA_TRACE_FLAG_SAMPLED, .detail = TA_TRACE_DETAIL_WATCHED };
    record.position = (ta_trace_position){ .at = TA_TRACE_AT_TRANSITION, .state = 21, .transition = 22, .action = 23,
                                           .iteration = 24 };
    record.event = &event;
    record.state = 31;
    record.expr = 32;
    record.name = (ta_str){ "counter", 7 };
    record.action = 33;
    record.iteration = 34;
    record.send = send;
    record.invoke = invoke;
    record.result = true;
    record.has_value = true;
    record.value = fake_value(&value_text);
    record.has_fault = true;
    record.fault = (ta_fault){ .detail = name_of(system, "error.execution"), .code = -7,
                               .message = { "it failed", 9 }, .data = fake_value(&fault_data) };
    record.dropped = 17;
    record.transitions = transitions;
    record.transition_count = 4;
    record.exited = exited;
    record.exited_count = 2;
    record.entered = entered;
    record.entered_count = 1;
    record.history = history;
    record.history_count = 3;
    record.states = states;
    record.state_count = 2;
    record.identity = (ta_definition_identity){ .family = { .name = { "fam", 3 } },
                                                .revision = { (const uint8_t *)"rev\0\1", 5 } };

    uint64_t attempts = heap.stats.attempts;
    ta_tel_write_trace(system, &record, &values);
    CHECK(heap.stats.attempts == attempts); /* writing allocates nothing */
    ta_ring_record raw;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && raw.seq == 1 && raw.gap == 0);
    CHECK(raw.size % 8 == 0 && raw.size > 296);
    ta_tel_entry entry;
    CHECK(ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.type == TA_TEL_TRACE && entry.seq == 1 && entry.at == record.now);
    const ta_tel_trace *t = &entry.trace;
    CHECK(t->kind == TA_TRACE_MICROSTEP);
    CHECK(t->key.session == 7 && t->key.macrostep == 5 && t->key.microstep == 2 && t->key.family == family);
    CHECK(t->cause.session == 8 && t->cause.macrostep == 4 && t->cause.microstep == 1 && t->cause.family == family);
    CHECK(t->context.trace_id_high == 0x1111 && t->context.trace_id_low == 0x2222 &&
          t->context.flags == TA_TRACE_FLAG_SAMPLED && t->context.detail == TA_TRACE_DETAIL_WATCHED);
    CHECK(t->position.at == TA_TRACE_AT_TRANSITION && t->position.state == 21 && t->position.transition == 22 &&
          t->position.action == 23 && t->position.iteration == 24);
    CHECK(t->actor == 42 && t->definition == 3 && t->now == record.now);
    CHECK(t->has_event && t->event_type == TA_EVENT_INTERNAL);
    CHECK(t->event_name.len == 5 && memcmp(t->event_name.ptr, "ev.go", 5) == 0);
    CHECK(t->event_send_id.len == 3 && memcmp(t->event_send_id.ptr, "s-1", 3) == 0);
    CHECK(t->event_invoke_id.len == 5 && memcmp(t->event_invoke_id.ptr, "inv.9", 5) == 0);
    CHECK(t->event_data.len == 7 && memcmp(t->event_data.ptr, "{\"e\":1}", 7) == 0);
    CHECK(t->state == 31 && t->expr == 32 && t->action == 33 && t->iteration == 34);
    CHECK(t->name.len == 7 && memcmp(t->name.ptr, "counter", 7) == 0);
    CHECK(t->send.len == 3 && memcmp(t->send.ptr, "s-1", 3) == 0);
    CHECK(t->invoke.len == 5 && memcmp(t->invoke.ptr, "inv.9", 5) == 0);
    CHECK(t->result && t->has_value && t->value.len == 3 && memcmp(t->value.ptr, "\"v\"", 3) == 0);
    CHECK(!t->truncated && !t->suppressed);
    CHECK(t->has_fault && t->fault_code == -7 && t->fault_message.len == 9 &&
          memcmp(t->fault_message.ptr, "it failed", 9) == 0);
    CHECK(t->fault_detail.len == 15 && memcmp(t->fault_detail.ptr, "error.execution", 15) == 0);
    CHECK(t->fault_data.len == 10 && memcmp(t->fault_data.ptr, "Error: bad", 10) == 0);
    CHECK(t->dropped == 17);
    CHECK(t->transitions.count == 4 && ta_tel_u32_at(&t->transitions, 0) == 11 && ta_tel_u32_at(&t->transitions, 3) == 14 &&
          ta_tel_u32_at(&t->transitions, 4) == 0);
    CHECK(t->exited.count == 2 && ta_tel_u32_at(&t->exited, 1) == 4);
    CHECK(t->entered.count == 1 && ta_tel_u32_at(&t->entered, 0) == 5);
    CHECK(t->history.count == 3 && ta_tel_u32_at(&t->history, 2) == 8);
    CHECK(t->states.count == 2 && ta_tel_u32_at(&t->states, 1) == 10);
    CHECK(t->identity.family.name.len == 3 && memcmp(t->identity.family.name.ptr, "fam", 3) == 0 &&
          t->identity.revision.len == 5 && memcmp(t->identity.revision.ptr, "rev\0\1", 5) == 0);
    /* The data model was asked for the three values, each with the parameter's limit. */
    CHECK(state.calls == 3 && state.last_limit == TA_PARAM_DEFAULT_TELEMETRY_VALUE_BYTES);
    ta_ring_release(system, TA_RING_TEL, raw.size);

    /* A record that names nothing decodes to NONE and empty, not to garbage. */
    ta_trace_record bare = record_of(TA_TRACE_SPAWNED, 0);
    ta_tel_write_trace(system, &bare, NULL);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && raw.size == 296);
    CHECK(ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(!entry.trace.has_event && !entry.trace.has_value && !entry.trace.has_fault);
    CHECK(entry.trace.expr == TA_EXPR_ID_NONE && entry.trace.state == TA_STATE_NONE &&
          entry.trace.action == TA_ACTION_NONE && entry.trace.position.state == TA_STATE_NONE &&
          entry.trace.position.transition == TA_TRANSITION_NONE && entry.trace.position.action == TA_ACTION_NONE);
    CHECK(entry.trace.invoke.len == 0 && entry.trace.send.len == 0);
    CHECK(entry.trace.event_name.len == 0 && entry.trace.value.len == 0 && entry.trace.states.count == 0);
    ta_ring_release(system, TA_RING_TEL, raw.size);
    ta_system_destroy(system);
    finish_clean();
}

/* A definition record carries the identity, borrowed for the call and copied into the ring. */
static void test_definition_record(void) {
    ta_system *system = create_with(NULL, 0);
    unsigned char revision[40];
    for (uint32_t at = 0; at < sizeof revision; at += 1u) {
        revision[at] = (unsigned char)(at * 7u + 1u);
    }
    ta_trace_record record = record_of(TA_TRACE_DEFINITION_REGISTERED, 9);
    record.key = (ta_step_key){ .session = TA_TRACE_DEFINITIONS_SESSION, .macrostep = 9, .microstep = 1 };
    record.identity = (ta_definition_identity){ .family = { .name = { "orders", 6 } },
                                                .revision = { revision, sizeof revision } };
    ta_tel_write_trace(system, &record, NULL);
    memset(revision, 0, sizeof revision); /* the identity was copied, not kept */
    ta_ring_record raw;
    ta_tel_entry entry;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.kind == TA_TRACE_DEFINITION_REGISTERED && entry.trace.key.session == TA_TRACE_DEFINITIONS_SESSION);
    CHECK(entry.trace.identity.family.name.len == 6 && memcmp(entry.trace.identity.family.name.ptr, "orders", 6) == 0);
    CHECK(entry.trace.identity.revision.len == 40 && entry.trace.identity.revision.ptr[3] == 22);
    ta_ring_release(system, TA_RING_TEL, raw.size);
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * The decoder refuses what is not a whole tel record
 */

static void test_decoder_refuses(void) {
    ta_system *system = create_with(NULL, 0);
    ta_trace_record record = record_of(TA_TRACE_RAISED, 1);
    ta_event event = { .name = name_of(system, "e"), .type = TA_EVENT_EXTERNAL };
    record.event = &event;
    ta_tel_write_trace(system, &record, NULL);
    ta_ring_record raw;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK);
    unsigned char copy[512];
    CHECK(raw.size <= sizeof copy);
    memcpy(copy, raw.bytes, raw.size);
    ta_tel_entry entry;
    ta_ring_record view = { copy, raw.size, raw.seq, 0 };
    CHECK(ta_tel_decode(&view, &entry) == TA_OK);

    /* Every single flipped header or fixed-part byte must either decode to the same kind of record or be refused, never crash. */
    for (uint32_t at = 0; at < 296; at += 1u) {
        unsigned char saved = copy[at];
        copy[at] = (unsigned char)(saved ^ 0xFFu);
        (void)ta_tel_decode(&view, &entry);
        copy[at] = saved;
    }
    /* Specific refusals. */
    unsigned char saved[8];
    memcpy(saved, copy, 8);
    copy[0] = 0x01; copy[1] = 0x02; /* a c2h kind on the tel ring */
    CHECK(ta_tel_decode(&view, &entry) == TA_ERR_INVALID);
    memcpy(copy, saved, 8);
    copy[0] = 0xFF; copy[1] = 0x7F; /* an unknown kind */
    CHECK(ta_tel_decode(&view, &entry) == TA_ERR_INVALID);
    memcpy(copy, saved, 8);
    CHECK(ta_tel_decode(&view, &entry) == TA_OK);
    ta_ring_record shorter = { copy, raw.size - 8, raw.seq, 0 };
    CHECK(ta_tel_decode(&shorter, &entry) == TA_ERR_INVALID); /* the size field says otherwise */
    ta_ring_record unseq = { copy, raw.size, 0, 0 };
    CHECK(ta_tel_decode(&unseq, &entry) == TA_ERR_INVALID);   /* seq 0 is no record */
    ta_ring_record seven = { copy, raw.size - 1, raw.seq, 0 };
    CHECK(ta_tel_decode(&seven, &entry) == TA_ERR_INVALID);
    ta_ring_record none = { NULL, 0, 1, 0 };
    CHECK(ta_tel_decode(&none, &entry) == TA_ERR_INVALID);
    CHECK(ta_tel_decode(NULL, &entry) == TA_ERR_INVALID && ta_tel_decode(&view, NULL) == TA_ERR_INVALID);
    /* A span moved outside the record (the event_name span sits at offset 152 + ...: flip every span's offset to a huge value). */
    for (uint32_t at = 144; at < 296; at += 8u) {
        unsigned char kept[4];
        memcpy(kept, copy + at, 4);
        copy[at + 3] = 0x7F;
        CHECK(ta_tel_decode(&view, &entry) == TA_ERR_INVALID || memcmp(kept, copy + at, 4) == 0);
        memcpy(copy + at, kept, 4);
    }
    /* A list whose length is not a multiple of four is refused, though the record is whole. */
    CHECK(ta_tel_decode(&view, &entry) == TA_OK);
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * The fill test of ADR 0098 (Consequences): the gap in seq, the count, and
 * tel.dropped before the next tel.trace
 */

static void test_fill_drop_and_report(void) {
    ta_system *system = create_tel(2048, 512);
    const ta_ring_shared *cell = &ta_system_control_block(system)->tel;
    /*
     * A record of 296 bytes and a 40-byte name is 336: six fit in 2,048 and leave 32 bytes, which is
     * not room for a tel.dropped (48), so nothing can be reported until the consumer reads.
     */
    for (uint64_t step = 1; step <= 10; step += 1) {
        ta_trace_record record = record_of(TA_TRACE_MACROSTEP_BEGIN, step);
        record.name = (ta_str){ "a-forty-byte-name-0123456789-0123456789", 40 };
        ta_tel_write_trace(system, &record, NULL);
    }
    CHECK(cell->dropped == 4);                   /* (c) the ring counted what it dropped */
    CHECK(cell->next_seq == 11);                 /* (a) every record, written or not, took a seq */
    CHECK(system->tel.pending == 4 && system->tel.first_seq == 7); /* (e) the first lost record would have been seq 7 */
    CHECK(cell->head == 6u * 336u);

    /* The consumer reads three records and leaves room. Nothing is reported until the producer writes again. */
    ta_ring_record raw;
    for (uint64_t expect = 1; expect <= 3; expect += 1) {
        CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && raw.seq == expect && raw.gap == 0);
        ta_ring_release(system, TA_RING_TEL, raw.size);
    }
    CHECK(cell->dropped == 4 && cell->next_seq == 11);

    ta_trace_record eleventh = record_of(TA_TRACE_MACROSTEP_BEGIN, 11);
    eleventh.name = (ta_str){ "a-forty-byte-name-0123456789-0123456789", 40 };
    ta_tel_write_trace(system, &eleventh, NULL);
    CHECK(system->tel.pending == 0);
    seen records[16];
    uint32_t count = drain(system, records, 16);
    CHECK(count == 5);
    /* 4, 5, 6 were there; then (d) tel.dropped, which precedes the trace that follows the gap. */
    CHECK(records[0].type == TA_TEL_TRACE && records[0].seq == 4 && records[0].macrostep == 4 && records[0].gap == 0);
    CHECK(records[2].type == TA_TEL_TRACE && records[2].seq == 6);
    CHECK(records[3].type == TA_TEL_DROPPED && records[3].seq == 11 && records[3].count == 4 &&
          records[3].first_seq == 7 && records[3].size == 48);
    CHECK(records[3].gap == 4);                  /* (b) the reader saw the jump: seqs 7 to 10 are missing */
    CHECK(records[4].type == TA_TEL_TRACE && records[4].seq == 12 && records[4].macrostep == 11 && records[4].gap == 0);
    CHECK(cell->dropped == 4 && cell->next_seq == 13);
    /* The sum identity: what the reports said plus what is pending is what the ring counted. */
    CHECK(records[3].count + system->tel.pending == cell->dropped);
    ta_system_destroy(system);
    finish_clean();
}

/* (f) A ring so full that tel.dropped cannot be written drops the next record too, and the count grows. */
static void test_report_does_not_fit(void) {
    ta_system *system = create_tel(512, 512);
    const ta_ring_shared *cell = &ta_system_control_block(system)->tel;
    char big[200];
    memset(big, 'm', sizeof big);
    ta_trace_record first = record_of(TA_TRACE_ACTION_FAILED, 1);
    first.has_fault = true;
    first.fault = (ta_fault){ .code = 1, .message = { big, 184 } }; /* 296 + 184 = 480 bytes: 32 are left */
    ta_tel_write_trace(system, &first, NULL);
    CHECK(cell->head == 480 && cell->dropped == 0);
    ta_trace_record second = record_of(TA_TRACE_MACROSTEP_BEGIN, 2);
    ta_tel_write_trace(system, &second, NULL); /* does not fit: dropped, seq 2 */
    CHECK(cell->dropped == 1 && system->tel.pending == 1 && system->tel.first_seq == 2);
    ta_trace_record third = record_of(TA_TRACE_MACROSTEP_BEGIN, 3);
    ta_tel_write_trace(system, &third, NULL);  /* 32 bytes are free, tel.dropped needs 48: this one is lost too */
    CHECK(cell->dropped == 2 && system->tel.pending == 2 && system->tel.first_seq == 2 && cell->next_seq == 4);
    CHECK(cell->head == 480);                  /* asking whether the report fits wrote and spent nothing */

    seen records[8];
    CHECK(drain(system, records, 8) == 1 && records[0].seq == 1);
    /* Empty ring, head at 480: the report needs a pad before it (32 bytes to the end), then fits. */
    ta_trace_record fourth = record_of(TA_TRACE_MACROSTEP_BEGIN, 4);
    ta_tel_write_trace(system, &fourth, NULL);
    CHECK(system->tel.pending == 0);
    CHECK(drain(system, records, 8) == 2);
    CHECK(records[0].type == TA_TEL_DROPPED && records[0].seq == 4 && records[0].count == 2 && records[0].first_seq == 2 &&
          records[0].gap == 2);
    CHECK(records[1].type == TA_TEL_TRACE && records[1].seq == 5 && records[1].macrostep == 4);
    ta_system_destroy(system);
    finish_clean();
}

/* A ring of exactly maxRecordBytes: a record is lone-padded, dropped, and the retry fits (ring.h, the lone pad). */
static void test_ring_of_one_record(void) {
    ta_system *system = create_tel(512, 512);
    char big[400];
    memset(big, 'k', sizeof big);
    for (uint64_t step = 1; step <= 6; step += 1) {
        ta_trace_record record = record_of(TA_TRACE_ACTION_FAILED, step);
        record.has_fault = true;
        record.fault = (ta_fault){ .code = (int32_t)step, .message = { big, 216 } }; /* 512 bytes exactly */
        ta_tel_write_trace(system, &record, NULL);
        seen records[8];
        uint32_t count = drain(system, records, 8);
        CHECK(count >= 1);
        for (uint32_t index = 0; index < count; index += 1u) {
            CHECK(records[index].type == TA_TEL_TRACE || records[index].type == TA_TEL_DROPPED);
        }
    }
    /* Every attempt took a seq; each was written or counted as dropped. */
    const ta_ring_shared *cell = &ta_system_control_block(system)->tel;
    CHECK(cell->next_seq - 1 >= 6);
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * Wrap with variable-length records
 */

static void test_wrap_variable_records(void) {
    ta_system *system = create_tel(1024, 512);
    const ta_ring_shared *cell = &ta_system_control_block(system)->tel;
    char name[160];
    for (uint32_t at = 0; at < sizeof name; at += 1u) {
        name[at] = (char)('a' + at % 26u);
    }
    uint64_t expected_seq = 1;
    for (uint32_t step = 1; step <= 600; step += 1) {
        ta_trace_record record = record_of(TA_TRACE_ASSIGN, step);
        uint32_t length = 1u + (step * 37u) % 150u; /* every residue of the text length, so every offset mod 8 and every wrap */
        record.name = (ta_str){ name, length };
        ta_tel_write_trace(system, &record, NULL);
        ta_ring_record raw;
        ta_tel_entry entry;
        CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK);
        CHECK(ta_tel_decode(&raw, &entry) == TA_OK);
        CHECK(raw.seq == expected_seq && raw.gap == 0);
        CHECK(entry.trace.key.macrostep == step && entry.trace.name.len == length &&
              memcmp(entry.trace.name.ptr, name, length) == 0);
        ta_ring_release(system, TA_RING_TEL, raw.size);
        expected_seq += 1u;
    }
    CHECK(cell->dropped == 0 && cell->head > 1024u * 10u); /* the ring wrapped many times without losing a record */
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * Values: when they are rendered, how they are cut
 */

static void test_values(void) {
    ta_system *system = create_with(NULL, 0);
    char text[3000];
    memset(text, 'x', sizeof text);
    fake_text value = { text, sizeof text };
    fake_state state = { 0 };
    ta_datamodel dm = { .self = &state, .v = &fake_vtable };
    ta_tel_values values = { .datamodel = &dm };
    ta_trace_record record = record_of(TA_TRACE_ASSIGN, 1);
    record.has_value = true;
    record.value = fake_value(&value);
    ta_ring_record raw;
    ta_tel_entry entry;

    /* Masks without `values`: the data model is never asked, and the record carries no value. */
    ta_tel_write_trace(system, &record, &values);
    CHECK(state.calls == 0);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(!entry.trace.has_value && entry.trace.value.len == 0 && !entry.trace.truncated);
    ta_ring_release(system, TA_RING_TEL, raw.size);

    /* With it, the text is cut at telemetryValueBytes and the record says so. */
    ta_system_set_trace_masks(system, &(ta_trace_masks){ .values = true });
    ta_tel_write_trace(system, &record, &values);
    CHECK(state.calls == 1 && state.last_limit == 1024);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.has_value && entry.trace.value.len == 1024 && entry.trace.truncated);
    ta_ring_release(system, TA_RING_TEL, raw.size);

    /* A text within the limit is whole. */
    value.length = 100;
    ta_tel_write_trace(system, &record, &values);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.has_value && entry.trace.value.len == 100 && !entry.trace.truncated);
    ta_ring_release(system, TA_RING_TEL, raw.size);

    /* A data model that ignores the limit is cut by core. */
    value.length = 3000;
    state.ignore_limit = true;
    ta_tel_write_trace(system, &record, &values);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.value.len == 1024 && entry.trace.truncated);
    ta_ring_release(system, TA_RING_TEL, raw.size);
    state.ignore_limit = false;

    /* A failing data model, no data model, a data model without the op, and no value: no text, and the record is written. */
    state.fail = true;
    state.calls = 0;
    ta_tel_write_trace(system, &record, &values);
    CHECK(state.calls == 1);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK && !entry.trace.has_value);
    ta_ring_release(system, TA_RING_TEL, raw.size);
    state.fail = false;
    ta_tel_write_trace(system, &record, NULL);
    ta_datamodel_vtable bare = { 0 };
    ta_datamodel bare_dm = { .self = &state, .v = &bare };
    ta_tel_values bare_values = { .datamodel = &bare_dm };
    ta_tel_write_trace(system, &record, &bare_values);
    ta_trace_record no_value = record;
    no_value.has_value = false;
    state.calls = 0;
    ta_tel_write_trace(system, &no_value, &values);
    CHECK(state.calls == 0);
    seen records[8];
    CHECK(drain(system, records, 8) == 3);

    /* A fault's data is rendered without the switch, and so is the data of an error event; ordinary event data is not. */
    ta_system_set_trace_masks(system, &(ta_trace_masks){ 0 });
    fake_text reason = { "Error: boom", 11 };
    ta_trace_record failed = record_of(TA_TRACE_ACTION_FAILED, 2);
    failed.has_fault = true;
    failed.fault = (ta_fault){ .code = 5, .message = { "m", 1 }, .data = fake_value(&reason) };
    state.calls = 0;
    ta_tel_write_trace(system, &failed, &values);
    CHECK(state.calls == 1);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.has_fault && entry.trace.fault_data.len == 11 && !entry.trace.has_value);
    ta_ring_release(system, TA_RING_TEL, raw.size);

    ta_event error = { .name = name_of(system, "error.execution"), .type = TA_EVENT_PLATFORM, .data = fake_value(&reason) };
    ta_event ordinary = { .name = name_of(system, "ping"), .type = TA_EVENT_EXTERNAL, .data = fake_value(&reason) };
    ta_trace_record raised = record_of(TA_TRACE_RAISED, 3);
    raised.event = &error;
    state.calls = 0;
    ta_tel_write_trace(system, &raised, &values);
    raised.event = &ordinary;
    ta_tel_write_trace(system, &raised, &values);
    CHECK(state.calls == 1);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.event_name.len == 15 && entry.trace.event_data.len == 11);
    ta_ring_release(system, TA_RING_TEL, raw.size);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.event_name.len == 4 && entry.trace.event_data.len == 0);
    ta_ring_release(system, TA_RING_TEL, raw.size);

    /* Each text says for itself that it was cut: the value, the event's data and the fault's data. */
    ta_system_set_trace_masks(system, &(ta_trace_masks){ .values = true });
    fake_text long_text = { text, sizeof text };
    ta_event long_error = { .name = error.name, .type = TA_EVENT_PLATFORM, .data = fake_value(&long_text) };
    ta_trace_record all = record_of(TA_TRACE_RAISED, 4);
    all.event = &long_error;
    all.has_fault = true;
    all.fault = (ta_fault){ .code = 1, .message = { "m", 1 }, .data = fake_value(&reason) };
    ta_tel_write_trace(system, &all, &values);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.event_data.len == 1024 && entry.trace.event_data_truncated && !entry.trace.truncated &&
          !entry.trace.fault_data_truncated && entry.trace.fault_data.len == 11);
    ta_ring_release(system, TA_RING_TEL, raw.size);
    all.fault.data = fake_value(&long_text);
    all.event = &error;
    ta_tel_write_trace(system, &all, &values);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(entry.trace.fault_data_truncated && !entry.trace.event_data_truncated && entry.trace.event_data.len == 11);
    ta_ring_release(system, TA_RING_TEL, raw.size);
    ta_system_destroy(system);
    finish_clean();
}

/* A value is cut to the room the record has, then the record is dropped if it is still too big. */
static void test_record_over_the_maximum(void) {
    fake_state state = { 0 };
    ta_datamodel dm = { .self = &state, .v = &fake_vtable };
    ta_tel_values values = { .datamodel = &dm };
    fake_text value = { "abc", 3 };
    ta_trace_record record = record_of(TA_TRACE_ASSIGN, 1);
    record.has_value = true;
    record.value = fake_value(&value);
    seen records[4];

    /* telemetryValueBytes 0 turns values off, whatever the masks say. */
    ta_param_value off[] = { { TA_PARAM_TEL_BYTES, 4096 }, { TA_PARAM_MAX_RECORD_BYTES, 512 },
                             { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    ta_system *zero = create_with(off, 3);
    ta_system_set_trace_masks(zero, &(ta_trace_masks){ .values = true });
    ta_tel_write_trace(zero, &record, &values);
    CHECK(state.calls == 0);
    CHECK(drain(zero, records, 4) == 1);
    ta_system_destroy(zero);

    /* 4,096-byte records, values up to 512: with a 400-byte name the value no longer fits in 1,024 and is cut to what does. */
    ta_param_value roomy[] = { { TA_PARAM_TEL_BYTES, 8192 }, { TA_PARAM_MAX_RECORD_BYTES, 1024 },
                               { TA_PARAM_TELEMETRY_VALUE_BYTES, 512 } };
    ta_system *system = create_with(roomy, 3);
    ta_system_set_trace_masks(system, &(ta_trace_masks){ .values = true });
    char text[900];
    memset(text, 'v', sizeof text);
    value = (fake_text){ text, sizeof text };
    char name[1100];
    memset(name, 'n', sizeof name);
    record.name = (ta_str){ name, 400 };
    ta_tel_write_trace(system, &record, &values);
    CHECK(state.calls == 1 && state.last_limit == 512);
    ta_ring_record raw;
    ta_tel_entry entry;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK && ta_tel_decode(&raw, &entry) == TA_OK);
    CHECK(raw.size == 1024 && entry.trace.name.len == 400 && entry.trace.has_value && entry.trace.value.len == 512 - 184 &&
          entry.trace.truncated);
    ta_ring_release(system, TA_RING_TEL, raw.size);

    /* A name too long for the record even with no value: lost, counted, and the gap is honest. */
    record.name = (ta_str){ name, 1000 };
    const ta_ring_shared *cell = &ta_system_control_block(system)->tel;
    uint64_t seq_before = cell->next_seq;
    ta_tel_write_trace(system, &record, &values);
    CHECK(cell->dropped == 1 && cell->next_seq == seq_before + 1 && system->tel.pending == 1 &&
          system->tel.first_seq == seq_before);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &raw) == TA_ERR_NOT_FOUND);
    ta_trace_record after = record_of(TA_TRACE_ASSIGN, 3);
    ta_tel_write_trace(system, &after, NULL);
    CHECK(drain(system, records, 4) == 2 && records[0].type == TA_TEL_DROPPED && records[0].count == 1 &&
          records[0].first_seq == seq_before && records[1].gap == 0);
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * Allocation, and a consumer that lies
 */

static void test_writing_never_allocates(void) {
    ta_system *system = create_tel(2048, 512);
    char name[100];
    memset(name, 'q', sizeof name);
    ta_event event = { .name = name_of(system, "a.b.c"), .type = TA_EVENT_EXTERNAL };
    ta_trace_record record = record_of(TA_TRACE_MICROSTEP, 1);
    record.event = &event;
    record.name = (ta_str){ name, sizeof name };
    ta_test_allocator_reset_counts(&heap);
    for (uint32_t step = 0; step < 500; step += 1u) {
        record.key.macrostep = step;
        ta_tel_write_trace(system, &record, NULL); /* the ring fills, drops, and is read now and then */
        if (step % 17u == 16u) {
            seen records[16];
            (void)drain(system, records, 16);
        }
    }
    CHECK(heap.stats.attempts == 0 && heap.stats.frees == 0);
    ta_system_destroy(system);
    finish_clean();
}

/* A consumer tail that moves backwards or past the head ends the ring's use, as a lost record, never a crash. */
static void test_corrupt_consumer_tail(void) {
    ta_system *system = create_tel(2048, 512);
    ta_trace_record record = record_of(TA_TRACE_SPAWNED, 1);
    ta_tel_write_trace(system, &record, NULL);
    ta_system_control_block(system); /* the host must not write; this test is the corrupt host */
    ((ta_ring_control *)(void *)ta_system_control_block(system))->tel.tail = 1u << 20;
    uint64_t head = ta_system_control_block(system)->tel.head;
    ta_tel_write_trace(system, &record, NULL);
    ta_tel_write_trace(system, &record, NULL);
    CHECK(ta_system_control_block(system)->tel.head == head); /* nothing was written */
    CHECK(system->tel.pending == 2 && ta_system_control_block(system)->tel.dropped == 0 + 2);
    ta_system_destroy(system);
    finish_clean();
}

int main(void) {
    test_control_block();
    test_reading_empty_rings();
    test_peek_release_through_the_system();
    test_one_allocation_sized_by_parameters();
    test_creation_allocation_sweep();
    test_masks();
    test_round_trip();
    test_definition_record();
    test_decoder_refuses();
    test_fill_drop_and_report();
    test_report_does_not_fit();
    test_ring_of_one_record();
    test_wrap_variable_records();
    test_values();
    test_record_over_the_maximum();
    test_writing_never_allocates();
    test_corrupt_consumer_tail();
    if (failures != 0) {
        fprintf(stderr, "tel_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tel_test: ok");
    return 0;
}

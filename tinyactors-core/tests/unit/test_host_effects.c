/* ta_host_effect_number / ta_host_effect_text (host.h): every field an effect
 * has, read without the struct's layout, and a send to an invoked service
 * carrying the id of the invoke that started it. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include "../support/host_config.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

#define CHART                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='caller' "         \
    "initial='s'><state id='s'>"                                                                   \
    "<invoke id='svc' type='http://example.com/service' src='svc:x'>"                             \
    "<param name='seed' expr='7'/></invoke>"                                                       \
    "<transition event='go'><log label='said' expr=\"'hello'\"/>"                                  \
    "<send target='#_svc' event='poke' id='p1'/></transition>"                                     \
    "<transition event='news'><log label='origin' expr=\"_event.origin + ' ' + _event.origintype\"/>"   \
    "</transition>"                                                                                \
    "<transition event='later'><send target='#_svc' event='tick' delay='1s'/></transition>"        \
    "<transition event='end' target='f'/></state><final id='f'/></scxml>"

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    uint32_t segment;
    uint64_t session;
} world;

static void *compile_caller(const char *text, size_t *length) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source source = {{"caller.scxml", 12}, {text, (uint32_t)strlen(text)}};
    ta_compile_result *result = NULL;
    void *copy = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &result) == TA_OK) {
        ta_string_view image = ta_compile_image(result);
        copy = aligned_alloc(8, (image.length + 7) & ~(size_t)7);
        memcpy(copy, image.pointer, image.length);
        *length = image.length;
    } else {
        print_diagnostics(result);
    }
    ta_compile_free(result);
    return copy;
}

static void world_open(world *w) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length;
    w->image_bytes = compile_caller(CHART, &length);
    CHECK(w->image_bytes != NULL);
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){w->image_bytes, length}, &w->segment), TA_OK);
    free(w->image_bytes); /* the host keeps a copy */
    w->image_bytes = NULL;
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = w->segment;
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &w->session), TA_OK);
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->image_bytes);
}

/* Run until idle; the published batch stays for the caller to read. */
static uint32_t run_batch(world *w, const ta_host_effect **effects) {
    ta_host_effects_ack(w->host);
    ta_run result = ta_host_run(w->host, 1000000);
    while (result.status == TA_RUN_YIELDED) /* a migration moves actors in steps */
        result = ta_host_run(w->host, 1000000);
    CHECK(result.status == TA_RUN_IDLE || result.status == TA_RUN_EFFECTS);
    return ta_host_effects(w->host, effects);
}

static void send_event(world *w, const char *name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.session = w->session;
    CHECK_EQUAL(ta_host_send(w->host, &event), TA_OK);
}

static int text_is(ta_string_view text, const char *expected) {
    return text.length == strlen(expected) && memcmp(text.pointer, expected, text.length) == 0;
}

/* The index of the first effect of `kind` in the batch, TA_NIL if none. */
static uint32_t find(ta_host *host, uint32_t count, uint32_t kind) {
    for (uint32_t i = 0; i < count; i++)
        if (ta_host_effect_number(host, i, TA_EFFECT_FIELD_KIND) == kind) return i;
    return TA_NIL;
}

static void test_invoked_service_is_keyed_by_invocation(void) {
    world w;
    world_open(&w);
    const ta_host_effect *effects;
    uint32_t count = run_batch(&w, &effects);
    uint32_t invoke = find(w.host, count, TA_EFFECT_INVOKE);
    CHECK(invoke != TA_NIL);
    uint64_t invocation = ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_INVOCATION);
    uint64_t segment = ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_SEGMENT);
    CHECK(invocation != 0);
    CHECK_EQUAL(ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_TOKEN), 0);
    CHECK_EQUAL(segment, w.segment);
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_TARGET), "svc:x"));
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_EVENT),
                  "http://example.com/service"));
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_SENDID), "svc"));
    send_event(&w, "go");
    count = run_batch(&w, &effects);
    uint32_t send = find(w.host, count, TA_EFFECT_SEND);
    CHECK(send != TA_NIL);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_INVOCATION), invocation);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_SEGMENT), segment);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_IO_PROCESSOR), (uint64_t)TA_NIL);
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_EVENT), "poke"));
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_SESSION), w.session);
    /* the log of the same step */
    uint32_t log = find(w.host, count, TA_EFFECT_LOG);
    CHECK(log != TA_NIL);
    CHECK(text_is(ta_host_effect_text(w.host, log, TA_EFFECT_FIELD_AUXILIARY), "said"));
    world_close(&w);
}

/* Every number and text field of the published batch matches the struct;
 * ORIGIN is the sender's address on a SEND; unknown fields, the wrong kind of
 * field and an index past the batch answer nothing. */
static void check_fields(world *w, const ta_host_effect *effects, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const ta_host_effect *e = &effects[i];
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_KIND), e->kind);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_SEGMENT), e->segment);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_ACTOR), e->actor);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_SESSION), e->session);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_ROOT), e->root);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_TOKEN), e->token);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_INVOCATION), e->invocation);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_IO_PROCESSOR), e->io_processor);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_FLAGS), e->flags);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_MIGRATION), e->migration);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_PAYLOAD), e->payload);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_DATAMODEL), e->datamodel);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_DELAY_HINT), e->delay_hint);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_CHART), e->chart);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_MIGRATED), e->migrated);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_FAILED), e->failed);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_PENDING), e->pending);
        ta_string_view t = ta_host_effect_text(w->host, i, TA_EFFECT_FIELD_TARGET);
        CHECK(t.length == e->target.length && memcmp(t.pointer, e->target.pointer, t.length) == 0);
        t = ta_host_effect_text(w->host, i, TA_EFFECT_FIELD_EVENT);
        CHECK(t.length == e->event.length && memcmp(t.pointer, e->event.pointer, t.length) == 0);
        t = ta_host_effect_text(w->host, i, TA_EFFECT_FIELD_SENDID);
        CHECK(t.length == e->sendid.length && memcmp(t.pointer, e->sendid.pointer, t.length) == 0);
        t = ta_host_effect_text(w->host, i, TA_EFFECT_FIELD_AUXILIARY);
        CHECK(t.length == e->auxiliary.length &&
              memcmp(t.pointer, e->auxiliary.pointer, t.length) == 0);
        char origin[64] = "";
        if (e->kind == TA_EFFECT_SEND && e->session)
            snprintf(origin, sizeof origin, "tinyactors://h/#_scxml_%llu", (unsigned long long)e->session);
        CHECK(text_is(ta_host_effect_text(w->host, i, TA_EFFECT_FIELD_ORIGIN), origin));
        char expected[TA_TRACEPARENT_LENGTH];
        ta_buffer out = {expected, sizeof expected, 0};
        int traced = ta_traceparent_format(&e->trace, &out) == TA_OK;
        t = ta_host_effect_text(w->host, i, TA_EFFECT_FIELD_TRACEPARENT);
        CHECK_EQUAL(t.length, traced ? TA_TRACEPARENT_LENGTH : 0u);
        CHECK(!traced || memcmp(t.pointer, expected, t.length) == 0);
        /* unknown fields and the wrong kind */
        CHECK_EQUAL(ta_host_effect_number(w->host, i, 0), 0u);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, 999), 0u);
        CHECK_EQUAL(ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_TARGET), 0u);
        CHECK_EQUAL(ta_host_effect_text(w->host, i, TA_EFFECT_FIELD_KIND).length, 0u);
    }
    CHECK_EQUAL(ta_host_effect_number(w->host, count, TA_EFFECT_FIELD_KIND), 0u);
    CHECK_EQUAL(ta_host_effect_text(w->host, count, TA_EFFECT_FIELD_EVENT).length, 0u);
}

static void test_fields_match_the_struct(void) {
    world w;
    world_open(&w);
    const ta_host_effect *effects;
    run_batch(&w, &effects);
    send_event(&w, "go");
    uint32_t count = run_batch(&w, &effects);
    CHECK(find(w.host, count, TA_EFFECT_SEND) != TA_NIL);
    check_fields(&w, effects, count);
    send_event(&w, "end");
    count = run_batch(&w, &effects);
    CHECK(find(w.host, count, TA_EFFECT_DONE) != TA_NIL);
    CHECK(find(w.host, count, TA_EFFECT_UNINVOKE) != TA_NIL);
    check_fields(&w, effects, count);
    /* after the ack the batch is gone */
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effect_number(w.host, 0, TA_EFFECT_FIELD_KIND), 0u);
    world_close(&w);
}

static void post_config(ta_host *host, uint64_t id) {
    unsigned char bytes[128];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){bytes, sizeof bytes, 0}, NULL);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("id"));
    ta_cbor_write_uint(&writer, id);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string("host.config"));
    ta_bytes message;
    CHECK_EQUAL(ta_cbor_writer_bytes(&writer, &message), TA_OK);
    CHECK_EQUAL(ta_host_post(host, message), TA_OK);
    ta_cbor_writer_free(&writer);
}

static void test_take_freezes_batch_across_growth(void) {
    world w;
    world_open(&w);
    CHECK_EQUAL(ta_host_run(w.host, 1000000).status, TA_RUN_IDLE);
    post_config(w.host, 0);
    const ta_host_effect *first, *again;
    uint32_t count = ta_host_effects(w.host, &first);
    CHECK(count > 0);
    CHECK_EQUAL(ta_host_effects(w.host, &again), count);
    CHECK(first == again);
    uint32_t reply = find(w.host, count, TA_HOST_EFFECT_REPLY);
    uint32_t invoke = find(w.host, count, TA_EFFECT_INVOKE);
    CHECK(reply != TA_NIL && invoke != TA_NIL);
    if (reply == TA_NIL || invoke == TA_NIL) {
        world_close(&w);
        return;
    }
    ta_bytes wire = ta_host_effect_bytes(w.host, reply, TA_EFFECT_FIELD_WIRE);
    void *saved = malloc(wire.length);
    CHECK(saved != NULL);
    if (!saved) {
        world_close(&w);
        return;
    }
    memcpy(saved, wire.pointer, wire.length);
    ta_string_view target = ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_TARGET);
    uintptr_t first_address = (uintptr_t)first;
    size_t bytes = 0;
    /* Enough replies to grow both the eight-record array and 4 KiB pool. */
    for (uint64_t id = 1; id <= 20; id++) {
        post_config(w.host, id);
        bytes += wire.length;
        CHECK_EQUAL(ta_host_effects(w.host, &again), count);
        CHECK(first == again);
        CHECK_EQUAL(ta_host_effect_number(w.host, count, TA_EFFECT_FIELD_KIND), 0);
        CHECK_EQUAL(ta_host_effect_number(w.host, reply, TA_EFFECT_FIELD_KIND), TA_HOST_EFFECT_REPLY);
        ta_bytes current = ta_host_effect_bytes(w.host, reply, TA_EFFECT_FIELD_WIRE);
        CHECK(current.pointer == wire.pointer && current.length == wire.length);
        CHECK(memcmp(current.pointer, saved, wire.length) == 0);
        ta_string_view current_target = ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_TARGET);
        CHECK(current_target.pointer == target.pointer && text_is(current_target, "svc:x"));
    }
    CHECK(bytes > 4096);
    free(saved);
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effect_number(w.host, 0, TA_EFFECT_FIELD_KIND), 0);
    CHECK_EQUAL(ta_host_effects(w.host, &again), 20);
    CHECK((uintptr_t)again != first_address); /* the array really grew */
    for (uint32_t i = 0; i < 20; i++) {
        CHECK_EQUAL(again[i].kind, TA_HOST_EFFECT_REPLY);
        ta_cbor_token tokens[512];
        ta_cbor_document document = {again[i].wire, tokens, 512, 0};
        CHECK_EQUAL(ta_cbor_decode(&document), TA_OK);
        uint32_t id = ta_cbor_member(&document, 0, ta_string("id"));
        CHECK(id != TA_NIL);
        if (id != TA_NIL) CHECK_EQUAL(tokens[id].value, i + 1);
    }
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effects(w.host, &again), 0);
    world_close(&w);
}

static void test_run_refuses_open_batch(void) {
    world w;
    world_open(&w);
    const ta_host_effect *first, *again;
    uint32_t count = run_batch(&w, &first);
    CHECK(count > 0);
    send_event(&w, "end");
    ta_run refused = ta_host_run(w.host, 1000000);
    CHECK_EQUAL(refused.status, TA_RUN_FAULT);
    CHECK_EQUAL((ta_status)refused.detail, TA_ERROR_STATE);
    CHECK_EQUAL(refused.used, 0);
    CHECK_EQUAL(refused.next_deadline, UINT64_MAX);
    CHECK_EQUAL(ta_host_effects(w.host, &again), count);
    CHECK(first == again);
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(w.host, w.session, &location), TA_OK);
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_run(w.host, 1000000).status, TA_RUN_IDLE);
    count = ta_host_effects(w.host, &again);
    CHECK(find(w.host, count, TA_EFFECT_DONE) != TA_NIL);
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effects(w.host, &again), 0);
    CHECK_EQUAL(ta_host_run(w.host, 1).status, TA_RUN_IDLE); /* empty take is not a batch */
    world_close(&w);
}

/* Every TRACEPARENT read from a batch stays valid until the ack: each effect's
 * text has its own memory, and reading another one, or the same one again,
 * leaves the views already taken as they were. */
static void test_traceparents_last_until_the_ack(void) {
    world w;
    world_open(&w);
    const ta_host_effect *effects;
    run_batch(&w, &effects);
    send_event(&w, "go");
    uint32_t count = run_batch(&w, &effects);
    ta_string_view views[16];
    char expected[16][TA_TRACEPARENT_LENGTH];
    uint32_t traced = 0;
    for (uint32_t i = 0; i < count && traced < 16; i++) {
        ta_buffer out = {expected[traced], TA_TRACEPARENT_LENGTH, 0};
        if (ta_traceparent_format(&effects[i].trace, &out) != TA_OK) continue;
        views[traced] = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_TRACEPARENT);
        traced++;
    }
    CHECK(traced >= 2); /* the log and the send of the same step */
    for (uint32_t i = 0; i < count; i++) /* every field read again */
        ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_TRACEPARENT);
    for (uint32_t k = 0; k < traced; k++) {
        CHECK_EQUAL(views[k].length, TA_TRACEPARENT_LENGTH);
        CHECK(memcmp(views[k].pointer, expected[k], TA_TRACEPARENT_LENGTH) == 0);
        for (uint32_t j = 0; j < k; j++)
            CHECK(views[j].pointer != views[k].pointer);
    }
    ta_host_effects_ack(w.host);
    world_close(&w);
}

/* The world's host snapshotted and packed, freed, and restored with a new
 * datamodel: the restore before its first run. */
static void restore_world(world *w) {
    /* the datamodel's state and the host, packed */
    ta_buffer probe = {NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(w->ecmascript, &probe), TA_ERROR_NO_MEMORY);
    void *state = malloc(probe.length);
    ta_buffer state_out = {state, probe.length, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(w->ecmascript, &state_out), TA_OK);
    ta_datamodel_state contents = {{"t", 1}, {state, state_out.length}};
    probe = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot_pack(w->host, &contents, &probe), TA_ERROR_NO_MEMORY);
    void *pack = aligned_alloc(8, (probe.length + 7) & ~(size_t)7);
    ta_buffer pack_out = {pack, probe.length, 0};
    CHECK_EQUAL(ta_host_snapshot_pack(w->host, &contents, &pack_out), TA_OK);
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    ta_host_snapshot_contents unpacked;
    memset(&unpacked, 0, sizeof unpacked);
    unpacked.size = sizeof unpacked;
    CHECK_EQUAL(ta_host_snapshot_unpack((ta_bytes){pack, pack_out.length}, &unpacked), TA_OK);
    ta_bytes image = ta_host_snapshot_image((ta_bytes){pack, pack_out.length}, 0);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    ta_host_snapshot_input input = {{0, 0}, {unpacked.host, unpacked.host_length}, &image, 1, 0};
    CHECK_EQUAL(test_host_restore(&config, &input, &w->host), TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(w->ecmascript, (ta_bytes){unpacked.state, unpacked.state_length}),
                TA_OK);
    free(state);
    free(pack);
}

/* A host restored from a snapshot announces each external invocation that
 * was running again, at its first run (after the datamodels' state is
 * back): the same segment, invocation id, type, src and invoke id, its
 * params, the invoker's session, flagged TA_EFFECT_FLAG_RESTORED; once. Its
 * id stays good for the service's answers. */
static void test_restore_announces_external_invocations(void) {
    world w;
    world_open(&w);
    const ta_host_effect *effects;
    uint32_t count = run_batch(&w, &effects);
    uint32_t invoke = find(w.host, count, TA_EFFECT_INVOKE);
    CHECK(invoke != TA_NIL);
    uint64_t invocation = ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_INVOCATION);
    CHECK_EQUAL(ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_FLAGS) & TA_EFFECT_FLAG_RESTORED, 0u);
    ta_host_effects_ack(w.host);
    restore_world(&w);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 0u); /* a take before the run: nothing yet */
    CHECK_EQUAL(ta_host_run(w.host, 1000000).status, TA_RUN_IDLE);
    count = ta_host_effects(w.host, &effects);
    invoke = find(w.host, count, TA_EFFECT_INVOKE);
    CHECK(invoke != TA_NIL);
    CHECK_EQUAL(ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_FLAGS) & TA_EFFECT_FLAG_RESTORED,
                (uint64_t)TA_EFFECT_FLAG_RESTORED);
    CHECK_EQUAL(ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_INVOCATION), invocation);
    CHECK_EQUAL(ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_SEGMENT), w.segment);
    CHECK_EQUAL(ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_SESSION), w.session);
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_TARGET), "svc:x"));
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_EVENT), "http://example.com/service"));
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_SENDID), "svc"));
    CHECK(ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_PAYLOAD) != 0);
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_run(w.host, 1000000).status, TA_RUN_IDLE);
    count = ta_host_effects(w.host, &effects);
    CHECK_EQUAL(find(w.host, count, TA_EFFECT_INVOKE), TA_NIL); /* once */
    ta_host_effects_ack(w.host);
    ta_event_in back;
    memset(&back, 0, sizeof back);
    back.size = sizeof back;
    back.session = w.session;
    back.flags = TA_EVENT_IN_INVOKE_DONE;
    back.invocation = invocation;
    CHECK_EQUAL(ta_host_send(w.host, &back), TA_OK); /* the id is the invocation's still */
    CHECK_EQUAL(ta_host_run(w.host, 1000000).status, TA_RUN_IDLE);
    world_close(&w);
}

static ta_status from_service(world *w, uint64_t invocation, uint32_t flags) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = w->session;
    event.flags = flags;
    event.invocation = invocation;
    event.name = (ta_string_view){"news", 4};
    return ta_host_send(w->host, &event);
}

/* An invocation whose service said done (its done.invoke not taken yet) or
 * failed is over for the service's side: a restore does not announce it
 * again, and the service's later events are refused. */
static void test_restore_skips_ended_invocations(void) {
    for (int failed = 0; failed < 2; failed++) {
        world w;
        world_open(&w);
        const ta_host_effect *effects;
        uint32_t count = run_batch(&w, &effects);
        uint64_t invocation = ta_host_effect_number(w.host, find(w.host, count, TA_EFFECT_INVOKE),
                                               TA_EFFECT_FIELD_INVOCATION);
        ta_host_effects_ack(w.host);
        uint32_t ended = failed ? TA_EVENT_IN_ERROR_COMMUNICATION | TA_EVENT_IN_FROM_INVOKE : TA_EVENT_IN_INVOKE_DONE;
        CHECK_EQUAL(from_service(&w, invocation, ended), TA_OK); /* not run: the actor has not taken it */
        CHECK_EQUAL(from_service(&w, invocation, TA_EVENT_IN_FROM_INVOKE), TA_ERROR_STALE);
        CHECK_EQUAL(from_service(&w, invocation, TA_EVENT_IN_INVOKE_DONE), TA_ERROR_STALE);
        restore_world(&w);
        CHECK_EQUAL(ta_host_run(w.host, 1000000).status, TA_RUN_IDLE);
        count = ta_host_effects(w.host, &effects);
        CHECK_EQUAL(find(w.host, count, TA_EFFECT_INVOKE), TA_NIL);
        ta_host_effects_ack(w.host);
        world_close(&w);
    }
}

/* An event from a service names it as its origin, so a chart can <send> a
 * reply: #_<invokeid>, through SCXML's I/O processor (SCXML 5.10.1). */
static void test_an_event_from_a_service_names_its_origin(void) {
    world w;
    world_open(&w);
    const ta_host_effect *effects;
    uint32_t count = run_batch(&w, &effects);
    uint64_t invocation = ta_host_effect_number(w.host, find(w.host, count, TA_EFFECT_INVOKE),
                                           TA_EFFECT_FIELD_INVOCATION);
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(from_service(&w, invocation, TA_EVENT_IN_FROM_INVOKE), TA_OK);
    count = run_batch(&w, &effects);
    uint32_t log = find(w.host, count, TA_EFFECT_LOG);
    CHECK(log != TA_NIL);
    ta_string_view text = ta_host_effect_text(w.host, log, TA_EFFECT_FIELD_EVENT);
    if (!text_is(text, "#_svc http://www.w3.org/TR/scxml/#SCXMLEventProcessor"))
        fprintf(stderr, "  origin: %.*s\n", (int)text.length, text.pointer);
    CHECK(text_is(text, "#_svc http://www.w3.org/TR/scxml/#SCXMLEventProcessor"));
    ta_host_effects_ack(w.host);
    world_close(&w);
}

/* ---- a migration carries the services along (docs/adr/0018) ---- */

/* Versions 2 of the caller: its state keeps the service (same id and type),
 * drops it, or replaces it with another <invoke>. */
#define CALLER_HEAD                                                                                \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='caller' "         \
    "initial='s'><state id='s'>"
#define CALLER_TAIL                                                                                \
    "<transition event='news'><log label='news' expr=\"'v2'\"/></transition>"                    \
    "<transition event='end' target='f'/></state><state id='added'/><final id='f'/></scxml>"
static const char kept_chart[] = CALLER_HEAD
    "<invoke id='svc' type='http://example.com/service' src='svc:x'>"
    "<finalize><log label='finalize' expr=\"'v2'\"/></finalize></invoke>" CALLER_TAIL;
static const char dropped_chart[] = CALLER_HEAD CALLER_TAIL;
static const char replaced_chart[] = CALLER_HEAD
    "<invoke id='other' type='http://example.com/service' src='svc:y'/>" CALLER_TAIL;

/* A migration of the world's caller: version 2's text, whether a delayed
 * send to the service waits, and the batch of the migration's run. */
typedef struct migration_case {
    const char *chart;
    int later;
    const ta_host_effect *effects;
    uint32_t count;
} migration_case;

/* The world's caller with its service started, then version 2 added and the
 * caller migrated to it; the batch of the migration's run is published.
 * Returns the service's id. */
static uint64_t migrate_caller(world *w, migration_case *c) {
    const ta_host_effect **effects = &c->effects;
    uint32_t *count = &c->count;
    *count = run_batch(w, effects);
    uint64_t invocation =
        ta_host_effect_number(w->host, find(w->host, *count, TA_EFFECT_INVOKE), TA_EFFECT_FIELD_INVOCATION);
    CHECK(invocation != 0);
    if (c->later) {
        send_event(w, "later");
        run_batch(w, effects);
    }
    size_t length;
    void *image = compile_caller(c->chart, &length);
    CHECK(image != NULL);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){image, length}, &segment), TA_OK);
    free(image);
    uint32_t migration;
    CHECK_EQUAL(ta_host_migrate(w->host, &(ta_host_migration){(ta_string_view){"caller", 6}, 0, 0, 0, 0},
                                &migration),
                TA_OK);
    *count = run_batch(w, effects);
    CHECK(find(w->host, *count, TA_HOST_EFFECT_MIGRATED) != TA_NIL);
    CHECK_EQUAL(ta_host_effect_number(w->host, find(w->host, *count, TA_HOST_EFFECT_MIGRATED),
                                      TA_EFFECT_FIELD_SEGMENT),
                segment);
    return invocation;
}

/* A kept service goes on under its id: nothing tells it of the move, its
 * events reach the new version (its transitions and <finalize>), and a
 * delayed send to it follows it. */
static void test_migration_carries_a_kept_service(void) {
    world w;
    world_open(&w);
    migration_case c = {kept_chart, 1, NULL, 0};
    uint64_t invocation = migrate_caller(&w, &c);
    const ta_host_effect *effects = c.effects;
    uint32_t count = c.count;
    CHECK_EQUAL(find(w.host, count, TA_EFFECT_UNINVOKE), TA_NIL);
    CHECK_EQUAL(find(w.host, count, TA_EFFECT_INVOKE), TA_NIL);
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(from_service(&w, invocation, TA_EVENT_IN_FROM_INVOKE), TA_OK);
    count = run_batch(&w, &effects);
    int finalized = 0, handled = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_KIND) != TA_EFFECT_LOG) continue;
        ta_string_view label = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_AUXILIARY);
        finalized += text_is(label, "finalize");
        handled += text_is(label, "news");
    }
    CHECK(finalized == 1 && handled == 1);
    ta_host_effects_ack(w.host);
    ta_host_advance_time(w.host, (ta_system_time){2000000000u, 0});
    count = run_batch(&w, &effects);
    uint32_t send = find(w.host, count, TA_EFFECT_SEND);
    CHECK(send != TA_NIL);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_IO_PROCESSOR), (uint64_t)TA_NIL);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_INVOCATION), invocation);
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_EVENT), "tick"));
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(from_service(&w, invocation, TA_EVENT_IN_INVOKE_DONE), TA_OK);
    run_batch(&w, &effects);
    ta_host_effects_ack(w.host);
    world_close(&w);
}

/* A service the new version drops is stopped (UNINVOKE with its id), and
 * its later events are refused; one it replaces with another <invoke> is
 * stopped, and the new one starts under a new id. */
static void test_migration_stops_a_dropped_service(void) {
    for (int replaced = 0; replaced < 2; replaced++) {
        world w;
        world_open(&w);
        migration_case c = {replaced ? replaced_chart : dropped_chart, 0, NULL, 0};
        uint64_t invocation = migrate_caller(&w, &c);
        const ta_host_effect *effects = c.effects;
        uint32_t count = c.count;
        uint32_t uninvoke = find(w.host, count, TA_EFFECT_UNINVOKE);
        CHECK(uninvoke != TA_NIL);
        CHECK_EQUAL(ta_host_effect_number(w.host, uninvoke, TA_EFFECT_FIELD_INVOCATION), invocation);
        CHECK_EQUAL(ta_host_effect_number(w.host, uninvoke, TA_EFFECT_FIELD_SESSION), w.session);
        uint32_t invoke = find(w.host, count, TA_EFFECT_INVOKE);
        if (!replaced) {
            CHECK_EQUAL(invoke, TA_NIL);
        } else {
            if (invoke == TA_NIL) { /* the new one starts in the migration's run or the next */
                ta_host_effects_ack(w.host);
                count = run_batch(&w, &effects);
                invoke = find(w.host, count, TA_EFFECT_INVOKE);
            }
            CHECK(invoke != TA_NIL);
            uint64_t started = ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_INVOCATION);
            CHECK(started != 0 && started != invocation);
            CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_SENDID), "other"));
        }
        ta_host_effects_ack(w.host);
        CHECK_EQUAL(from_service(&w, invocation, TA_EVENT_IN_FROM_INVOKE), TA_ERROR_STALE);
        world_close(&w);
    }
}

int main(void) {
    RUN(test_take_freezes_batch_across_growth);
    RUN(test_run_refuses_open_batch);
    RUN(test_invoked_service_is_keyed_by_invocation);
    RUN(test_fields_match_the_struct);
    RUN(test_traceparents_last_until_the_ack);
    RUN(test_restore_announces_external_invocations);
    RUN(test_restore_skips_ended_invocations);
    RUN(test_an_event_from_a_service_names_its_origin);
    RUN(test_migration_carries_a_kept_service);
    RUN(test_migration_stops_a_dropped_service);
    TEST_MAIN_END();
}

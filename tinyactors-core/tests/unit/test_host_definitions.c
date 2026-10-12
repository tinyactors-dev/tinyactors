/* Definition logs (docs/architecture.md, 16.3): a log record
 * when a chart version is linked, retired and freed, taken with
 * ta_host_logs; none without an encoder. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_config.h"

#define CHART(final)                                                                               \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='pinged' "         \
    "initial='s'><state id='s'><transition event='stop' target='" final "'/></state>"             \
    "<final id='" final "'/></scxml>"

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *bytes[2];
    ta_image image[2];
    uint32_t freed; /* TA_HOST_EFFECT_SEGMENT_FREED seen */
} world;

/* telemetry: 0 none, 1 an encoder, 2 an encoder with "logs": false */
static void world_open(world *w, int telemetry) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    if (telemetry) config.telemetry.otlp.size = sizeof config.telemetry.otlp;
    if (telemetry == 2) config.telemetry.otlp.flags = TA_OTLP_NO_LOGS;
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    const char *charts[2] = {CHART("one"), CHART("two")};
    for (int i = 0; i < 2; i++) {
        size_t length = 0;
        w->bytes[i] = compile_chart("pinged.scxml", charts[i], NULL, &length, 0);
        CHECK(w->bytes[i] != NULL);
        CHECK_EQUAL(ta_image_open((ta_bytes){w->bytes[i], length}, &w->image[i]), TA_OK);
    }
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->bytes[0]);
    free(w->bytes[1]);
}

static void run_once(world *w) {
    ta_host_run(w->host, 1000000);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w->host, &effects);
    for (uint32_t i = 0; i < n; i++)
        w->freed += effects[i].kind == TA_HOST_EFFECT_SEGMENT_FREED;
    ta_host_effects_ack(w->host);
}

static void advance(world *w, ta_time monotonic) {
    ta_host_advance_time(w->host, (ta_host_time){monotonic, 1700000000000000000ull + monotonic});
}

/* Version one with an actor, version two linked (one is retired, its actor
 * stays), the actor ends, version one is freed. */
static void scenario(world *w) {
    advance(w, 10);
    uint32_t first = 0, second = 0;
    CHECK_EQUAL(ta_host_add_image(w->host, &w->image[0], &first), TA_OK);
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = first;
    uint64_t session = 0;
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &session), TA_OK);
    run_once(w);
    advance(w, 20);
    CHECK_EQUAL(ta_host_add_image(w->host, &w->image[1], &second), TA_OK);
    CHECK_EQUAL(second, 1u);
    ta_event_in stop;
    memset(&stop, 0, sizeof stop);
    stop.session = session;
    stop.name = (ta_string_view){"stop", 4};
    CHECK_EQUAL(ta_host_send(w->host, &stop), TA_OK);
    advance(w, 30);
    for (int i = 0; i < 4 && !w->freed; i++)
        run_once(w);
    CHECK_EQUAL(w->freed, 1u);
}

/* ---- reading an ExportLogsServiceRequest: just enough protobuf ------------ */

typedef struct field {
    uint32_t number, wire;
    uint64_t value;
    const unsigned char *bytes;
    size_t length;
} field;

static uint64_t varint(const unsigned char **at, const unsigned char *end) {
    uint64_t value = 0;
    for (int shift = 0; *at < end && shift < 64; shift += 7) {
        unsigned char byte = *(*at)++;
        value |= (uint64_t)(byte & 0x7Fu) << shift;
        if (!(byte & 0x80u)) break;
    }
    return value;
}

/* The next field of a message, 0 at its end. */
static int next_field(const unsigned char **at, const unsigned char *end, field *out) {
    if (*at >= end) return 0;
    uint64_t tag = varint(at, end);
    out->number = (uint32_t)(tag >> 3);
    out->wire = (uint32_t)(tag & 7u);
    out->bytes = NULL;
    out->length = 0;
    if (out->wire == 0) {
        out->value = varint(at, end);
    } else if (out->wire == 1) {
        out->value = 0;
        for (int i = 0; i < 8; i++)
            out->value |= (uint64_t)(*at)[i] << (8 * i);
        *at += 8;
    } else if (out->wire == 2) {
        out->length = (size_t)varint(at, end);
        out->bytes = *at;
        *at += out->length;
    } else if (out->wire == 5) {
        *at += 4;
    }
    return 1;
}

typedef struct record {
    char event[48], body[96], chart[16], hash[17];
    uint64_t time, severity;
    int64_t segment, actors; /* -1: absent */
} record;

static void copy_text(char *to, size_t capacity, const unsigned char *bytes, size_t length) {
    size_t n = length < capacity - 1 ? length : capacity - 1;
    memcpy(to, bytes, n);
    to[n] = 0;
}

/* AnyValue: its string, or its int. */
static void any_value(const unsigned char *at, const unsigned char *end, char *text, size_t
                      capacity, int64_t *integer) {
    field f;
    while (next_field(&at, end, &f)) {
        if (f.number == 1 && f.wire == 2 && text) copy_text(text, capacity, f.bytes, f.length);
        if (f.number == 3 && f.wire == 0 && integer) *integer = (int64_t)f.value;
    }
}

static void attribute(record *r, const unsigned char *at, const unsigned char *end) {
    field f;
    char key[48] = {0};
    const unsigned char *value = NULL, *value_end = NULL;
    while (next_field(&at, end, &f)) {
        if (f.number == 1) copy_text(key, sizeof key, f.bytes, f.length);
        if (f.number == 2) value = f.bytes, value_end = f.bytes + f.length;
    }
    if (!value) return;
    if (!strcmp(key, "tinyactors.chart.name")) any_value(value, value_end, r->chart, sizeof r->chart, NULL);
    if (!strcmp(key, "tinyactors.image.hash")) any_value(value, value_end, r->hash, sizeof r->hash, NULL);
    if (!strcmp(key, "tinyactors.segment")) any_value(value, value_end, NULL, 0, &r->segment);
    if (!strcmp(key, "tinyactors.chart.actors")) any_value(value, value_end, NULL, 0, &r->actors);
}

static void log_record(record *r, const unsigned char *at, const unsigned char *end) {
    memset(r, 0, sizeof *r);
    r->segment = r->actors = -1;
    field f;
    while (next_field(&at, end, &f)) {
        if (f.number == 1) r->time = f.value;
        if (f.number == 2) r->severity = f.value;
        if (f.number == 5) any_value(f.bytes, f.bytes + f.length, r->body, sizeof r->body, NULL);
        if (f.number == 6) attribute(r, f.bytes, f.bytes + f.length);
        if (f.number == 12) copy_text(r->event, sizeof r->event, f.bytes, f.length);
    }
}

/* Every LogRecord of the request, in order; how many. */
static uint32_t log_records(const unsigned char *bytes, size_t length, record *out, uint32_t max) {
    uint32_t n = 0;
    field resource, scope, item;
    const unsigned char *at = bytes, *end = bytes + length;
    while (next_field(&at, end, &resource)) {
        if (resource.number != 1) continue; /* ResourceLogs */
        const unsigned char *r = resource.bytes, *r_end = r + resource.length;
        while (next_field(&r, r_end, &scope)) {
            if (scope.number != 2) continue; /* ScopeLogs */
            const unsigned char *s = scope.bytes, *s_end = s + scope.length;
            while (next_field(&s, s_end, &item))
                if (item.number == 2 && n < max) log_record(&out[n++], item.bytes, item.bytes + item.length);
        }
    }
    return n;
}

static uint32_t definition_records(const unsigned char *bytes, size_t length, record *out,
                                   uint32_t max) {
    record all[32];
    uint32_t n = log_records(bytes, length, all, 32), kept = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strncmp(all[i].event, "tinyactors.definition.", 22) && kept < max) out[kept++] = all[i];
    return kept;
}

static void hash_of(const ta_image *image, char hex[17]) {
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(image, &info);
    for (int i = 0; i < 16; i++)
        hex[i] = "0123456789abcdef"[(info.content_hash >> (60 - 4 * i)) & 15u];
    hex[16] = 0;
}

static void test_linked_retired_freed(void) {
    world w;
    world_open(&w, 1);
    scenario(&w);
    static unsigned char bytes[1 << 16];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_logs(w.host, &out), TA_OK);
    record r[8];
    uint32_t n = definition_records(bytes, out.length, r, 8);
    CHECK_EQUAL(n, 4u);
    char one[17], two[17];
    hash_of(&w.image[0], one);
    hash_of(&w.image[1], two);
    CHECK(strcmp(one, two) != 0);

    CHECK(strcmp(r[0].event, "tinyactors.definition.linked") == 0);
    CHECK(strcmp(r[0].body, "linked pinged (segment 0)") == 0);
    CHECK(strcmp(r[0].chart, "pinged") == 0);
    CHECK(strcmp(r[0].hash, one) == 0);
    CHECK_EQUAL(r[0].segment, 0);
    CHECK_EQUAL(r[0].actors, -1);
    CHECK_EQUAL(r[0].severity, 5u); /* DEBUG */
    CHECK_EQUAL(r[0].time, 1700000000000000010ull);

    CHECK(strcmp(r[1].event, "tinyactors.definition.linked") == 0);
    CHECK(strcmp(r[1].hash, two) == 0);
    CHECK_EQUAL(r[1].segment, 1);
    CHECK_EQUAL(r[1].time, 1700000000000000020ull);

    CHECK(strcmp(r[2].event, "tinyactors.definition.retired") == 0);
    CHECK(strcmp(r[2].body, "retired pinged (segment 0, 1 actor)") == 0);
    CHECK(strcmp(r[2].hash, one) == 0);
    CHECK_EQUAL(r[2].segment, 0);
    CHECK_EQUAL(r[2].actors, 1);
    CHECK_EQUAL(r[2].severity, 9u); /* INFO */

    CHECK(strcmp(r[3].event, "tinyactors.definition.freed") == 0);
    CHECK(strcmp(r[3].body, "freed pinged (segment 0)") == 0);
    CHECK(strcmp(r[3].hash, one) == 0);
    CHECK_EQUAL(r[3].segment, 0);
    CHECK_EQUAL(r[3].severity, 5u);
    CHECK_EQUAL(r[3].time, 1700000000000000030ull);
    world_close(&w);
}

/* Without an encoder: the same changes, nothing to take. */
static void test_without_telemetry(void) {
    world w;
    world_open(&w, 0);
    scenario(&w);
    unsigned char bytes[64];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_logs(w.host, &out), TA_ERROR_STATE);
    CHECK_EQUAL(out.length, 0u);
    world_close(&w);
}

/* "logs": false: no definition logs either. */
static void test_logs_disabled(void) {
    world w;
    world_open(&w, 2);
    scenario(&w);
    static unsigned char bytes[1 << 16];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_logs(w.host, &out), TA_OK);
    record r[8];
    CHECK_EQUAL(definition_records(bytes, out.length, r, 8), 0u);
    world_close(&w);
}

int main(void) {
    RUN(test_linked_retired_freed);
    RUN(test_without_telemetry);
    RUN(test_logs_disabled);
    TEST_MAIN_END();
}

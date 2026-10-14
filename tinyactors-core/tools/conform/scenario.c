/* scenario.run (driver.md, "Scenario steps" and "The scenario
 * environment"): a host of its own, the steps' calls in order, and after
 * each the loop until the host is idle at the current time, with the
 * effects a scenario needs answered: WAKE from the store, REQUEST from the
 * segment's image, JOB_DONE for a migration. The charts check what
 * happened; the first Outcome log decides. */
#include "conform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct scenario_blob {
    uint64_t session;
    ta_buffer bytes;
} scenario_blob;

/* A ta_host_snapshot_pack blob, and its images (views into it). */
typedef struct scenario_snapshot {
    char name[64];
    ta_buffer pack;
    ta_bytes images[16];
} scenario_snapshot;

typedef struct scenario {
    conform_call *call;
    conform_sources documents;
    conform_datamodel datamodel;
    ta_host *host;
    unsigned char config[256];
    size_t config_length;
    conform_alias aliases[64];
    uint32_t alias_count;
    scenario_blob store[64]; /* the hibernated actors' blobs */
    uint32_t store_count;
    scenario_snapshot snapshots[8];
    uint32_t snapshot_count;
    ta_time now;
    int job_done;
    const char *outcome;
} scenario;

/* The host's config: {"systemID": "conformance", "system": {"ticksPerSecond": 1000}}. */
static ta_status config_write(void *context, ta_buffer *out) {
    (void)context;
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, *out, NULL);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("systemID"));
    ta_cbor_write_text(&writer, ta_string("conformance"));
    ta_cbor_write_text(&writer, ta_string("system"));
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("ticksPerSecond"));
    ta_cbor_write_uint(&writer, 1000);
    return conform_writer_end(&writer, out);
}

/* A step the host refused: the scenario fails, and says why. */
static int refused(scenario *run, const char *why) {
    fprintf(stderr, "ta-conform: %s\n", why);
    if (!run->outcome) run->outcome = "fail";
    return 0;
}

/* A step that is not one (driver.md, "Scenario steps"): -32602. */
static int malformed(scenario *run, uint32_t index, const char *message) {
    char path[96];
    snprintf(path, sizeof path, "params.steps[%u]", index);
    conform_fail(run->call, &(conform_failure){CONFORM_INVALID_PARAMS, "type", path}, message);
    return 0;
}

static const conform_alias *alias_named(const scenario *run, ta_string_view name) {
    for (uint32_t i = 0; i < run->alias_count; i++)
        if (strlen(run->aliases[i].name) == name.length && !memcmp(run->aliases[i].name, name.pointer, name.length))
            return &run->aliases[i];
    return NULL;
}

/* The text member `name` of the step map at `step`. */
static int step_text(const scenario *run, uint32_t step, const char *name, ta_string_view *out) {
    return conform_token_text(run->call, ta_cbor_member(&run->call->document, step, ta_string(name)), out);
}

/* The value at `at` (TA_NIL: none), "@alias" replaced, as a payload. */
static ta_status value_read(scenario *run, uint32_t at, ta_payload *out) {
    *out = 0;
    if (at == TA_NIL) return TA_OK;
    ta_buffer bytes = {NULL, 0, 0};
    conform_aliases aliases = {run->aliases, run->alias_count};
    ta_status status = conform_item_bytes(run->call, &(conform_item){at, &aliases}, &bytes);
    if (status == TA_OK) status = conform_datamodel_read(&run->datamodel, (ta_bytes){bytes.pointer, bytes.length}, out);
    conform_buffer_free(&bytes);
    return status;
}

/* ---- the loop -------------------------------------------------------------------------- */

/* A REQUEST (an <invoke srcexpr> the image did not resolve): the chart of
 * the segment's image its auxiliary names, or TA_ERROR_INVALID. */
typedef struct scenario_request {
    uint32_t segment, token;
    char source[256];
} scenario_request;

static void request_answer(scenario *run, const scenario_request *request) {
    ta_string_view name = ta_string(request->source);
    static const char *const prefixes[] = {"file://", "file:", "./"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t length = strlen(prefixes[i]);
        if (name.length > length && !memcmp(name.pointer, prefixes[i], length)) {
            name.pointer += length;
            name.length -= (uint32_t)length;
        }
    }
    ta_host_segment_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    uint32_t chart = ta_host_segment_info_get(run->host, request->segment, &info) == TA_OK && info.image
                         ? ta_image_chart_by_name(info.image, name)
                         : TA_NIL;
    ta_resume resume = {request->segment, request->token, chart == TA_NIL ? TA_ERROR_INVALID : TA_OK, 0,
                        chart == TA_NIL ? 0 : (ta_payload)chart + 1};
    ta_host_resume(run->host, &resume);
}

static void wake(scenario *run, uint64_t session) {
    for (uint32_t i = 0; i < run->store_count; i++) {
        scenario_blob *blob = &run->store[i];
        if (blob->session != session) continue;
        if (ta_host_wake(run->host, session, (ta_bytes){blob->bytes.pointer, blob->bytes.length}) == TA_OK) {
            conform_buffer_free(&blob->bytes);
            *blob = run->store[--run->store_count];
        }
        return;
    }
}

/* Runs until the host is idle at the current time with nothing left to
 * answer, at most 1,000 runs; stops at the first Outcome. */
static void settle(scenario *run) {
    for (int round = 0; round < 1000 && !run->outcome; round++) {
        ta_run result = ta_host_run(run->host, 100000);
        const ta_host_effect *effects;
        uint32_t count = ta_host_effects(run->host, &effects);
        static uint64_t wakes[64];
        static scenario_request requests[16];
        uint32_t wake_count = 0, request_count = 0;
        for (uint32_t i = 0; i < count; i++) {
            const ta_host_effect *effect = &effects[i];
            if (!run->outcome) run->outcome = conform_outcome(effect);
            if (effect->kind == TA_EFFECT_WAKE && wake_count < 64) {
                wakes[wake_count++] = effect->session;
            } else if (effect->kind == TA_EFFECT_REQUEST && request_count < 16) {
                scenario_request *request = &requests[request_count++];
                request->segment = effect->segment;
                request->token = effect->token;
                snprintf(request->source, sizeof request->source, "%.*s", (int)effect->auxiliary.length,
                         effect->auxiliary.pointer ? effect->auxiliary.pointer : "");
            } else if (effect->kind == TA_HOST_EFFECT_JOB_DONE) {
                run->job_done = 1;
            }
        }
        ta_host_effects_ack(run->host);
        for (uint32_t i = 0; i < wake_count; i++)
            wake(run, wakes[i]);
        for (uint32_t i = 0; i < request_count; i++)
            request_answer(run, &requests[i]);
        if (result.status == TA_RUN_IDLE && !wake_count && !request_count) return;
    }
}

/* ---- steps ------------------------------------------------------------------------------- */

typedef struct scenario_step {
    uint32_t index; /* in params.steps */
    uint32_t at;    /* the step's map */
    uint32_t value; /* the value of its first member: the step's operand */
} scenario_step;

static int step_image(scenario *run, const scenario_step *step) {
    const ta_cbor_document *document = &run->call->document;
    const ta_cbor_token *names = &document->tokens[step->value];
    static ta_source charts[32];
    if (names->kind != TA_CBOR_ARRAY || !names->count || names->count > 32)
        return malformed(run, step->index, "image names its documents in an array");
    uint32_t item = step->value + 1;
    for (uint32_t i = 0; i < names->count; i++, item = ta_cbor_next(document, item)) {
        ta_string_view name;
        uint32_t k = 0;
        if (!conform_token_text(run->call, item, &name)) return malformed(run, step->index, "a document's name is text");
        while (k < run->documents.count && !(run->documents.items[k].name.length == name.length &&
                                              !memcmp(run->documents.items[k].name.pointer, name.pointer, name.length)))
            k++;
        if (k == run->documents.count) return malformed(run, step->index, "image names a document the call does not have");
        charts[i] = run->documents.items[k];
    }
    conform_image image;
    conform_documents documents = {{charts, names->count}, {run->documents.items, run->documents.count}};
    if (conform_image_compile(&documents, &image) != TA_OK) {
        run->outcome = "compile";
        return 0;
    }
    uint32_t segment;
    ta_status status = ta_host_add_image(run->host, image.bytes, &segment); /* the host copies it */
    conform_image_free(&image);
    return status == TA_OK || refused(run, "ta_host_add_image refused an image");
}

static int step_spawn(scenario *run, const scenario_step *step) {
    ta_string_view alias, chart;
    if (!conform_token_text(run->call, step->value, &alias) || !step_text(run, step->at, "chart", &chart) ||
        alias.length >= sizeof run->aliases[0].name)
        return malformed(run, step->index, "spawn names an alias and a chart");
    if (run->alias_count == sizeof run->aliases / sizeof run->aliases[0]) return refused(run, "too many actors");
    ta_host_chart_ref reference;
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    if (ta_host_chart(run->host, chart, &reference) != TA_OK) return refused(run, "ta_host_chart: no such chart");
    spawn.segment = reference.segment;
    spawn.chart = reference.chart;
    spawn.name = alias;
    if (value_read(run, ta_cbor_member(&run->call->document, step->at, ta_string("params")), &spawn.params) != TA_OK)
        return refused(run, "the spawn's params are not a value the datamodel reads");
    conform_alias *added = &run->aliases[run->alias_count];
    if (ta_host_spawn(run->host, &spawn, &added->session) != TA_OK) {
        conform_datamodel_release(&run->datamodel, spawn.params);
        return refused(run, "ta_host_spawn refused");
    }
    snprintf(added->name, sizeof added->name, "%.*s", (int)alias.length, alias.pointer);
    run->alias_count++;
    return 1;
}

/* The event of a send step's map (`to` names the alias: "send" or "to"). */
typedef struct scenario_event_read {
    uint32_t at;
    const char *to;
} scenario_event_read;

static int event_read(scenario *run, const scenario_event_read *read, ta_event_in *event) {
    ta_string_view to;
    memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    if (!step_text(run, read->at, read->to, &to) || !step_text(run, read->at, "event", &event->name)) return 0;
    const conform_alias *alias = alias_named(run, to);
    if (!alias) return refused(run, "a send to an alias no spawn made");
    event->session = alias->session;
    if (value_read(run, ta_cbor_member(&run->call->document, read->at, ta_string("data")), &event->data) != TA_OK)
        return refused(run, "the event's data is not a value the datamodel reads");
    return 1;
}

static int step_send(scenario *run, const scenario_step *step) {
    ta_event_in event;
    if (!event_read(run, &(scenario_event_read){step->at, "send"}, &event))
        return run->outcome ? 0 : malformed(run, step->index, "send names an alias and an event");
    if (ta_host_send(run->host, &event) == TA_OK) return 1;
    conform_datamodel_release(&run->datamodel, event.data);
    return refused(run, "ta_host_send refused");
}

static int step_send_many(scenario *run, const scenario_step *step) {
    const ta_cbor_document *document = &run->call->document;
    static ta_event_in events[64];
    uint32_t count = document->tokens[step->value].count, item = step->value + 1, read = 0;
    if (document->tokens[step->value].kind != TA_CBOR_ARRAY || count > 64)
        return malformed(run, step->index, "sendMany holds an array of sends");
    for (; read < count; read++, item = ta_cbor_next(document, item))
        if (!event_read(run, &(scenario_event_read){item, "to"}, &events[read])) break;
    uint32_t taken = read == count ? ta_host_send_many(run->host, events, count) : 0;
    for (uint32_t i = taken; i < read; i++)
        conform_datamodel_release(&run->datamodel, events[i].data);
    if (read < count) return run->outcome ? 0 : malformed(run, step->index, "each send names an alias and an event");
    return taken == count || refused(run, "ta_host_send_many took fewer events");
}

static int step_advance(scenario *run, const scenario_step *step) {
    const ta_cbor_token *ticks = &run->call->document.tokens[step->value];
    if (ticks->kind != TA_CBOR_UINT) return malformed(run, step->index, "advance takes ticks");
    run->now += (ta_time)ticks->value;
    ta_host_advance_time(run->host, (ta_host_time){run->now, 0});
    return 1;
}

static int step_hibernate(scenario *run, const scenario_step *step) {
    ta_string_view name;
    if (!conform_token_text(run->call, step->value, &name)) return malformed(run, step->index, "hibernate names an alias");
    const conform_alias *alias = alias_named(run, name);
    if (!alias) return refused(run, "hibernate of an alias no spawn made");
    if (run->store_count == sizeof run->store / sizeof run->store[0]) return refused(run, "the store is full");
    scenario_blob *blob = &run->store[run->store_count];
    *blob = (scenario_blob){alias->session, {NULL, 0, 0}};
    ta_bytes written = {NULL, 0}; /* the host's, until the ack: stored */
    if (ta_host_hibernate(run->host, alias->session, &written) != TA_OK) return refused(run, "ta_host_hibernate refused");
    if (!conform_buffer_reserve(&blob->bytes, written.length)) return refused(run, "no memory for the blob");
    memcpy(blob->bytes.pointer, written.pointer, written.length);
    ta_host_effects_ack(run->host); /* the blob is stored: the output's batch ends */
    blob->bytes.length = written.length;
    run->store_count++;
    return 1;
}

typedef struct packing {
    ta_host *host;
    const ta_datamodel_state *state;
} packing;

static ta_status pack_write(void *context, ta_buffer *out) {
    const packing *writing = (const packing *)context;
    return ta_host_snapshot_pack(writing->host, writing->state, out);
}

static int step_snapshot(scenario *run, const scenario_step *step) {
    ta_string_view name;
    if (!conform_token_text(run->call, step->value, &name) || name.length >= sizeof run->snapshots[0].name)
        return malformed(run, step->index, "snapshot names it");
    if (run->snapshot_count == sizeof run->snapshots / sizeof run->snapshots[0]) return refused(run, "too many snapshots");
    scenario_snapshot *snapshot = &run->snapshots[run->snapshot_count];
    memset(snapshot, 0, sizeof *snapshot);
    ta_buffer state = {NULL, 0, 0};
    ta_status status = conform_datamodel_snapshot(&run->datamodel, &state);
    ta_datamodel_state labelled = {ta_string(run->datamodel.binding.vtable->name), {state.pointer, state.length}};
    packing writing = {run->host, &labelled};
    if (status == TA_OK) status = conform_write_growing(&(conform_writing){pack_write, &writing}, &snapshot->pack);
    conform_buffer_free(&state);
    if (status != TA_OK) {
        conform_buffer_free(&snapshot->pack);
        return refused(run, "the snapshot failed");
    }
    snprintf(snapshot->name, sizeof snapshot->name, "%.*s", (int)name.length, name.pointer);
    run->snapshot_count++;
    return 1;
}

/* A restart: the host and its datamodel go, and come back from the
 * snapshot's bytes, with the images it holds. */
static int step_restore(scenario *run, const scenario_step *step) {
    ta_string_view name;
    if (!conform_token_text(run->call, step->value, &name)) return malformed(run, step->index, "restore names a snapshot");
    scenario_snapshot *snapshot = NULL;
    for (uint32_t i = 0; i < run->snapshot_count && !snapshot; i++)
        if (strlen(run->snapshots[i].name) == name.length && !memcmp(run->snapshots[i].name, name.pointer, name.length))
            snapshot = &run->snapshots[i];
    if (!snapshot) return refused(run, "restore of a snapshot no step took");
    ta_bytes pack = {snapshot->pack.pointer, snapshot->pack.length};
    ta_host_snapshot_contents contents;
    memset(&contents, 0, sizeof contents);
    contents.size = sizeof contents;
    if (ta_host_snapshot_unpack(pack, &contents) != TA_OK || contents.image_count > 16)
        return refused(run, "ta_host_snapshot_unpack refused the snapshot");
    for (uint32_t i = 0; i < contents.image_count; i++) /* empty for a retired segment */
        snapshot->images[i] = ta_host_snapshot_image(pack, i);
    ta_host_free(run->host);
    run->host = NULL;
    if (!conform_datamodel_reopen(&run->datamodel)) return refused(run, "the datamodel could not be made again");
    ta_host_environment environment = conform_datamodel_environment(&run->datamodel, NULL);
    ta_host_snapshot_input input = {{run->config, run->config_length},
                                    {contents.host, contents.host_length},
                                    snapshot->images,
                                    contents.image_count,
                                    0};
    if (ta_host_restore(&environment, &input, &run->host) != TA_OK) return refused(run, "ta_host_restore refused");
    if (conform_datamodel_restore(&run->datamodel, (ta_bytes){contents.state, contents.state_length}) != TA_OK)
        return refused(run, "the datamodel's restore refused");
    return 1;
}

static int step_migrate(scenario *run, const scenario_step *step) {
    const ta_cbor_document *document = &run->call->document;
    if (document->tokens[step->value].kind != TA_CBOR_MAP) return malformed(run, step->index, "migrate takes a map");
    uint32_t wake_at = ta_cbor_member(document, step->value, ta_string("wake"));
    uint32_t timeout_at = ta_cbor_member(document, step->value, ta_string("timeout"));
    ta_host_migration migration;
    memset(&migration, 0, sizeof migration);
    if (wake_at != TA_NIL && document->tokens[wake_at].kind == TA_CBOR_SIMPLE &&
        document->tokens[wake_at].value == TA_CBOR_TRUE)
        migration.flags = TA_HOST_MIGRATE_WAKE;
    if (timeout_at != TA_NIL && document->tokens[timeout_at].kind == TA_CBOR_UINT)
        migration.timeout = (ta_time)document->tokens[timeout_at].value;
    uint32_t job;
    run->job_done = 0;
    if (ta_host_migrate(run->host, &migration, &job) != TA_OK) return refused(run, "ta_host_migrate refused");
    for (int round = 0; round < 100 && !run->job_done && !run->outcome; round++)
        settle(run);
    return run->job_done || run->outcome || refused(run, "the migration did not end");
}

typedef int (*step_function)(scenario *run, const scenario_step *step);
static const struct {
    const char *name;
    step_function function;
} step_kinds[] = {
    {"image", step_image},       {"spawn", step_spawn},       {"send", step_send},
    {"sendMany", step_send_many}, {"advance", step_advance},   {"hibernate", step_hibernate},
    {"snapshot", step_snapshot}, {"restore", step_restore},   {"migrate", step_migrate},
};

/* One step: its kind is the member of its map that names one. */
static int step_run(scenario *run, uint32_t index, uint32_t at) {
    const ta_cbor_document *document = &run->call->document;
    if (document->tokens[at].kind != TA_CBOR_MAP) return malformed(run, index, "a step is a map");
    for (size_t k = 0; k < sizeof step_kinds / sizeof step_kinds[0]; k++) {
        uint32_t value = ta_cbor_member(document, at, ta_string(step_kinds[k].name));
        if (value == TA_NIL) continue;
        scenario_step step = {index, at, value};
        if (!step_kinds[k].function(run, &step)) return 0;
        settle(run);
        return 1;
    }
    return malformed(run, index, "not a step of driver.md");
}

/* ---- the method ------------------------------------------------------------------------------ */

static void scenario_free(scenario *run) {
    if (run->host) ta_host_free(run->host);
    conform_datamodel_close(&run->datamodel);
    for (uint32_t i = 0; i < run->store_count; i++)
        conform_buffer_free(&run->store[i].bytes);
    for (uint32_t i = 0; i < run->snapshot_count; i++)
        conform_buffer_free(&run->snapshots[i].pack);
    memset(run, 0, sizeof *run);
}

static void scenario_run(conform *state, conform_call *call, conform_result *result) {
    (void)state;
    static scenario run;
    memset(&run, 0, sizeof run);
    run.call = call;
    uint32_t steps = conform_param(call, "steps");
    if (!conform_required(call, "documents") || !conform_required(call, "steps") ||
        !conform_sources_read(call, "documents", &run.documents))
        return;
    if (call->document.tokens[steps].kind != TA_CBOR_ARRAY) {
        conform_fail(call, &(conform_failure){CONFORM_INVALID_PARAMS, "type", "params.steps"}, "steps is an array");
        return;
    }
    if (!conform_datamodel_open(&run.datamodel, call)) return;
    ta_buffer config = {run.config, sizeof run.config, 0};
    ta_host_environment environment = conform_datamodel_environment(&run.datamodel, NULL);
    ta_status status = config_write(NULL, &config);
    run.config_length = config.length;
    if (status == TA_OK) status = ta_host_create((ta_bytes){run.config, run.config_length}, &environment, &run.host);
    if (status != TA_OK) refused(&run, "ta_host_create refused the scenario's config");
    uint32_t count = call->document.tokens[steps].count, at = steps + 1;
    for (uint32_t i = 0; i < count && !run.outcome && !call->error.code; i++, at = ta_cbor_next(&call->document, at))
        step_run(&run, i, at);
    if (!call->error.code) conform_result_text(result, "outcome", ta_string(run.outcome ? run.outcome : "timeout"));
    scenario_free(&run);
}

static const char *const run_params[] = {"documents", "steps", "datamodel", "realm", NULL};

const conform_method conform_scenario_methods[] = {
    {"scenario.run", scenario_run, run_params},
    {NULL, NULL, NULL},
};

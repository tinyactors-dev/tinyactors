/* system_util.h — test harness around ta_system: compile, create, run, collect. */
#ifndef TINYACTORS_TESTS_SUPPORT_SYSTEM_UTIL_H
#define TINYACTORS_TESTS_SUPPORT_SYSTEM_UTIL_H

#include "compile_util.h"
#include "datamodel_test.h"
#include <tinyactors/system.h>

typedef struct test_system {
    void *bytes;
    size_t length;
    ta_image image;
    datamodel_test *datamodel;
    ta_datamodel_binding binding;
    void *memory;
    size_t memory_length;
    ta_system *system;
    char log[1 << 16];
    int effect_count[8];
    ta_effect last[8];   /* last effect seen per kind (views valid until next call) */
    char done_state[64]; /* aux of the last TA_EFFECT_DONE (top-level final id) */
} test_system;

TEST_UNUSED static ta_system_config test_system_config(void) {
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.ticks_per_second = 1000; /* milliseconds */
    return config;
}

TEST_UNUSED static int tsys_open_image(test_system *t, const char *name, const char *chart,
                                       const test_file *files) {
    memset(t, 0, sizeof *t);
    t->bytes = compile_chart(name, chart, files, &t->length, 0);
    if (!t->bytes || ta_image_open((ta_bytes){t->bytes, t->length}, &t->image) != TA_OK) return 0;
    t->datamodel = datamodel_test_new();
    t->binding = datamodel_test_binding(t->datamodel);
    return 1;
}

TEST_UNUSED static int tsys_create(test_system *t, const ta_system_config *config) {
    if (ta_system_layout(&t->image, config, &t->memory_length) != TA_OK) return 0;
    t->memory = malloc(t->memory_length);
    return ta_system_init(
               &(ta_system_setup){
                   {t->memory, t->memory_length}, &t->image, config, &t->binding, 1, 0},
               &t->system) == TA_OK;
}

TEST_UNUSED static int tsys_init(test_system *t, const char *chart, const test_file *files,
                                 const ta_system_config *config) {
    ta_system_config def = test_system_config();
    return tsys_open_image(t, "main.scxml", chart, files) && tsys_create(t, config ? config : &def);
}

/* Append one log effect as "label=text;". */
TEST_UNUSED static void tsys_drain_one(test_system *t, const ta_effect *effect) {
    size_t l = strlen(t->log);
    snprintf(t->log + l, sizeof t->log - l, "%.*s%s%.*s;", (int)effect->auxiliary.length,
             effect->auxiliary.pointer ? effect->auxiliary.pointer : "",
             effect->auxiliary.length ? "=" : "", (int)effect->event.length,
             effect->event.pointer ? effect->event.pointer : "");
}

/* Drain effects: append "label=text;" for logs, count kinds. */
TEST_UNUSED static void tsys_drain(test_system *t) {
    const ta_effect *effect;
    uint32_t n = ta_system_effects(t->system, &effect);
    for (uint32_t i = 0; i < n; i++) {
        if (effect[i].kind < 8) {
            t->effect_count[effect[i].kind]++;
            t->last[effect[i].kind] = effect[i];
        }
        if (effect[i].kind == TA_EFFECT_DONE)
            snprintf(t->done_state, sizeof t->done_state, "%.*s", (int)effect[i].auxiliary.length,
                     effect[i].auxiliary.pointer ? effect[i].auxiliary.pointer : "");
        if (effect[i].kind == TA_EFFECT_LOG) {
            size_t l = strlen(t->log);
            snprintf(t->log + l, sizeof t->log - l, "%.*s%s%.*s;", (int)effect[i].auxiliary.length,
                     effect[i].auxiliary.pointer ? effect[i].auxiliary.pointer : "",
                     effect[i].auxiliary.length ? "=" : "", (int)effect[i].event.length,
                     effect[i].event.pointer ? effect[i].event.pointer : "");
        }
    }
    /* keep `last` views valid for the caller: ack only logs/errors immediately */
    ta_system_effects_ack(t->system, n);
}

/* Run until idle (or `max_runs` budgets). Returns last status. */
TEST_UNUSED static uint32_t tsys_run(test_system *t, uint32_t budget, int max_runs) {
    ta_run result;
    result.status = TA_RUN_IDLE;
    for (int i = 0; i < max_runs; i++) {
        result = ta_system_run(t->system, budget);
        tsys_drain(t);
        if (result.status == TA_RUN_IDLE || result.status == TA_RUN_EXHAUSTED ||
            result.status == TA_RUN_FAULT)
            break;
    }
    return result.status;
}

TEST_UNUSED static ta_status tsys_send(test_system *t, uint32_t actor, const char *name,
                                       const char *data) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = actor;
    event.name.pointer = name;
    event.name.length = (uint32_t)strlen(name);
    event.data = data ? datamodel_test_payload(t->datamodel, data) : 0;
    return ta_system_send(t->system, &event);
}

TEST_UNUSED static uint32_t tsys_actor(test_system *t, const char *name) {
    ta_string_view n = {name, (uint32_t)strlen(name)};
    return ta_system_actor_by_name(t->system, n);
}

/* Active states of an actor as "a,b". */
TEST_UNUSED static const char *tsys_config(test_system *t, uint32_t actor) {
    static char buffer[1024];
    uint32_t states[128];
    ta_actor_info info;
    info.size = sizeof info;
    buffer[0] = 0;
    if (ta_actor_info_get(t->system, actor, &info) != TA_OK) return "<gone>";
    uint32_t n = ta_actor_config(t->system, actor, (ta_uint32_array){states, 128});
    for (uint32_t i = 0; i < n && i < 128; i++) {
        ta_string_view nm = ta_image_state_name(&t->image, info.chart, states[i]);
        size_t l = strlen(buffer);
        snprintf(buffer + l, sizeof buffer - l, "%s%.*s", l ? "," : "", (int)nm.length, nm.pointer);
    }
    return buffer;
}

#ifdef TINYACTORS_SYSTEM_SYSTEM_INTERNAL_H
/* Read a datamodel variable of an actor (test-only: peeks at internals). */
TEST_UNUSED static const char *tsys_var(test_system *t, uint32_t actor, const char *var) {
    uint32_t index = ta_system_resolve_actor(t->system, actor);
    if (index == TA_NIL) return "<gone>";
    return datamodel_test_variable(
        t->datamodel, ta_actor_instance(t->system, ta_actor_at(t->system, index))->datamodel, var);
}
#endif

TEST_UNUSED static void tsys_free(test_system *t) {
    if (t->system) ta_system_deinit(t->system);
    free(t->memory);
    if (t->datamodel) datamodel_test_free(t->datamodel);
    free(t->bytes);
}

#endif

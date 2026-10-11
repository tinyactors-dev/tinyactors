/* Host conformance driver for C (see tests/conformance/README.md): runs every
 * scenario on a ta_host with the quickjs-ng ECMAScript datamodel, on
 * simulated time, as an embedder would — the host keeps the hibernation
 * blobs and answers wakes, answers requests (dynamic <invoke src>) from the
 * segment's image, snapshots host + datamodel and restores both into fresh
 * ones (a restart), adds images for reloads and runs migration jobs.
 *
 *   conformance-c <dir> [--only name] [-v]
 *
 * Scenario files are JSON; they are parsed with the engine the datamodel
 * links anyway (quickjs-ng). */
#define _POSIX_C_SOURCE 200809L /* strdup, opendir: a hosted driver */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include <tinyactors.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include "../support/payload_text.h"
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

#define BASE       "tinyactors://conformance/"
#define MAX_ACTORS 64
#define MAX_IMAGES 32
#define MAX_LOGS   256

static int verbose;
static JSContext *J; /* scenario parsing and checks */

/* ---- small JS helpers --------------------------------------------------------- */
static JSValue get(JSValueConst o, const char *k) {
    return JS_GetPropertyStr(J, o, k);
}
static int has(JSValueConst o, const char *k) {
    JSAtom a = JS_NewAtom(J, k);
    int r = JS_HasProperty(J, o, a);
    JS_FreeAtom(J, a);
    return r > 0;
}
static char *str_of(JSValueConst v) { /* malloc'd */
    size_t n;
    const char *s = JS_ToCStringLen(J, &n, v);
    char *r = (char *)malloc(n + 1);
    if (s) memcpy(r, s, n);
    r[s ? n : 0] = 0;
    if (s) JS_FreeCString(J, s);
    return r;
}
static char *json_of(JSValueConst v) { /* malloc'd JSON.stringify */
    JSValue s = JS_JSONStringify(J, v, JS_UNDEFINED, JS_UNDEFINED);
    char *r = JS_IsUndefined(s) ? NULL : str_of(s);
    JS_FreeValue(J, s);
    return r;
}
static uint32_t length_of(JSValueConst a) {
    int64_t n = 0;
    JS_GetLength(J, a, &n);
    return (uint32_t)n;
}

/* ---- the host under test ------------------------------------------------------------ */
typedef struct conformance_image {
    char key[512]; /* document + documents */
    void *bytes;
    size_t length;
    ta_image image;
} conformance_image;

typedef struct conformance_actor {
    char name[32];
    uint64_t session;
    int done;
    char *output; /* donedata JSON (NULL: none) */
} conformance_actor;

typedef struct conformance_blob {
    uint64_t session;
    void *bytes;
    size_t length;
} conformance_blob;

typedef struct conformance_world {
    JSValue documents;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    ta_time now;
    conformance_image *segments[MAX_IMAGES]; /* by segment */
    uint32_t segment_count;
    conformance_image images[MAX_IMAGES];
    uint32_t image_count;
    conformance_actor actors[MAX_ACTORS];
    uint32_t actor_count;
    conformance_blob store[MAX_ACTORS];
    uint32_t store_count;
    char *logs[MAX_LOGS];
    uint32_t log_count;
    int job_done;
    void *snapshot_host[8], *snapshot_datamodel[8];
    size_t snapshot_host_length[8], snapshot_datamodel_length[8];
    char snapshot_name[8][32];
    uint32_t snapshot_count;
    char error[1024];
} conformance_world;

static void host_open(conformance_world *w) {
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
}
static ta_host_config host_config(conformance_world *w) {
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"conformance", 11};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000; /* ms */
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    return config;
}

static char *document_text(conformance_world *w, const char *name) { /* malloc'd, lines joined */
    JSValue d = get(w->documents, name);
    char *r = NULL;
    if (JS_IsArray(d)) {
        size_t capacity = 1, n = 0;
        uint32_t k = length_of(d);
        r = (char *)calloc(1, 1);
        for (uint32_t i = 0; i < k; i++) {
            JSValue l = JS_GetPropertyUint32(J, d, i);
            char *s = str_of(l);
            JS_FreeValue(J, l);
            size_t sl = strlen(s);
            r = (char *)realloc(r, capacity += sl + 1);
            memcpy(r + n, s, sl);
            n += sl;
            r[n++] = '\n';
            r[n] = 0;
            free(s);
        }
    } else if (JS_IsString(d)) {
        r = str_of(d);
    }
    JS_FreeValue(J, d);
    return r;
}

/* the image of `document` (+ the documents listed), compiled once */
static conformance_image *image_for(conformance_world *w, const char *document,
                                    JSValueConst documents) {
    char key[512];
    snprintf(key, sizeof key, "%s", document);
    uint32_t nd = JS_IsArray(documents) ? length_of(documents) : 0;
    char *names[8] = {0};
    for (uint32_t i = 0; i < nd && i < 7; i++) {
        JSValue v = JS_GetPropertyUint32(J, documents, i);
        names[i] = str_of(v);
        JS_FreeValue(J, v);
        strncat(key, "+", sizeof key - strlen(key) - 1);
        strncat(key, names[i], sizeof key - strlen(key) - 1);
    }
    for (uint32_t i = 0; i < w->image_count; i++)
        if (!strcmp(w->images[i].key, key)) {
            for (uint32_t k = 0; k < 8; k++)
                free(names[k]);
            return &w->images[i];
        }
    ta_source source[8];
    char *texts[8] = {0};
    texts[0] = document_text(w, document);
    source[0] =
        (ta_source){{document, (uint32_t)strlen(document)}, {texts[0], (uint32_t)strlen(texts[0])}};
    uint32_t n = 1;
    for (uint32_t i = 0; i < nd && i < 7; i++, n++) {
        texts[n] = document_text(w, names[i]);
        source[n] = (ta_source){{names[i], (uint32_t)strlen(names[i])},
                                {texts[n], (uint32_t)strlen(texts[n])}};
    }
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_compile_result *result = NULL;
    conformance_image *im = NULL;
    if (ta_compile_charts(&options, source, n, &result) == TA_OK && w->image_count < MAX_IMAGES) {
        ta_string_view bytes = ta_compile_image(result);
        im = &w->images[w->image_count];
        snprintf(im->key, sizeof im->key, "%s", key);
        im->bytes = malloc(bytes.length + 8);
        memcpy(im->bytes, bytes.pointer, bytes.length);
        im->length = bytes.length;
        if (ta_image_open(im->bytes, im->length, &im->image) == TA_OK) {
            w->image_count++;
        } else {
            free(im->bytes);
            im = NULL;
        }
    } else {
        const ta_diagnostic *diagnostic;
        uint32_t ndg = ta_compile_diagnostics(result, &diagnostic);
        snprintf(w->error, sizeof w->error, "compile %s: %.*s", document,
                 ndg ? (int)diagnostic[0].message.length : 0,
                 ndg ? diagnostic[0].message.pointer : "");
    }
    ta_compile_free(result);
    for (uint32_t k = 0; k < 8; k++) {
        free(names[k]);
        free(texts[k]);
    }
    return im;
}

/* a new segment running the image (images linked once; a reload links a new one) */
static int link(conformance_world *w, conformance_image *im) {
    for (uint32_t s = 0; s < w->segment_count; s++)
        if (w->segments[s] == im) return 1;
    uint32_t segment;
    if (ta_host_add_image(w->host, &im->image, &segment) != TA_OK || segment != w->segment_count)
        return 0;
    w->segments[w->segment_count++] = im;
    return 1;
}

static conformance_actor *actor_of(conformance_world *w, const char *name) {
    for (uint32_t i = 0; i < w->actor_count; i++)
        if (!strcmp(w->actors[i].name, name)) return &w->actors[i];
    return NULL;
}
static conformance_actor *actor_by_session(conformance_world *w, uint64_t s) {
    for (uint32_t i = 0; i < w->actor_count; i++)
        if (w->actors[i].session == s) return &w->actors[i];
    return NULL;
}
static conformance_blob *stored(conformance_world *w, uint64_t s) {
    for (uint32_t i = 0; i < w->store_count; i++)
        if (w->store[i].session == s) return &w->store[i];
    return NULL;
}

/* a request: a dynamic src -> a chart of the segment's image, or an error */
static void answer_request(conformance_world *w, uint32_t segment, uint32_t token,
                           ta_string_view source) {
    const char *pre[] = {"file://", "file:", "./"};
    for (int k = 0; k < 3; k++) {
        size_t l = strlen(pre[k]);
        if (source.length > l && !memcmp(source.pointer, pre[k], l)) {
            source.pointer += l;
            source.length -= (uint32_t)l;
        }
    }
    uint32_t chart = segment < w->segment_count
                         ? ta_image_chart_by_name(&w->segments[segment]->image, source)
                         : TA_NIL;
    ta_host_resume(w->host, segment, token, chart == TA_NIL ? TA_ERROR_INVALID : TA_OK,
                   chart == TA_NIL ? 0 : (ta_payload)chart + 1);
}

/* run until idle at the current time: logs, done outputs, wakes, requests, jobs */
static void settle(conformance_world *w) {
    for (int round = 0; round < 1000; round++) {
        ta_run result = ta_host_run(w->host, 100000);
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(w->host, &effects);
        uint64_t wakes[MAX_ACTORS];
        uint32_t nw = 0;
        struct {
            uint32_t segment, token;
            char source[256];
        } req[16];
        uint32_t nr = 0;
        for (uint32_t i = 0; i < n; i++) {
            const ta_host_effect *effect = &effects[i];
            if (effect->kind == TA_EFFECT_LOG && w->log_count < MAX_LOGS) {
                char *m = (char *)malloc(effect->event.length + 1);
                memcpy(m, effect->event.pointer, effect->event.length);
                m[effect->event.length] = 0;
                w->logs[w->log_count++] = m;
            } else if (effect->kind == TA_EFFECT_DONE) {
                conformance_actor *a = actor_by_session(w, effect->session);
                if (a) {
                    a->done = 1;
                    if (effect->payload) {
                        char buffer[4096];
                        payload_text(w->ecmascript, effect->payload,
                                                                buffer, sizeof buffer);
                        a->output = strdup(buffer);
                    }
                }
            } else if (effect->kind == TA_EFFECT_WAKE && nw < MAX_ACTORS) {
                wakes[nw++] = effect->session;
            } else if (effect->kind == TA_EFFECT_REQUEST && nr < 16) {
                req[nr].segment = effect->segment;
                req[nr].token = effect->token;
                snprintf(req[nr].source, sizeof req[nr].source, "%.*s",
                         (int)effect->auxiliary.length, effect->auxiliary.pointer);
                nr++;
            } else if (effect->kind == TA_HOST_EFFECT_JOB_DONE) {
                w->job_done = 1;
            }
        }
        ta_host_effects_ack(w->host);
        for (uint32_t i = 0; i < nw; i++) { /* the host reads the blob back */
            conformance_blob *b = stored(w, wakes[i]);
            if (b && ta_host_wake(w->host, b->session, b->bytes, b->length) == TA_OK) {
                free(b->bytes);
                *b = w->store[--w->store_count];
            }
        }
        for (uint32_t i = 0; i < nr; i++)
            answer_request(w, req[i].segment, req[i].token,
                           (ta_string_view){req[i].source, (uint32_t)strlen(req[i].source)});
        if (result.status == TA_RUN_IDLE && !nw && !nr) return;
    }
}

/* data with "@name" replaced by "#_scxml_<session>", as a payload */
static ta_payload payload(conformance_world *w, JSValueConst data) {
    if (JS_IsUndefined(data)) return 0;
    JSValue map = JS_NewObject(J);
    for (uint32_t i = 0; i < w->actor_count; i++) {
        char t[48];
        snprintf(t, sizeof t, "#_scxml_%llu", (unsigned long long)w->actors[i].session);
        JS_SetPropertyStr(J, map, w->actors[i].name, JS_NewString(J, t));
    }
    static const char function[] =
        "(function s(v, m) { if (typeof v === 'string' && v[0] === '@') "
        "return m[v.slice(1)]; if (Array.isArray(v)) return v.map(x => s(x, "
        "m)); if (v && typeof v === 'object') { const o = {}; for (const k "
        "in v) o[k] = s(v[k], m); return o; } return v; })";
    JSValue f = JS_Eval(J, function, sizeof function - 1, "<subst>", JS_EVAL_TYPE_GLOBAL);
    JSValue argv[2] = {JS_DupValue(J, data), map};
    JSValue v = JS_Call(J, f, JS_UNDEFINED, 2, argv);
    char *json = json_of(v);
    ta_payload payload_value =
        json ? ta_datamodel_ecmascript_payload_eval(
                   w->ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)})
             : 0;
    free(json);
    JS_FreeValue(J, v);
    JS_FreeValue(J, f);
    JS_FreeValue(J, argv[0]);
    JS_FreeValue(J, map);
    return payload_value;
}

static ta_event_in event_in(const char *name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    return event;
}

/* the actor's atomic states, sorted, as JSON */
static char *states_json(conformance_world *w, conformance_actor *a) {
    uint32_t segment, act;
    JSValue arr = JS_NewArray(J);
    if (ta_host_locate(w->host, a->session, &segment, &act) == TA_OK) {
        ta_system *system = ta_host_system(w->host, segment);
        ta_actor_info actor_info;
        memset(&actor_info, 0, sizeof actor_info);
        actor_info.size = sizeof actor_info;
        ta_actor_info_get(system, act, &actor_info);
        uint32_t states[256], n = ta_actor_config(system, act, states, 256), k = 0;
        for (uint32_t i = 0; i < n && i < 256; i++) {
            ta_state_info state_info;
            if (ta_image_state_info(&w->segments[segment]->image, actor_info.chart, states[i],
                                    &state_info) != TA_OK)
                continue;
            if (state_info.kind == TA_STATE_ATOMIC || state_info.kind == TA_STATE_FINAL)
                JS_SetPropertyUint32(
                    J, arr, k++, JS_NewStringLen(J, state_info.id.pointer, state_info.id.length));
        }
    }
    static const char sort[] = "(a => a.sort())";
    JSValue f = JS_Eval(J, sort, sizeof sort - 1, "<sort>", JS_EVAL_TYPE_GLOBAL);
    JSValue s = JS_Call(J, f, JS_UNDEFINED, 1, (JSValueConst *)&arr);
    char *r = json_of(s);
    JS_FreeValue(J, s);
    JS_FreeValue(J, f);
    JS_FreeValue(J, arr);
    return r;
}

static char *sorted_json(JSValueConst arr) {
    static const char sort[] = "(a => [...a].sort())";
    JSValue f = JS_Eval(J, sort, sizeof sort - 1, "<sort>", JS_EVAL_TYPE_GLOBAL);
    JSValue s = JS_Call(J, f, JS_UNDEFINED, 1, &arr);
    char *r = json_of(s);
    JS_FreeValue(J, s);
    JS_FreeValue(J, f);
    return r;
}

static char *logs_json(conformance_world *w, int sorted) {
    JSValue arr = JS_NewArray(J);
    for (uint32_t i = 0; i < w->log_count; i++)
        JS_SetPropertyUint32(J, arr, i, JS_NewString(J, w->logs[i]));
    char *r = sorted ? sorted_json(arr) : json_of(arr);
    JS_FreeValue(J, arr);
    return r;
}

static int failf(char *out, size_t capacity, const char *format, const char *a, const char *b,
                 const char *c) {
    size_t l = strlen(out);
    snprintf(out + l, capacity - l, format, a, b, c);
    return 1;
}

static int expect(conformance_world *w, JSValueConst e, char *out, size_t capacity) {
    int bad = 0;
    for (int unordered = 0; unordered < 2; unordered++) {
        const char *key = unordered ? "logsUnordered" : "logs";
        if (!has(e, key)) continue;
        JSValue want = get(e, key);
        char *g = logs_json(w, unordered), *x = unordered ? sorted_json(want) : json_of(want);
        if (strcmp(g, x)) bad |= failf(out, capacity, "\n       %s %s, expected %s", key, g, x);
        free(g);
        free(x);
        JS_FreeValue(J, want);
    }
    for (uint32_t i = 0; i < w->log_count; i++)
        free(w->logs[i]);
    w->log_count = 0;
    JSValue states = get(e, "states"), done = get(e, "done"), hib = get(e, "hibernated"),
            awake = get(e, "awake");
    JSPropertyEnum *props;
    uint32_t np;
    if (JS_IsObject(states) &&
        !JS_GetOwnPropertyNames(J, &props, &np, states, JS_GPN_STRING_MASK)) {
        for (uint32_t i = 0; i < np; i++) {
            const char *name = JS_AtomToCString(J, props[i].atom);
            JSValue want = JS_GetProperty(J, states, props[i].atom);
            conformance_actor *a = actor_of(w, name);
            char *g = a ? states_json(w, a) : strdup("(no actor)"), *x = sorted_json(want);
            if (strcmp(g, x))
                bad |= failf(out, capacity, "\n       %s in %s, expected %s", name, g, x);
            free(g);
            free(x);
            JS_FreeValue(J, want);
            JS_FreeCString(J, name);
        }
        JS_FreePropertyEnum(J, props, np);
    }
    if (JS_IsObject(done) && !JS_GetOwnPropertyNames(J, &props, &np, done, JS_GPN_STRING_MASK)) {
        for (uint32_t i = 0; i < np; i++) {
            const char *name = JS_AtomToCString(J, props[i].atom);
            JSValue want = JS_GetProperty(J, done, props[i].atom);
            conformance_actor *a = actor_of(w, name);
            if (!a || !a->done) {
                bad |= failf(out, capacity, "\n       %s is not done%s%s", name, "", "");
            } else if (!JS_IsBool(want)) {
                char *x = json_of(want);
                if (!a->output || strcmp(a->output, x))
                    bad |= failf(out, capacity, "\n       %s output %s, expected %s", name,
                                 a->output ? a->output : "(none)", x);
                free(x);
            }
            JS_FreeValue(J, want);
            JS_FreeCString(J, name);
        }
        JS_FreePropertyEnum(J, props, np);
    }
    for (int want_hib = 1; want_hib >= 0; want_hib--) {
        JSValue list = want_hib ? hib : awake;
        for (uint32_t i = 0; JS_IsArray(list) && i < length_of(list); i++) {
            JSValue v = JS_GetPropertyUint32(J, list, i);
            char *name = str_of(v);
            conformance_actor *a = actor_of(w, name);
            int is = a && stored(w, a->session) != NULL;
            if (is != want_hib)
                bad |= failf(out, capacity, "\n       %s is %s%s", name,
                             want_hib ? "not hibernated" : "hibernated", "");
            free(name);
            JS_FreeValue(J, v);
        }
    }
    JS_FreeValue(J, states);
    JS_FreeValue(J, done);
    JS_FreeValue(J, hib);
    JS_FreeValue(J, awake);
    return bad;
}

static int run_scenario(JSValueConst sc, const char *file) {
    static conformance_world world;
    conformance_world *w = &world;
    memset(w, 0, sizeof *w);
    w->documents = get(sc, "documents");
    host_open(w);
    ta_host_config config = host_config(w);
    ta_host_new(&config, &w->host);
    JSValue steps = get(sc, "steps"), name = get(sc, "name");
    char *sname = str_of(name);
    char fail[8192] = "";
    uint32_t n = length_of(steps), i;
    for (i = 0; i < n && !fail[0]; i++) {
        JSValue s = JS_GetPropertyUint32(J, steps, i);
        char *sj = json_of(s), where[160];
        snprintf(where, sizeof where, "step %u %.80s", i + 1, sj);
        if (verbose) printf("  %s\n", where);
        if (has(s, "spawn")) {
            JSValue d = get(s, "spawn"), documents = get(s, "documents"), as = get(s, "as"),
                    input = get(s, "input");
            char *document = str_of(d), *an = str_of(as);
            conformance_image *im = image_for(w, document, documents);
            uint32_t segment, chart;
            uint64_t session = 0;
            ta_string_view cname =
                im ? ta_image_chart_name(
                         &im->image,
                         ta_image_chart_by_name(
                             &im->image, (ta_string_view){document, (uint32_t)strlen(document)}))
                   : (ta_string_view){0, 0};
            if (!im || !link(w, im) || ta_host_chart(w->host, cname, &segment, &chart) != TA_OK ||
                ta_host_spawn(w->host, segment, chart, (ta_string_view){an, (uint32_t)strlen(an)},
                              payload(w, input), (ta_string_view){0, 0}, &session) != TA_OK) {
                snprintf(fail, sizeof fail, "%s: cannot spawn (%s)", where, w->error);
            } else {
                conformance_actor *a = &w->actors[w->actor_count++];
                snprintf(a->name, sizeof a->name, "%s", an);
                a->session = session;
            }
            free(document);
            free(an);
            JS_FreeValue(J, d);
            JS_FreeValue(J, documents);
            JS_FreeValue(J, as);
            JS_FreeValue(J, input);
        } else if (has(s, "send")) {
            JSValue to = get(s, "send"), event_value = get(s, "event"), data = get(s, "data");
            char *tn = str_of(to), *en = str_of(event_value);
            conformance_actor *a = actor_of(w, tn);
            ta_event_in e = event_in(en);
            e.data = payload(w, data);
            if (!a || ta_host_send(w->host, a->session, &e) != TA_OK)
                snprintf(fail, sizeof fail, "%s: cannot send", where);
            free(tn);
            free(en);
            JS_FreeValue(J, to);
            JS_FreeValue(J, event_value);
            JS_FreeValue(J, data);
        } else if (has(s, "sendMany")) {
            JSValue list = get(s, "sendMany");
            uint32_t k = length_of(list);
            uint64_t sessions[64];
            ta_event_in events[64];
            char *names[64];
            for (uint32_t j = 0; j < k && j < 64; j++) {
                JSValue it = JS_GetPropertyUint32(J, list, j), to = get(it, "to"),
                        event_value = get(it, "event"), data = get(it, "data");
                char *tn = str_of(to);
                names[j] = str_of(event_value);
                conformance_actor *a = actor_of(w, tn);
                sessions[j] = a ? a->session : 0;
                events[j] = event_in(names[j]);
                events[j].data = payload(w, data);
                free(tn);
                JS_FreeValue(J, to);
                JS_FreeValue(J, event_value);
                JS_FreeValue(J, data);
                JS_FreeValue(J, it);
            }
            if (ta_host_send_many(w->host, sessions, events, k) != k)
                snprintf(fail, sizeof fail, "%s: send_many accepted fewer", where);
            for (uint32_t j = 0; j < k && j < 64; j++)
                free(names[j]);
            JS_FreeValue(J, list);
        } else if (has(s, "advance")) {
            JSValue v = get(s, "advance");
            double ms = 0;
            JS_ToFloat64(J, &ms, v);
            w->now += (ta_time)ms;
            ta_host_advance_time(w->host, w->now);
        } else if (has(s, "hibernate")) {
            JSValue v = get(s, "hibernate");
            char *an = str_of(v);
            conformance_actor *a = actor_of(w, an);
            static unsigned char buffer[1 << 16];
            size_t length = 0;
            if (!a || w->store_count >= MAX_ACTORS ||
                ta_host_hibernate(w->host, a->session, buffer, sizeof buffer, &length) != TA_OK) {
                snprintf(fail, sizeof fail, "%s: could not hibernate", where);
            } else { /* the host's store */
                conformance_blob *b = &w->store[w->store_count++];
                b->session = a->session;
                b->bytes = malloc(length);
                memcpy(b->bytes, buffer, length);
                b->length = length;
            }
            free(an);
            JS_FreeValue(J, v);
        } else if (has(s, "snapshot")) {
            JSValue v = get(s, "snapshot");
            char *id = str_of(v);
            uint32_t k = w->snapshot_count++;
            snprintf(w->snapshot_name[k], sizeof w->snapshot_name[k], "%s", id);
            ta_host_snapshot(w->host, NULL, 0, &w->snapshot_host_length[k]);
            w->snapshot_host[k] = malloc(w->snapshot_host_length[k]);
            ta_datamodel_ecmascript_snapshot(w->ecmascript, NULL, 0,
                                             &w->snapshot_datamodel_length[k]);
            w->snapshot_datamodel[k] = malloc(w->snapshot_datamodel_length[k]);
            if (ta_host_snapshot(w->host, w->snapshot_host[k], w->snapshot_host_length[k],
                                 &w->snapshot_host_length[k]) != TA_OK ||
                ta_datamodel_ecmascript_snapshot(w->ecmascript, w->snapshot_datamodel[k],
                                                 w->snapshot_datamodel_length[k],
                                                 &w->snapshot_datamodel_length[k]) != TA_OK)
                snprintf(fail, sizeof fail, "%s: snapshot failed", where);
            free(id);
            JS_FreeValue(J, v);
        } else if (has(s, "restore")) {
            JSValue v = get(s, "restore");
            char *id = str_of(v);
            uint32_t k = 0;
            while (k < w->snapshot_count && strcmp(w->snapshot_name[k], id))
                k++;
            /* a restart: host and datamodel go; both come back from bytes */
            ta_host_free(w->host);
            ta_datamodel_ecmascript_free(w->ecmascript);
            host_open(w);
            config = host_config(w);
            const ta_image *images[MAX_IMAGES];
            for (uint32_t s2 = 0; s2 < w->segment_count; s2++)
                images[s2] = &w->segments[s2]->image;
            if (k == w->snapshot_count ||
                ta_host_restore(&config, images, w->segment_count, w->snapshot_host[k],
                                w->snapshot_host_length[k], &w->host) != TA_OK ||
                ta_datamodel_ecmascript_restore(w->ecmascript, w->snapshot_datamodel[k],
                                                w->snapshot_datamodel_length[k]) != TA_OK)
                snprintf(fail, sizeof fail, "%s: restore failed", where);
            free(id);
            JS_FreeValue(J, v);
        } else if (has(s, "reload")) {
            JSValue d = get(s, "reload"), documents = get(s, "documents");
            char *document = str_of(d);
            conformance_image *im = image_for(w, document, documents);
            if (!im || !link(w, im)) snprintf(fail, sizeof fail, "%s: cannot reload", where);
            free(document);
            JS_FreeValue(J, d);
            JS_FreeValue(J, documents);
        } else if (has(s, "migrate")) {
            JSValue o = get(s, "migrate"), wake = get(o, "wake"), to = get(o, "timeoutMs");
            double tmo = 0;
            if (!JS_IsUndefined(to)) JS_ToFloat64(J, &tmo, to);
            uint32_t job;
            w->job_done = 0;
            if (ta_host_migrate(w->host, (ta_string_view){0, 0}, 0,
                                JS_ToBool(J, wake) ? TA_HOST_MIGRATE_WAKE : 0, (ta_time)tmo,
                                &job) != TA_OK)
                snprintf(fail, sizeof fail, "%s: cannot migrate", where);
            for (int r = 0; r < 100 && !w->job_done && !fail[0]; r++)
                settle(w);
            if (!w->job_done && !fail[0])
                snprintf(fail, sizeof fail, "%s: the migration did not end", where);
            JS_FreeValue(J, o);
            JS_FreeValue(J, wake);
            JS_FreeValue(J, to);
        } else if (has(s, "expect")) {
            JSValue e = get(s, "expect");
            char out[4096] = "";
            if (expect(w, e, out, sizeof out)) snprintf(fail, sizeof fail, "%s:%s", where, out);
            JS_FreeValue(J, e);
        } else {
            snprintf(fail, sizeof fail, "%s: unknown step", where);
        }
        if (!has(s, "expect")) settle(w);
        free(sj);
        JS_FreeValue(J, s);
    }
    printf("%s c    %s\n", fail[0] ? "FAIL" : "ok  ", sname);
    if (fail[0]) printf("       %s\n", fail);
    (void)file;
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    for (uint32_t k = 0; k < w->image_count; k++)
        free(w->images[k].bytes);
    for (uint32_t k = 0; k < w->store_count; k++)
        free(w->store[k].bytes);
    for (uint32_t k = 0; k < w->snapshot_count; k++) {
        free(w->snapshot_host[k]);
        free(w->snapshot_datamodel[k]);
    }
    for (uint32_t k = 0; k < w->actor_count; k++)
        free(w->actors[k].output);
    for (uint32_t k = 0; k < w->log_count; k++)
        free(w->logs[k]);
    JS_FreeValue(J, w->documents);
    JS_FreeValue(J, steps);
    JS_FreeValue(J, name);
    free(sname);
    return fail[0] != 0;
}

static int cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv) {
    const char *dir = NULL, *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc)
            only = argv[++i];
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else
            dir = argv[i];
    }
    if (!dir) {
        fprintf(stderr, "usage: conformance-c <dir> [--only name] [-v]\n");
        return 2;
    }
    JSRuntime *rt = JS_NewRuntime();
    J = JS_NewContext(rt);
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return 2;
    }
    char *files[64];
    int nf = 0;
    struct dirent *de;
    while ((de = readdir(d)) && nf < 64) {
        size_t l = strlen(de->d_name);
        /* protocol vectors are run by their own tests (test_address, ...) */
        if (!strcmp(de->d_name, "addresses.json") || !strcmp(de->d_name, "values.json") ||
            !strcmp(de->d_name, "span_ids.json") || !strcmp(de->d_name, "traceparent.json"))
            continue;
        if (l > 5 && !strcmp(de->d_name + l - 5, ".json")) files[nf++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(files, (size_t)nf, sizeof *files, cmp);
    int failed = 0;
    for (int i = 0; i < nf; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        FILE *f = fopen(path, "rb");
        char *text = NULL;
        long length = 0;
        if (f && !fseek(f, 0, SEEK_END) && (length = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET)) {
            text = (char *)malloc((size_t)length + 1);
            if (fread(text, 1, (size_t)length, f) != (size_t)length) length = 0;
            text[length] = 0;
        }
        if (f) fclose(f);
        JSValue sc = text ? JS_ParseJSON(J, text, (size_t)length, path) : JS_EXCEPTION;
        if (JS_IsException(sc)) {
            printf("FAIL c    %s: not JSON\n", files[i]);
            failed++;
        } else {
            JSValue nm = get(sc, "name");
            char *name = str_of(nm);
            if (!only || !strcmp(only, name)) failed += run_scenario(sc, files[i]);
            free(name);
            JS_FreeValue(J, nm);
        }
        JS_FreeValue(J, sc);
        free(text);
        free(files[i]);
    }
    JS_FreeContext(J);
    JS_FreeRuntime(rt);
    return failed ? 1 : 0;
}

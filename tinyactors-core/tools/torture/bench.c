/* torture-bench — runs torture-chamber scenarios against tinyactors systems.
 *
 * One thread drives several ta_systems (each with its own ECMAScript
 * datamodel runtime, like separate processes), routes SCXML-to-SCXML events
 * between them through a TA_IO_PROCESSOR_SCXML_REMOTE processor
 * ("tinyactors://<system>/#_scxml_<id>"), runs scenario scripts in a separate
 * quickjs context (prelude.js + the scenario's scripts; their only I/O is
 * reading files of the scenario directory), samples system statistics as a
 * time series, and streams it as JSON lines to a file and over WebSocket to
 * the dashboard in web/torture.
 *
 *   torture-bench [SCENARIO_DIR] [--scenarios DIR] [--duration S] [--interval MS]
 *                 [--scale F] [--out FILE] [--port N] [--host ADDR] [--web DIR]
 *                 [--headless] [--loop] [--seed N] [--quiet] [--param KEY=VALUE]...
 *
 * Hot reload: bench.reload(system, charts) compiles a new version of a system
 * into a new ta_system (the version) that shares the family's base URI, its
 * ECMAScript runtime and a session range of its own (first_serial = version
 * << 32, TA_IO_PROCESSOR_SCXML_SHARED); new actors start in the newest version,
 * remote sends find a session in whichever version holds it, and versions
 * without actors are freed. bench.migrate(system) moves running actors to the
 * newest version (ta_actor_export -> ta_blob_migrate -> ta_system_adopt).
 *
 * With the server (default), the home page lists the scenarios of --scenarios
 * (default: the scenario's parent directory, else tools/torture/scenarios) and
 * the dashboard can switch the running scenario.
 */
#define _POSIX_C_SOURCE 200809L
/* macOS hides what POSIX does not define (INADDR_LOOPBACK,
 * _SC_NPROCESSORS_ONLN, st_mtimespec) unless asked; Linux ignores this. */
#define _DARWIN_C_SOURCE
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "quickjs.h"
#include <tinyactors.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "net.h"
#include "postgres.h"

#ifndef BENCH_SOURCE_DIR
#define BENCH_SOURCE_DIR "."
#endif
#ifndef BENCH_WEB_DIR
#define BENCH_WEB_DIR "web/torture"
#endif

#define MAX_SYSTEMS 32
#define HISTORY     2400 /* samples kept for dashboards that connect late */
#define LOG_RING    200
#define BUDGET      4000

#define REMOTE_TYPE "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"
#define BENCH_TYPE  "bench"
/* The test runtime's system I/O processor (SCXML 1.0 §6.2.5: platforms MAY
 * add Event I/O Processors). The SCXML processor itself only reaches existing
 * sessions (Appendix C.1), so creating sessions from a chart is a host service:
 *   <send type="system" event="spawn" target="chart.scxml" id="..."> + params
 * starts a top-level session of that chart; when it finishes the sender gets
 * "done.spawn.<sendid>" ("done.spawn" without an id) with the donedata.
 *   <send type="system" event="terminate" target="#_scxml_<id>">
 * stops a session. Unknown charts/sessions raise error.communication. */
#define SYSTEM_TYPE  "http://tinyactors.dev/torture/#SystemEventProcessor"
#define SYSTEM_SHORT "system"
/* host I/O processors of every system (ta_host_config.io_processors; TA_EFFECT_SEND.io_processor):
 * SCXML sends to other systems come out of their router as TA_HOST_EFFECT_PEER */
#define IO_BENCH  0
#define IO_SYSTEM 1 /* and 2 (short form) */
#define IO_HOST   3 /* first scenario-defined host processor */
#define MAX_IO    16
#define ID_SHIFT  281474976710656.0 /* 2^48: script actor id = system * 2^48 + session */

/* ---- growable string ----------------------------------------------------------------- */

typedef struct string_builder {
    char *data;
    size_t length, capacity;
} string_builder;

static void sb_put(string_builder *b, const char *s, size_t n) {
    if (b->length + n + 1 > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 1024;
        while (capacity < b->length + n + 1)
            capacity *= 2;
        b->data = (char *)realloc(b->data, capacity);
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, s, n);
    b->length += n;
    b->data[b->length] = 0;
}

static void sb_printf(string_builder *b, const char *format, ...) {
    char temporary[2048];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(temporary, sizeof temporary, format, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof temporary) {
        sb_put(b, temporary, (size_t)n);
        return;
    }
    char *big = (char *)malloc((size_t)n + 1);
    va_start(ap, format);
    vsnprintf(big, (size_t)n + 1, format, ap);
    va_end(ap);
    sb_put(b, big, (size_t)n);
    free(big);
}

static void sb_json_str(string_builder *b, const char *s, size_t n) {
    sb_put(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            sb_put(b, e, 2);
        } else if (c < 0x20) {
            sb_printf(b, "\\u%04x", c);
        } else {
            sb_put(b, (const char *)&c, 1);
        }
    }
    sb_put(b, "\"", 1);
}

/* ---- systems --------------------------------------------------------------------------- */

typedef struct bench_image { /* a version's image (heap: the host holds &image), freed with it */
    void *bytes;
    ta_image image;
} bench_image;

/* A torture system: one ta_host (versions are its segments, sharing the
 * system's address and ECMAScript runtime). */
typedef struct bench_system {
    int live;
    char name[64];
    char base[TA_ADDRESS_SYSTEM_MAX + 16];
    ta_datamodel_ecmascript *ecmascript;
    ta_host *host;
    bench_image **images; /* by segment (NULL once freed) */
    uint32_t image_count, image_capacity;
    ta_io_processor_descriptor io_processors[MAX_IO];
    char io_names[MAX_IO][48];
    uint32_t io_processor_count;
    /* actors started by the system processor: session -> spawner (open addressing) */
    struct bench_spawn *spawns;
    uint32_t spawn_capacity, spawn_count;
    uint64_t spawned, spawn_done, spawn_failed;
    int busy;          /* not idle after its last run */
    uint64_t deadline; /* next timer (system ticks = µs), UINT64_MAX if none */
    uint64_t remote_out, remote_in, remote_fail, to_bench, logs, done, faults, host_sent, host_io;
    uint32_t peer_of[MAX_SYSTEMS]; /* peer handle of each other system in our router (0: none) */
    double busy_ms;
    size_t js_heap;
    /* previous sample, for rates */
    ta_system_stats previous;
    uint64_t previous_remote_out, previous_remote_in, previous_logs, previous_to_bench,
        previous_host_sent;
    double previous_busy;
    /* hibernation: blobs in an append-only file (unlinked at creation, so it
     * vanishes with the process); index: session -> blob (open addressing) */
    int hibernation_fd;
    uint64_t hibernation_end, hibernations_live;
    struct bench_hibernate *hibernations;
    uint32_t hibernation_capacity, hibernation_count;
    uint64_t hibernation_write_ns, hibernation_read_ns, hibernations_failed;
    /* versions (hot reload): the current sources, the capacities */
    uint32_t source_count;
    char **source_names, **source_texts;
    ta_system_config config;
    size_t memory_limit;
    /* migration jobs: their onDone callbacks; totals */
    struct bench_job *jobs;
    uint32_t job_count, job_capacity;
    uint64_t migrated, migrate_pending, migrate_failed, retired;
} bench_system;

typedef struct bench_job {
    uint32_t id;
    int wake;
    JSValue callbacks; /* array of functions */
} bench_job;

typedef struct bench_hibernate {
    uint64_t session; /* 0 = empty, UINT64_MAX = removed */
    uint64_t offset;
    uint32_t length;
} bench_hibernate;

static JSValue h_hibernate(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv);
static JSValue h_postgres_status(JSContext *context, JSValueConst this_value, int argc,
                                 JSValueConst *argv);
static JSValue h_postgres_query(JSContext *context, JSValueConst this_value, int argc,
                                JSValueConst *argv);
static JSValue h_postgres_load(JSContext *context, JSValueConst this_value, int argc,
                               JSValueConst *argv);
static JSValue h_postgres_stop_load(JSContext *context, JSValueConst this_value, int argc,
                                    JSValueConst *argv);
static JSValue h_postgres_start(JSContext *context, JSValueConst this_value, int argc,
                                JSValueConst *argv);
static JSValue h_postgres_restart(JSContext *context, JSValueConst this_value, int argc,
                                  JSValueConst *argv);
static JSValue h_postgres_use(JSContext *context, JSValueConst this_value, int argc,
                              JSValueConst *argv);
static void postgres_stop_loads(void);
static void postgres_close_connection(void);
static void postgres_scenario_reset(void);
static void postgres_status_json(string_builder *b, int with_type);

typedef struct bench_spawn {
    uint64_t session; /* the spawned actor; 0 = empty slot, UINT64_MAX = removed */
    uint64_t spawner; /* session that asked */
    char *sendid;     /* NULL when the send had no id */
} bench_spawn;

typedef struct bench_timer {
    int id;
    double due, period; /* loop-clock ms; period 0 = once */
    JSValue function;
} bench_timer;

typedef struct bench_state {
    char directory[1024]; /* the running scenario's directory */
    char catalog[1024];   /* directory of all scenarios (home page) */
    char current[128];    /* the running scenario's id (directory name) */
    char pending[128];    /* switch requested from the dashboard */
    int running, finished, failed;
    char *summary;       /* the finished scenario's summary line */
    double cli_duration; /* --duration (else the scenario's own) */
    double duration, interval, scale;
    uint64_t seed;
    int headless, loop, quiet, port;
    char host[64], web[1024];
    FILE *out;
    bench_net *net;
    bench_loop *event_loop;
    bench_system system[MAX_SYSTEMS];
    JSRuntime *runtime;
    JSContext *context;
    JSValue finish_result;
    int finishing;
    bench_timer *timers;
    int timer_count, timer_capacity, timer_sequence;
    double scenario_origin; /* loop-clock ms at scenario start */
    double now;             /* ms since start */
    double next_sample, last_sample;
    int paused, stopping, exit_code;
    char stop_message[512];
    /* custom metrics set by scripts */
    char metric_names[64][48];
    double metric_values[64];
    int metric_count;
    char *history[HISTORY];
    int history_head, history_length;
    char *logs[LOG_RING];
    int log_head, log_length;
    int logs_this_second;
    double log_window;
    uint64_t script_errors, samples;
    const char *params[32]; /* --param key=value */
    int param_count;
    size_t script_heap;
    double heap_next;
    double time_script, time_run, time_sample, previous_script, previous_run,
        previous_sample; /* where the loop's time goes (ms) */
} bench_state;

static bench_state B;

static double now_ms(void) {
    return bench_loop_now() - B.scenario_origin;
}

static void push_ring(char **ring, int capacity, int *head, int *length, char *s) {
    if (*length < capacity) {
        ring[(*head + *length) % capacity] = s;
        (*length)++;
    } else { /* overwrite the oldest */
        free(ring[*head]);
        ring[*head] = s;
        *head = (*head + 1) % capacity;
    }
}

static void emit(const char *line, size_t length, int keep) {
    if (B.out) {
        fwrite(line, 1, length, B.out);
        fputc('\n', B.out);
    }
    if (B.net) bench_net_broadcast(B.net, line, length);
    if (keep == 1) push_ring(B.history, HISTORY, &B.history_head, &B.history_length, strdup(line));
    if (keep == 2) push_ring(B.logs, LOG_RING, &B.log_head, &B.log_length, strdup(line));
}

static void bench_log(const char *level, const char *source, const char *message, size_t length) {
    if (!B.quiet || !strcmp(level, "error"))
        fprintf(stderr, "[%8.3f] %-5s %s: %.*s\n", B.now / 1000, level, source, (int)length,
                message);
    string_builder b = {0};
    sb_printf(&b, "{\"type\":\"log\",\"t\":%.3f,\"level\":\"%s\",\"source\":", B.now / 1000, level);
    sb_json_str(&b, source, strlen(source));
    sb_printf(&b, ",\"message\":");
    sb_json_str(&b, message, length > 2000 ? 2000 : length);
    sb_put(&b, "}", 1);
    emit(b.data, b.length, 2);
    free(b.data);
}

static int system_by_name(const char *name, size_t length) {
    for (int i = 0; i < MAX_SYSTEMS; i++)
        if (B.system[i].live && strlen(B.system[i].name) == length &&
            !memcmp(B.system[i].name, name, length))
            return i;
    return -1;
}

static void peers_disconnect(int si);
static void peers_connect(int si);
static void system_free(bench_system *system) {
    if (system->live) peers_disconnect((int)(system - B.system)); /* the others' routes to us */
    for (uint32_t i = 0; i < system->source_count; i++) {
        free(system->source_names[i]);
        free(system->source_texts[i]);
    }
    free(system->source_names);
    free(system->source_texts);
    if (system->hibernation_fd > 0) close(system->hibernation_fd);
    free(system->hibernations);
    for (uint32_t i = 0; i < system->spawn_capacity; i++)
        if (system->spawns[i].session && system->spawns[i].session != UINT64_MAX)
            free(system->spawns[i].sendid);
    free(system->spawns);
    for (uint32_t i = 0; i < system->job_count; i++)
        JS_FreeValue(B.context, system->jobs[i].callbacks);
    free(system->jobs);
    if (system->host)
        ta_host_free(system->host); /* before the runtime its datamodel instances live in */
    if (system->ecmascript) ta_datamodel_ecmascript_free(system->ecmascript);
    for (uint32_t i = 0; i < system->image_count; i++)
        if (system->images[i]) {
            free(system->images[i]->bytes);
            free(system->images[i]);
        }
    free(system->images);
    memset(system, 0, sizeof *system);
}

/* Compile sources into image bytes (malloc'd). NULL on failure (message set). */
static void *compile_image(const char *name, ta_source *source, uint32_t n, ta_image *image,
                           char *message, size_t capacity) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_compile_result *result = NULL;
    if (ta_compile_charts(&options, source, n, &result) != TA_OK) {
        const ta_diagnostic *diagnostic;
        uint32_t nd = ta_compile_diagnostics(result, &diagnostic);
        snprintf(message, capacity, "%s: compile failed", name);
        for (uint32_t i = 0; i < nd && strlen(message) < capacity - 200; i++)
            snprintf(message + strlen(message), capacity - strlen(message), "\n  %.*s:%u:%u: %.*s",
                     (int)diagnostic[i].file.length,
                     diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "",
                     diagnostic[i].line, diagnostic[i].column, (int)diagnostic[i].message.length,
                     diagnostic[i].message.pointer);
        ta_compile_free(result);
        return NULL;
    }
    ta_string_view bytes = ta_compile_image(result);
    void *b = malloc(bytes.length + 8);
    memcpy(b, bytes.pointer, bytes.length);
    ta_compile_free(result);
    if (ta_image_open(b, bytes.length, image) != TA_OK) {
        snprintf(message, capacity, "%s: invalid image", name);
        free(b);
        return NULL;
    }
    return b;
}

/* A new version of system s: a segment of its host running these sources.
 * Returns the segment (= version - 1) or -1 (message set). */
static int system_add_version(bench_system *system, ta_source *source, uint32_t n, char *message,
                              size_t capacity) {
    bench_image *image = (bench_image *)calloc(1, sizeof *image);
    image->bytes = compile_image(system->name, source, n, &image->image, message, capacity);
    uint32_t segment = 0;
    ta_status status =
        image->bytes ? ta_host_add_image(system->host, &image->image, &segment) : TA_ERROR_INVALID;
    if (status != TA_OK) {
        if (image->bytes)
            snprintf(message, capacity, "%s: cannot start the version (%d)", system->name, status);
        free(image->bytes);
        free(image);
        return -1;
    }
    if (segment >= system->image_capacity) {
        uint32_t c = system->image_capacity ? system->image_capacity * 2 : 8;
        while (c <= segment)
            c *= 2;
        system->images = (bench_image **)realloc(system->images, sizeof(bench_image *) * c);
        memset(system->images + system->image_capacity, 0,
               sizeof(bench_image *) * (c - system->image_capacity));
        system->image_capacity = c;
    }
    system->images[segment] = image;
    if (segment >= system->image_count) system->image_count = segment + 1;
    system->busy = 1;
    return (int)segment;
}

/* Start a system (a ta_host with its first version). Returns index or -1 (message set). */
static int system_create(const char *name, ta_source *source, uint32_t n,
                         const ta_system_config *config_in, size_t memory_limit,
                         const char *const *host_io, uint32_t host_io_count, char *message,
                         size_t capacity) {
    if (!ta_address_system_valid((ta_string_view){name, (uint32_t)strlen(name)})) {
        snprintf(message, capacity, "\"%s\" is not a valid system id (lowercase a-z, 0-9, -, dots)",
                 name);
        return -1;
    }
    if (system_by_name(name, strlen(name)) >= 0) {
        snprintf(message, capacity, "system \"%s\" exists", name);
        return -1;
    }
    int index = -1;
    for (int i = 0; i < MAX_SYSTEMS && index < 0; i++)
        if (!B.system[i].live) index = i;
    if (index < 0) {
        snprintf(message, capacity, "too many systems");
        return -1;
    }
    bench_system *system = &B.system[index];
    memset(system, 0, sizeof *system);
    snprintf(system->name, sizeof system->name, "%s", name);
    snprintf(system->base, sizeof system->base, "tinyactors://%s/", name);
    ta_datamodel_ecmascript_config ec;
    memset(&ec, 0, sizeof ec);
    ec.size = sizeof ec;
    ec.memory_limit = memory_limit;
    system->ecmascript = ta_datamodel_ecmascript_new(&ec);
    ta_datamodel_binding datamodels[2] = {ta_datamodel_ecmascript_binding(system->ecmascript),
                                          {ta_datamodel_null(), NULL}};
    system->io_processors[IO_BENCH].type =
        (ta_string_view){BENCH_TYPE, (uint32_t)strlen(BENCH_TYPE)};
    system->io_processors[IO_BENCH].location = (ta_string_view){"bench:", 6};
    system->io_processors[IO_SYSTEM].type =
        (ta_string_view){SYSTEM_TYPE, (uint32_t)strlen(SYSTEM_TYPE)};
    system->io_processors[IO_SYSTEM].location = (ta_string_view){"#_system", 8};
    system->io_processors[IO_SYSTEM + 1].type =
        (ta_string_view){SYSTEM_SHORT, (uint32_t)strlen(SYSTEM_SHORT)};
    system->io_processors[IO_SYSTEM + 1].location = (ta_string_view){"#_system", 8};
    system->io_processor_count = IO_HOST;
    for (uint32_t i = 0; i < host_io_count && system->io_processor_count < MAX_IO - 1;
         i++, system->io_processor_count++) {
        snprintf(system->io_names[system->io_processor_count], sizeof system->io_names[0], "%s",
                 host_io[i]);
        system->io_processors[system->io_processor_count].type =
            (ta_string_view){system->io_names[system->io_processor_count],
                             (uint32_t)strlen(system->io_names[system->io_processor_count])};
        system->io_processors[system->io_processor_count].location = (ta_string_view){"bench:", 6};
    }
    system->config = *config_in;
    system->config.size = sizeof system->config;
    system->config.ticks_per_second = 1000000; /* µs */
    system->memory_limit = memory_limit;
    ta_host_config host_config;
    memset(&host_config, 0, sizeof host_config);
    host_config.size = sizeof host_config;
    host_config.system_id = (ta_string_view){system->base + 13, (uint32_t)strlen(system->base) -
                                                                    14}; /* tinyactors://<id>/ */
    host_config.system = system->config;
    host_config.datamodels = datamodels;
    host_config.datamodel_count = 2;
    host_config.io_processors = system->io_processors;
    host_config.io_processor_count = system->io_processor_count;
    host_config.allocator = ta_runtime_malloc();
    if (ta_host_new(&host_config, &system->host) != TA_OK) {
        snprintf(message, capacity, "%s: cannot create the system", name);
        ta_datamodel_ecmascript_free(system->ecmascript);
        memset(system, 0, sizeof *system);
        return -1;
    }
    ta_host_advance_time(system->host, (ta_time)(B.now * 1000));
    if (system_add_version(system, source, n, message, capacity) < 0) {
        ta_host_free(system->host);
        ta_datamodel_ecmascript_free(system->ecmascript);
        free(system->images);
        memset(system, 0, sizeof *system);
        return -1;
    }
    system->live = 1;
    system->deadline = UINT64_MAX;
    peers_connect(index);
    system->busy = 1;
    system->source_count = n; /* reload() replaces some of them */
    system->source_names = (char **)calloc(n ? n : 1, sizeof(char *));
    system->source_texts = (char **)calloc(n ? n : 1, sizeof(char *));
    for (uint32_t i = 0; i < n; i++) {
        system->source_names[i] = strndup(source[i].name.pointer, source[i].name.length);
        system->source_texts[i] = strndup(source[i].text.pointer, source[i].text.length);
    }
    return index;
}

static ta_datamodel_ecmascript *datamodel_of(bench_system *system) {
    return system->ecmascript;
}

/* ---- script host ------------------------------------------------------------------------ */

static double actor_id(int system, uint64_t session) {
    return (double)system * ID_SHIFT + (double)session;
}

static int split_id(JSContext *context, JSValueConst v, bench_system **system, uint64_t *session) {
    double d;
    if (JS_ToFloat64(context, &d, v)) return 0;
    int si = (int)(d / ID_SHIFT);
    if (si < 0 || si >= MAX_SYSTEMS || !B.system[si].live) return 0;
    *system = &B.system[si];
    *session = (uint64_t)(d - (double)si * ID_SHIFT);
    return 1;
}

static void script_exception(const char *where) {
    JSValue e = JS_GetException(B.context);
    JSValue stack = JS_GetPropertyStr(B.context, e, "stack");
    const char *m = JS_ToCString(B.context, e);
    const char *stack_text = JS_IsUndefined(stack) ? NULL : JS_ToCString(B.context, stack);
    char buffer[4096];
    snprintf(buffer, sizeof buffer, "%s%s%s", m ? m : "exception", stack_text ? "\n" : "",
             stack_text ? stack_text : "");
    bench_log("error", where, buffer, strlen(buffer));
    if (m) JS_FreeCString(B.context, m);
    if (stack_text) JS_FreeCString(B.context, stack_text);
    JS_FreeValue(B.context, stack);
    JS_FreeValue(B.context, e);
    B.script_errors++;
}

static int safe_path(const char *relative_path) {
    return relative_path[0] && relative_path[0] != '/' && !strstr(relative_path, "..");
}

static char *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = (char *)malloc((size_t)(n > 0 ? n : 0) + 1);
    size_t got = p ? fread(p, 1, (size_t)(n > 0 ? n : 0), f) : 0;
    fclose(f);
    if (!p) return NULL;
    p[got] = 0;
    if (length) *length = got;
    return p;
}

/* readText(path): a file of the scenario directory */
static JSValue h_read_text(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    const char *relative_path = argc ? JS_ToCString(context, argv[0]) : NULL;
    if (!relative_path) return JS_EXCEPTION;
    if (!safe_path(relative_path)) {
        JS_FreeCString(context, relative_path);
        return JS_ThrowTypeError(context, "readText: path must stay inside the scenario");
    }
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", B.directory, relative_path);
    size_t length;
    char *text = read_file(path, &length);
    JSValue r = text ? JS_NewStringLen(context, text, length)
                     : JS_ThrowTypeError(context, "readText: cannot read %s", relative_path);
    free(text);
    JS_FreeCString(context, relative_path);
    return r;
}

/* evalFile(path): run a scenario script (block-scoped declarations) */
static JSValue h_eval_file(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    const char *relative_path = argc ? JS_ToCString(context, argv[0]) : NULL;
    if (!relative_path) return JS_EXCEPTION;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", B.directory, relative_path);
    size_t length;
    char *text = safe_path(relative_path) ? read_file(path, &length) : NULL;
    JSValue r;
    if (!text)
        r = JS_ThrowTypeError(context, "evalFile: cannot read %s", relative_path);
    else { /* each script in its own block scope (same line numbers); share via globalThis */
        char *wrapped = (char *)malloc(length + 4);
        wrapped[0] = '{';
        memcpy(wrapped + 1, text, length);
        memcpy(wrapped + 1 + length, "\n}", 3);
        r = JS_Eval(context, wrapped, length + 3, relative_path, JS_EVAL_TYPE_GLOBAL);
        free(wrapped);
    }
    free(text);
    JS_FreeCString(context, relative_path);
    return r;
}

static uint32_t config_u32(JSContext *context, JSValueConst o, const char *k) {
    JSValue v = JS_GetPropertyStr(context, o, k);
    double d = 0;
    if (!JS_IsUndefined(v)) JS_ToFloat64(context, &d, v);
    JS_FreeValue(context, v);
    return d > 0 ? (uint32_t)d : 0;
}

/* createSystem(name, [{name, text}], config) -> index */
static JSValue h_create_system(JSContext *context, JSValueConst this_value, int argc,
                               JSValueConst *argv) {
    (void)this_value;
    if (argc < 2 || !JS_IsArray(argv[1]))
        return JS_ThrowTypeError(context, "createSystem(name, charts, config)");
    const char *name = JS_ToCString(context, argv[0]);
    int64_t n = 0;
    JS_GetLength(context, argv[1], &n);
    ta_source *source = (ta_source *)calloc((size_t)(n ? n : 1), sizeof *source);
    const char **strs = (const char **)calloc((size_t)(2 * (n ? n : 1)), sizeof(char *));
    for (int64_t i = 0; i < n; i++) {
        JSValue o = JS_GetPropertyUint32(context, argv[1], (uint32_t)i);
        JSValue nm = JS_GetPropertyStr(context, o, "name"),
                tx = JS_GetPropertyStr(context, o, "text");
        size_t nl = 0, tl = 0;
        strs[2 * i] = JS_ToCStringLen(context, &nl, nm);
        strs[2 * i + 1] = JS_ToCStringLen(context, &tl, tx);
        source[i].name = (ta_string_view){strs[2 * i] ? strs[2 * i] : "", (uint32_t)nl};
        source[i].text = (ta_string_view){strs[2 * i + 1] ? strs[2 * i + 1] : "", (uint32_t)tl};
        JS_FreeValue(context, nm);
        JS_FreeValue(context, tx);
        JS_FreeValue(context, o);
    }
    ta_system_config config;
    memset(&config, 0, sizeof config);
    size_t memory_limit = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        config.max_actors = config_u32(context, argv[2], "maxActors");
        config.max_events = config_u32(context, argv[2], "maxEvents");
        config.max_timers = config_u32(context, argv[2], "maxTimers");
        config.workspaces = config_u32(context, argv[2], "workspaces");
        config.quantum = config_u32(context, argv[2], "quantum");
        config.outbox = config_u32(context, argv[2], "outbox");
        memory_limit = (size_t)config_u32(context, argv[2], "jsMemoryMB") << 20;
    }
    if (!config.outbox) config.outbox = 4096;
    /* scenario-defined host I/O processors (handled by scripts: bench.ioprocessor) */
    const char *io[MAX_IO];
    uint32_t io_count = 0;
    JSValue ios = argc > 2 && JS_IsObject(argv[2])
                      ? JS_GetPropertyStr(context, argv[2], "ioprocessors")
                      : JS_UNDEFINED;
    if (JS_IsArray(ios)) {
        int64_t k = 0;
        JS_GetLength(context, ios, &k);
        for (int64_t i = 0; i < k && io_count < MAX_IO - IO_HOST; i++) {
            JSValue v = JS_GetPropertyUint32(context, ios, (uint32_t)i);
            io[io_count] = JS_ToCString(context, v);
            if (io[io_count]) io_count++;
            JS_FreeValue(context, v);
        }
    }
    JS_FreeValue(context, ios);
    char message[4096];
    int index = name ? system_create(name, source, (uint32_t)n, &config, memory_limit, io, io_count,
                                     message, sizeof message)
                     : -1;
    for (uint32_t i = 0; i < io_count; i++)
        JS_FreeCString(context, io[i]);
    for (int64_t i = 0; i < 2 * n; i++)
        if (strs[i]) JS_FreeCString(context, strs[i]);
    free(strs);
    free(source);
    if (name) JS_FreeCString(context, name);
    if (index < 0) return JS_ThrowTypeError(context, "%s", message);
    return JS_NewInt32(context, index);
}

static JSValue h_destroy_system(JSContext *context, JSValueConst this_value, int argc,
                                JSValueConst *argv) {
    (void)this_value;
    int32_t i = -1;
    if (argc) JS_ToInt32(context, &i, argv[0]);
    if (i < 0 || i >= MAX_SYSTEMS || !B.system[i].live) return JS_FALSE;
    system_free(&B.system[i]);
    return JS_TRUE;
}

static uint32_t find_chart(const ta_image *image, const char *name, size_t length) {
    uint32_t c = ta_image_chart_by_name(image, (ta_string_view){name, (uint32_t)length});
    if (c != TA_NIL) return c;
    ta_image_info image_info;
    ta_image_get_info(image, &image_info);
    for (uint32_t i = 0; i < image_info.chart_count; i++) {
        ta_string_view s = ta_image_chart_source(image, i);
        if (s.length == length && !memcmp(s.pointer, name, length)) return i;
    }
    return TA_NIL;
}

/* The chart called `name` in the current version: <scxml name>, else a source
 * name (e.g. "delivery.scxml"). */
static int system_chart(bench_system *system, const char *name, size_t length, uint32_t *segment,
                        uint32_t *chart) {
    if (ta_host_chart(system->host, (ta_string_view){name, (uint32_t)length}, segment, chart) ==
        TA_OK)
        return 1;
    for (uint32_t i = system->image_count; i-- > 0;) {
        if (!system->images[i]) continue;
        uint32_t c = find_chart(&system->images[i]->image, name, length);
        if (c == TA_NIL) continue;
        /* the current version of that chart's name, if it has one */
        if (ta_host_chart(system->host, ta_image_chart_name(&system->images[i]->image, c), segment,
                          chart) != TA_OK) {
            *segment = i;
            *chart = c;
        }
        return 1;
    }
    return 0;
}

static ta_payload payload_arg(JSContext *context, bench_system *system, JSValueConst v) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return 0;
    size_t length;
    const char *json = JS_ToCStringLen(context, &length, v); /* already JSON text */
    if (!json) return 0;
    ta_payload payload = ta_datamodel_ecmascript_payload_from_json(
        datamodel_of(system), (ta_string_view){json, (uint32_t)length});
    JS_FreeCString(context, json);
    return payload;
}

/* spawn(system, chart, name, json?) -> actor id (in the current version of the chart) */
static JSValue h_spawn(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    int32_t si = -1;
    if (argc < 2) return JS_ThrowTypeError(context, "spawn(system, chart, name?, json?)");
    JS_ToInt32(context, &si, argv[0]);
    if (si < 0 || si >= MAX_SYSTEMS || !B.system[si].live)
        return JS_ThrowTypeError(context, "spawn: no such system");
    bench_system *system = &B.system[si];
    size_t cl = 0, nl = 0;
    const char *chart = JS_ToCStringLen(context, &cl, argv[1]);
    const char *name =
        argc > 2 && JS_IsString(argv[2]) ? JS_ToCStringLen(context, &nl, argv[2]) : NULL;
    uint32_t segment = 0, c = 0;
    JSValue r;
    if (!chart || !system_chart(system, chart, cl, &segment, &c)) {
        r = JS_ThrowTypeError(context, "spawn: no chart \"%s\" in system %s", chart ? chart : "?",
                              system->name);
    } else {
        ta_payload payload = argc > 3 ? payload_arg(context, system, argv[3]) : 0;
        uint64_t session = 0;
        ta_status status =
            ta_host_spawn(system->host, segment, c,
                          (ta_string_view){name ? name : "", (uint32_t)nl}, payload, &session);
        if (status != TA_OK && payload)
            ta_datamodel_ecmascript_vtable()->release(system->ecmascript, payload);
        r = status == TA_OK ? JS_NewFloat64(context, actor_id(si, session))
                            : JS_ThrowTypeError(context, "spawn failed (%d) in system %s", status,
                                                system->name);
        system->busy = 1;
    }
    if (chart) JS_FreeCString(context, chart);
    if (name) JS_FreeCString(context, name);
    return r;
}

static ta_status send_one(bench_system *system, uint64_t session, const char *name, size_t nl,
                          ta_payload payload) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)nl};
    event.data = payload;
    ta_status status = ta_host_send(system->host, session, &event);
    if (status != TA_OK && payload)
        ta_datamodel_ecmascript_vtable()->release(system->ecmascript, payload);
    if (status == TA_OK) system->host_sent++;
    system->busy = 1;
    return status;
}

/* send(id, name, json?) -> bool */
static JSValue h_send(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    bench_system *system;
    uint64_t h;
    if (argc < 2 || !split_id(context, argv[0], &system, &h)) return JS_FALSE;
    size_t nl;
    const char *name = JS_ToCStringLen(context, &nl, argv[1]);
    if (!name) return JS_EXCEPTION;
    ta_payload payload = argc > 2 ? payload_arg(context, system, argv[2]) : 0;
    ta_status status = send_one(system, h, name, nl, payload);
    JS_FreeCString(context, name);
    return JS_NewBool(context, status == TA_OK);
}

/* sendMany(ids, name, json?) -> count (one JSON payload parsed per target) */
static JSValue h_send_many(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    if (argc < 2 || !JS_IsArray(argv[0])) return JS_NewInt32(context, 0);
    size_t nl, jl = 0;
    const char *name = JS_ToCStringLen(context, &nl, argv[1]);
    const char *json =
        argc > 2 && !JS_IsUndefined(argv[2]) ? JS_ToCStringLen(context, &jl, argv[2]) : NULL;
    int64_t n = 0;
    JS_GetLength(context, argv[0], &n);
    int ok = 0;
    for (int64_t i = 0; name && i < n; i++) {
        JSValue v = JS_GetPropertyUint32(context, argv[0], (uint32_t)i);
        bench_system *system;
        uint64_t h;
        if (split_id(context, v, &system, &h)) {
            ta_payload payload = json
                                     ? ta_datamodel_ecmascript_payload_from_json(
                                           system->ecmascript, (ta_string_view){json, (uint32_t)jl})
                                     : 0;
            ok += send_one(system, h, name, nl, payload) == TA_OK;
        }
        JS_FreeValue(context, v);
    }
    if (name) JS_FreeCString(context, name);
    if (json) JS_FreeCString(context, json);
    return JS_NewInt32(context, ok);
}

static void communication_error_to(bench_system *system, uint64_t session, const char *sendid) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
    if (sendid) event.sendid = (ta_string_view){sendid, (uint32_t)strlen(sendid)};
    ta_host_send(system->host, session, &event);
    system->busy = 1;
}

/* failSend(id, sendid): error.communication for a send the host could not carry */
static JSValue h_fail_send(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    bench_system *system;
    uint64_t h;
    if (argc < 1 || !split_id(context, argv[0], &system, &h)) return JS_FALSE;
    const char *sendid = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(context, argv[1]) : NULL;
    communication_error_to(system, h, sendid);
    if (sendid) JS_FreeCString(context, sendid);
    return JS_TRUE;
}

static JSValue h_terminate(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    bench_system *system;
    uint64_t h;
    if (argc < 1 || !split_id(context, argv[0], &system, &h)) return JS_FALSE;
    system->busy = 1;
    return JS_NewBool(context, ta_host_terminate(system->host, h) == TA_OK);
}

/* address(id) -> "tinyactors://<system>/#_scxml_<session>" (SCXML target, every version) */
static JSValue h_address(JSContext *context, JSValueConst this_value, int argc,
                         JSValueConst *argv) {
    (void)this_value;
    bench_system *system;
    uint64_t h;
    if (argc < 1 || !split_id(context, argv[0], &system, &h)) return JS_UNDEFINED;
    if (ta_host_locate(system->host, h, NULL, NULL) != TA_OK) return JS_UNDEFINED;
    char buffer[TA_ADDRESS_SESSION_MAX];
    uint32_t n =
        ta_address_format((ta_string_view){system->base + 13, (uint32_t)strlen(system->base) - 14},
                          h, buffer, sizeof buffer);
    return JS_NewStringLen(context, buffer, n);
}

static JSValue h_alive(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    bench_system *system;
    uint64_t h;
    return JS_NewBool(context, argc && split_id(context, argv[0], &system, &h) &&
                                   ta_host_locate(system->host, h, NULL, NULL) == TA_OK);
}

static JSValue h_now(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(context, now_ms()); /* live clock: latency measurements */
}

static JSValue h_set_timer(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    double delay = 0, period = 0;
    if (argc < 3 || !JS_IsFunction(context, argv[2]))
        return JS_ThrowTypeError(context, "setTimer(delay, period, fn)");
    JS_ToFloat64(context, &delay, argv[0]);
    JS_ToFloat64(context, &period, argv[1]);
    if (B.timer_count == B.timer_capacity) {
        B.timer_capacity = B.timer_capacity ? B.timer_capacity * 2 : 32;
        B.timers = (bench_timer *)realloc(B.timers, sizeof(bench_timer) * (size_t)B.timer_capacity);
    }
    bench_timer *timer = &B.timers[B.timer_count++];
    timer->id = ++B.timer_sequence;
    timer->due = bench_loop_now() + (delay > 0 ? delay : 0);
    timer->period = period > 0 ? period : 0;
    timer->function = JS_DupValue(context, argv[2]);
    return JS_NewInt32(context, timer->id);
}

static JSValue h_clear_timer(JSContext *context, JSValueConst this_value, int argc,
                             JSValueConst *argv) {
    (void)this_value;
    int32_t id = 0;
    if (argc) JS_ToInt32(context, &id, argv[0]);
    for (int i = 0; i < B.timer_count; i++)
        if (B.timers[i].id == id) {
            JS_FreeValue(context, B.timers[i].function);
            B.timers[i] = B.timers[--B.timer_count];
            return JS_TRUE;
        }
    return JS_FALSE;
}

static JSValue h_log(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    const char *level = argc > 0 ? JS_ToCString(context, argv[0]) : NULL;
    size_t length = 0;
    const char *message = argc > 1 ? JS_ToCStringLen(context, &length, argv[1]) : NULL;
    bench_log(level ? level : "info", "script", message ? message : "", length);
    if (level && !strcmp(level, "error")) B.script_errors++;
    if (level) JS_FreeCString(context, level);
    if (message) JS_FreeCString(context, message);
    return JS_UNDEFINED;
}

static JSValue h_metric(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    if (argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(context, argv[0]);
    double v = 0;
    JS_ToFloat64(context, &v, argv[1]);
    if (!name) return JS_EXCEPTION;
    int i = 0;
    while (i < B.metric_count && strcmp(B.metric_names[i], name))
        i++;
    if (i == B.metric_count && B.metric_count < 64) {
        snprintf(B.metric_names[i], sizeof B.metric_names[i], "%s", name);
        B.metric_count++;
    }
    if (i < B.metric_count) B.metric_values[i] = v;
    JS_FreeCString(context, name);
    return JS_UNDEFINED;
}

/* Totals of a system's live versions (segments). */
static void system_stats(bench_system *system_state, ta_system_stats *sum) {
    memset(sum, 0, sizeof *sum);
    uint32_t nseg = ta_host_segment_count(system_state->host);
    for (uint32_t segment = 0; segment < nseg; segment++) {
        ta_system *system = ta_host_system(system_state->host, segment);
        if (!system) continue;
        ta_system_stats stats;
        stats.size = sizeof stats;
        ta_system_stats_get(system, &stats);
        sum->actors += stats.actors;
        sum->actors_capacity += stats.actors_capacity;
        sum->runnable += stats.runnable;
        sum->events += stats.events;
        sum->events_capacity += stats.events_capacity;
        sum->timers += stats.timers;
        sum->timers_capacity += stats.timers_capacity;
        sum->workspaces += stats.workspaces;
        sum->workspaces_capacity += stats.workspaces_capacity;
        sum->requests += stats.requests;
        sum->requests_capacity += stats.requests_capacity;
        sum->slab_pages += stats.slab_pages;
        sum->slab_pages_capacity += stats.slab_pages_capacity;
        sum->slab_page_bytes = stats.slab_page_bytes;
        sum->region_bytes += stats.region_bytes;
        sum->delivered += stats.delivered;
        sum->fuel += stats.fuel;
        sum->turns += stats.turns;
        sum->timers_fired += stats.timers_fired;
        sum->spawned += stats.spawned - ((uint64_t)segment << 32); /* sessions of this version */
        sum->hibernated += stats.hibernated;
        sum->wakes_pending += stats.wakes_pending;
        sum->hibernations += stats.hibernations;
        sum->wakes += stats.wakes;
    }
    /* totals of retired versions stay counted */
    if (sum->delivered < system_state->previous.delivered)
        sum->delivered = system_state->previous.delivered;
    if (sum->fuel < system_state->previous.fuel) sum->fuel = system_state->previous.fuel;
    if (sum->turns < system_state->previous.turns) sum->turns = system_state->previous.turns;
    if (sum->timers_fired < system_state->previous.timers_fired)
        sum->timers_fired = system_state->previous.timers_fired;
}

static JSValue stats_object(JSContext *context, bench_system *system) {
    ta_system_stats stats;
    system_stats(system, &stats);
    JSValue o = JS_NewObject(context);
#define F(k, v) JS_SetPropertyStr(context, o, k, JS_NewFloat64(context, (double)(v)))
    F("actors", stats.actors);
    F("actorsCap", stats.actors_capacity);
    F("runnable", stats.runnable);
    F("events", stats.events);
    F("eventsCap", stats.events_capacity);
    F("timers", stats.timers);
    F("timersCap", stats.timers_capacity);
    F("delivered", stats.delivered);
    F("fuel", stats.fuel);
    F("turns", stats.turns);
    F("timersFired", stats.timers_fired);
    F("spawned", stats.spawned);
    F("regionBytes", stats.region_bytes);
    F("remoteOut", system->remote_out);
    F("remoteIn", system->remote_in);
    F("remoteFailed", system->remote_fail);
    F("toBench", system->to_bench);
    F("logs", system->logs);
    F("done", system->done);
    F("faults", system->faults);
    F("jsHeap", system->js_heap);
    F("systemSpawned", system->spawned); /* by the system I/O processor */
    F("systemSpawnDone", system->spawn_done);
    F("systemSpawnFailed", system->spawn_failed);
    F("hostIO", system->host_io);
    F("hostSent", system->host_sent);
    F("hibernated", stats.hibernated);
    F("wakesPending", stats.wakes_pending);
    F("hibernations", stats.hibernations);
    F("wakes", stats.wakes);
    F("storeBytes", system->hibernation_end);
    F("storeLiveBytes", system->hibernations_live);
    F("storeWriteMs", (double)system->hibernation_write_ns / 1e6);
    F("storeReadMs", (double)system->hibernation_read_ns / 1e6);
    F("versions", ta_host_segment_count(system->host));
    F("migrated", system->migrated);
    F("migratePending", system->migrate_pending);
    F("migrateFailed", system->migrate_failed);
    F("versionsRetired", system->retired);
#undef F
    return o;
}

static JSValue h_stats(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    int32_t i = -1;
    if (argc) JS_ToInt32(context, &i, argv[0]);
    if (i < 0 || i >= MAX_SYSTEMS || !B.system[i].live) return JS_UNDEFINED;
    return stats_object(context, &B.system[i]);
}

/* ---- versions (hot reload, live migration: the host layer's) ---- */

/* reload(system, [{name, text}]) -> version: the system's sources with these
 * charts replaced (added if new), compiled into a new version */
static JSValue h_reload(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    int32_t si = -1;
    if (argc) JS_ToInt32(context, &si, argv[0]);
    if (si < 0 || si >= MAX_SYSTEMS || !B.system[si].live)
        return JS_ThrowTypeError(context, "reload: no such system");
    bench_system *system = &B.system[si];
    uint32_t n = system->source_count, capacity = n + 16;
    ta_source *source = (ta_source *)calloc(capacity, sizeof *source);
    const char **owned = (const char **)calloc(2 * capacity, sizeof(char *));
    for (uint32_t i = 0; i < n; i++) {
        source[i].name =
            (ta_string_view){system->source_names[i], (uint32_t)strlen(system->source_names[i])};
        source[i].text =
            (ta_string_view){system->source_texts[i], (uint32_t)strlen(system->source_texts[i])};
    }
    int64_t k = 0;
    if (argc > 1 && JS_IsArray(argv[1])) JS_GetLength(context, argv[1], &k);
    for (int64_t j = 0; j < k && n < capacity; j++) {
        JSValue o = JS_GetPropertyUint32(context, argv[1], (uint32_t)j);
        JSValue nm = JS_GetPropertyStr(context, o, "name"),
                tx = JS_GetPropertyStr(context, o, "text");
        size_t nl = 0, xl = 0;
        const char *name = JS_ToCStringLen(context, &nl, nm),
                   *text = JS_ToCStringLen(context, &xl, tx);
        owned[2 * j] = name;
        owned[2 * j + 1] = text;
        if (name && text) {
            uint32_t i = 0;
            while (i < n &&
                   (source[i].name.length != nl || memcmp(source[i].name.pointer, name, nl)))
                i++;
            if (i == n) n++;
            source[i].name = (ta_string_view){name, (uint32_t)nl};
            source[i].text = (ta_string_view){text, (uint32_t)xl};
        }
        JS_FreeValue(context, nm);
        JS_FreeValue(context, tx);
        JS_FreeValue(context, o);
    }
    char message[4096];
    int segment = system_add_version(system, source, n, message, sizeof message);
    if (segment >= 0) { /* the system remembers the new sources */
        char **nn = (char **)calloc(n ? n : 1, sizeof(char *)),
             **nt = (char **)calloc(n ? n : 1, sizeof(char *));
        for (uint32_t i = 0; i < n; i++) {
            nn[i] = strndup(source[i].name.pointer, source[i].name.length);
            nt[i] = strndup(source[i].text.pointer, source[i].text.length);
        }
        for (uint32_t i = 0; i < system->source_count; i++) {
            free(system->source_names[i]);
            free(system->source_texts[i]);
        }
        free(system->source_names);
        free(system->source_texts);
        system->source_names = nn;
        system->source_texts = nt;
        system->source_count = n;
    }
    for (int64_t j = 0; j < 2 * k; j++)
        if (owned[j]) JS_FreeCString(context, owned[j]);
    free(owned);
    free(source);
    return segment >= 0 ? JS_NewInt32(context, segment + 1)
                        : JS_ThrowTypeError(context, "%s", message);
}

static bench_job *system_job(bench_system *system, uint32_t id) {
    for (uint32_t i = 0; i < system->job_count; i++)
        if (system->jobs[i].id == id) return &system->jobs[i];
    return NULL;
}

static JSValue job_status(JSContext *context, bench_system *system) {
    JSValue o = JS_NewObject(context);
    JS_SetPropertyStr(context, o, "active", JS_NewBool(context, system->job_count > 0));
    JS_SetPropertyStr(context, o, "migrated", JS_NewFloat64(context, (double)system->migrated));
    JS_SetPropertyStr(context, o, "failed", JS_NewFloat64(context, (double)system->migrate_failed));
    JS_SetPropertyStr(context, o, "pending",
                      JS_NewFloat64(context, (double)system->migrate_pending));
    return o;
}

/* migrate(system, { wake, timeoutMs, onDone }) -> status: a ta_host migration
 * job, advanced inside ta_host_run in bounded steps */
static JSValue h_migrate(JSContext *context, JSValueConst this_value, int argc,
                         JSValueConst *argv) {
    (void)this_value;
    int32_t si = -1;
    if (argc) JS_ToInt32(context, &si, argv[0]);
    if (si < 0 || si >= MAX_SYSTEMS || !B.system[si].live)
        return JS_ThrowTypeError(context, "migrate: no such system");
    bench_system *system = &B.system[si];
    JSValueConst opt = argc > 1 ? argv[1] : JS_UNDEFINED;
    int wake = 0;
    double timeout_ms = 0;
    JSValue callback = JS_UNDEFINED;
    if (JS_IsObject(opt)) {
        JSValue v = JS_GetPropertyStr(context, opt, "wake");
        wake = JS_ToBool(context, v);
        JS_FreeValue(context, v);
        v = JS_GetPropertyStr(context, opt, "timeoutMs");
        if (!JS_IsUndefined(v)) JS_ToFloat64(context, &timeout_ms, v);
        JS_FreeValue(context, v);
        callback = JS_GetPropertyStr(context, opt, "onDone");
    }
    uint32_t id = 0;
    ta_status status =
        ta_host_migrate(system->host, (ta_string_view){0, 0}, 0, wake ? TA_HOST_MIGRATE_WAKE : 0,
                        timeout_ms > 0 ? (ta_time)(timeout_ms * 1000) : 0, &id);
    if (status != TA_OK) {
        JS_FreeValue(context, callback);
        return JS_ThrowTypeError(context, "migrate failed (%d)", status);
    }
    if (system->job_count == system->job_capacity) {
        system->job_capacity = system->job_capacity ? system->job_capacity * 2 : 4;
        system->jobs = (bench_job *)realloc(system->jobs, sizeof(bench_job) * system->job_capacity);
    }
    bench_job *job = &system->jobs[system->job_count++];
    job->id = id;
    job->wake = wake;
    job->callbacks = JS_NewArray(context);
    if (JS_IsFunction(context, callback))
        JS_SetPropertyUint32(context, job->callbacks, 0, callback);
    else
        JS_FreeValue(context, callback);
    system->busy = 1;
    return job_status(context, system);
}

/* a job ended (TA_HOST_EFFECT_JOB_DONE): totals, onDone callbacks */
static void job_done(bench_system *system, const ta_host_effect *effect) {
    system->migrated += effect->migrated;
    system->migrate_failed += effect->failed;
    system->migrate_pending = effect->pending;
    bench_job *job = system_job(system, effect->job);
    if (!job) return;
    JSValue stack_text = JS_NewObject(B.context);
    JS_SetPropertyStr(B.context, stack_text, "migrated",
                      JS_NewFloat64(B.context, effect->migrated));
    JS_SetPropertyStr(B.context, stack_text, "failed", JS_NewFloat64(B.context, effect->failed));
    JS_SetPropertyStr(B.context, stack_text, "pending", JS_NewFloat64(B.context, effect->pending));
    JSValue cbs = job->callbacks;
    *job = system->jobs[--system->job_count]; /* drop it before calling out (a callback may migrate
                                                 again) */
    int64_t n = 0;
    JS_GetLength(B.context, cbs, &n);
    for (int64_t k = 0; k < n; k++) {
        JSValue function = JS_GetPropertyUint32(B.context, cbs, (uint32_t)k);
        JSValue r = JS_Call(B.context, function, JS_UNDEFINED, 1, &stack_text);
        if (JS_IsException(r)) script_exception("migrate onDone");
        JS_FreeValue(B.context, r);
        JS_FreeValue(B.context, function);
    }
    JS_FreeValue(B.context, cbs);
    JS_FreeValue(B.context, stack_text);
}

/* migration(system) -> { active, migrated, failed, pending } */
static JSValue h_migration(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    int32_t si = -1;
    if (argc) JS_ToInt32(context, &si, argv[0]);
    if (si < 0 || si >= MAX_SYSTEMS || !B.system[si].live) return JS_UNDEFINED;
    return job_status(context, &B.system[si]);
}

/* versions(system) -> [{ name, version, actors, current }] (versions alive now) */
static JSValue h_versions(JSContext *context, JSValueConst this_value, int argc,
                          JSValueConst *argv) {
    (void)this_value;
    int32_t si = -1;
    if (argc) JS_ToInt32(context, &si, argv[0]);
    JSValue a = JS_NewArray(context);
    if (si < 0 || si >= MAX_SYSTEMS || !B.system[si].live) return a;
    bench_system *system = &B.system[si];
    uint32_t n = 0, nseg = ta_host_segment_count(system->host);
    for (uint32_t segment = 0; segment < nseg; segment++) {
        ta_host_segment_info info;
        info.size = sizeof info;
        if (ta_host_segment_info_get(system->host, segment, &info) != TA_OK || !info.live) continue;
        char name[96];
        if (segment)
            snprintf(name, sizeof name, "%s@v%u", system->name, segment + 1);
        else
            snprintf(name, sizeof name, "%s", system->name);
        JSValue o = JS_NewObject(context);
        JS_SetPropertyStr(context, o, "name", JS_NewString(context, name));
        JS_SetPropertyStr(context, o, "version", JS_NewInt32(context, (int32_t)segment + 1));
        JS_SetPropertyStr(context, o, "actors", JS_NewInt32(context, (int32_t)info.actors));
        JS_SetPropertyStr(context, o, "current", JS_NewBool(context, info.current > 0));
        JS_SetPropertyUint32(context, a, n++, o);
    }
    return a;
}

/* mtime(path) -> ms since the epoch of a scenario file, or 0 (CGI-style watching) */
static JSValue h_mtime(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    const char *relative_path = argc ? JS_ToCString(context, argv[0]) : NULL;
    if (!relative_path) return JS_EXCEPTION;
    double ms = 0;
    if (safe_path(relative_path)) {
        char path[2048];
        struct stat sb_;
        snprintf(path, sizeof path, "%s/%s", B.directory, relative_path);
        if (stat(path, &sb_) == 0) {
#ifdef __APPLE__
            struct timespec modified = sb_.st_mtimespec;
#else
            struct timespec modified = sb_.st_mtim;
#endif
            ms = (double)modified.tv_sec * 1e3 + (double)modified.tv_nsec / 1e6;
        }
    }
    JS_FreeCString(context, relative_path);
    return JS_NewFloat64(context, ms);
}

/* ---- the machine (Linux /proc, cgroup v2) ---- */

static double meminfo_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    double v = 0;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') {
            v = atof(line + kl + 1);
            break;
        }
    if (f) fclose(f);
    return v * 1024;
}

/* the tightest memory.max on our cgroup v2 path (bytes, 0 = none) */
static double cgroup_limit(void) {
    char path[512] = "", line[512];
    FILE *f = fopen("/proc/self/cgroup", "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "0::", 3)) {
            snprintf(path, sizeof path, "%s", line + 3);
            path[strcspn(path, "\n")] = 0;
        }
    if (f) fclose(f);
    double best = 0;
    for (;;) {
        char file[700];
        snprintf(file, sizeof file, "/sys/fs/cgroup%s/memory.max", path);
        FILE *m = fopen(file, "r");
        char buffer[64] = "";
        if (m) {
            if (!fgets(buffer, sizeof buffer, m)) buffer[0] = 0;
            fclose(m);
        }
        if (buffer[0] >= '0' && buffer[0] <= '9' && (best == 0 || atof(buffer) < best))
            best = atof(buffer);
        char *slash = strrchr(path, '/');
        if (!slash || !path[0]) break;
        *slash = 0;
    }
    return best;
}

static double rss_bytes(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    double v = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) {
            v = atof(line + 6) * 1024;
            break;
        }
    if (f) fclose(f);
    return v;
}

static JSValue h_rss(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(context, rss_bytes());
}

static JSValue h_stop(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    const char *m = argc > 0 ? JS_ToCString(context, argv[0]) : NULL;
    snprintf(B.stop_message, sizeof B.stop_message, "%s", m ? m : "stopped by script");
    if (m) JS_FreeCString(context, m);
    B.stopping = 1;
    return JS_UNDEFINED;
}

static JSValue call_global(const char *function, int argc, JSValueConst *argv) {
    JSValue g = JS_GetGlobalObject(B.context);
    JSValue f = JS_GetPropertyStr(B.context, g, function);
    JSValue r = JS_UNDEFINED;
    if (JS_IsFunction(B.context, f)) {
        r = JS_Call(B.context, f, g, argc, argv);
        if (JS_IsException(r)) {
            script_exception(function);
            r = JS_UNDEFINED;
        }
    }
    JS_FreeValue(B.context, f);
    JS_FreeValue(B.context, g);
    return r;
}

static int script_init(void) {
    B.runtime = JS_NewRuntime();
    B.context = JS_NewContext(B.runtime);
    JSValue g = JS_GetGlobalObject(B.context);
    JSValue h = JS_NewObject(B.context);
    static const struct {
        const char *name;
        JSCFunction *function;
        int count;
    } functions[] = {{"readText", h_read_text, 1},
                     {"evalFile", h_eval_file, 1},
                     {"createSystem", h_create_system, 3},
                     {"destroySystem", h_destroy_system, 1},
                     {"spawn", h_spawn, 4},
                     {"send", h_send, 3},
                     {"sendMany", h_send_many, 3},
                     {"terminate", h_terminate, 1},
                     {"failSend", h_fail_send, 2},
                     {"address", h_address, 1},
                     {"alive", h_alive, 1},
                     {"now", h_now, 0},
                     {"setTimer", h_set_timer, 3},
                     {"clearTimer", h_clear_timer, 1},
                     {"log", h_log, 2},
                     {"metric", h_metric, 2},
                     {"stats", h_stats, 1},
                     {"stop", h_stop, 1},
                     {"rss", h_rss, 0},
                     {"hibernate", h_hibernate, 3},
                     {"reload", h_reload, 2},
                     {"migrate", h_migrate, 2},
                     {"migration", h_migration, 1},
                     {"versions", h_versions, 1},
                     {"mtime", h_mtime, 1},
                     {"pgStatus", h_postgres_status, 0},
                     {"pgQuery", h_postgres_query, 1},
                     {"pgLoad", h_postgres_load, 3},
                     {"pgStopLoad", h_postgres_stop_load, 0},
                     {"pgStart", h_postgres_start, 0},
                     {"pgRestart", h_postgres_restart, 0},
                     {"pgUse", h_postgres_use, 1}};
    for (size_t i = 0; i < sizeof functions / sizeof functions[0]; i++)
        JS_SetPropertyStr(B.context, h, functions[i].name,
                          JS_NewCFunction(B.context, functions[i].function, functions[i].name,
                                          functions[i].count));
    JS_SetPropertyStr(B.context, h, "scale", JS_NewFloat64(B.context, B.scale));
    JS_SetPropertyStr(B.context, h, "seed", JS_NewFloat64(B.context, (double)B.seed));
    JS_SetPropertyStr(B.context, h, "duration", JS_NewFloat64(B.context, B.duration));
    JSValue params = JS_NewObject(B.context);
    for (int i = 0; i < B.param_count; i++) {
        const char *equals_sign = strchr(B.params[i], '=');
        char key[128];
        snprintf(key, sizeof key, "%.*s",
                 equals_sign ? (int)(equals_sign - B.params[i]) : (int)strlen(B.params[i]),
                 B.params[i]);
        JS_SetPropertyStr(B.context, params, key,
                          JS_NewString(B.context, equals_sign ? equals_sign + 1 : "true"));
    }
    JS_SetPropertyStr(B.context, h, "params", params);
    JSValue m = JS_NewObject(B.context); /* what this machine offers */
    JS_SetPropertyStr(B.context, m, "memTotal", JS_NewFloat64(B.context, meminfo_kb("MemTotal")));
    JS_SetPropertyStr(B.context, m, "memAvailable",
                      JS_NewFloat64(B.context, meminfo_kb("MemAvailable")));
    JS_SetPropertyStr(B.context, m, "memoryLimit", JS_NewFloat64(B.context, cgroup_limit()));
    JS_SetPropertyStr(B.context, m, "cpus",
                      JS_NewFloat64(B.context, (double)sysconf(_SC_NPROCESSORS_ONLN)));
    JS_SetPropertyStr(B.context, m, "rss", JS_NewFloat64(B.context, rss_bytes()));
    JS_SetPropertyStr(B.context, m, "systemMaxActors",
                      JS_NewFloat64(B.context, TA_SYSTEM_MAX_ACTORS));
    JS_SetPropertyStr(B.context, h, "machine", m);
    JS_SetPropertyStr(B.context, g, "__host", h);
    JS_FreeValue(B.context, g);
    char path[2048];
    snprintf(path, sizeof path, "%s/prelude.js", BENCH_SOURCE_DIR);
    size_t length;
    char *text = read_file(path, &length);
    if (!text) {
        fprintf(stderr, "torture-bench: cannot read %s\n", path);
        return 0;
    }
    JSValue r = JS_Eval(B.context, text, length, "prelude.js", JS_EVAL_TYPE_GLOBAL);
    free(text);
    if (JS_IsException(r)) {
        script_exception("prelude");
        return 0;
    }
    JS_FreeValue(B.context, r);
    return 1;
}

static void fire_timers(void) {
    double until = bench_loop_now() + 2;
    for (int guard = 0; guard < 32 && bench_loop_now() < until; guard++) {
        double now = bench_loop_now();
        int best = -1;
        for (int i = 0; i < B.timer_count; i++)
            if (B.timers[i].due <= now && (best < 0 || B.timers[i].due < B.timers[best].due))
                best = i;
        if (best < 0) return;
        bench_timer timer = B.timers[best];
        JSValue function = JS_DupValue(B.context, timer.function);
        if (timer.period > 0) {
            B.timers[best].due = timer.due + timer.period;
            if (B.timers[best].due < now - 10 * timer.period)
                B.timers[best].due = now + timer.period;
        } else {
            JS_FreeValue(B.context, B.timers[best].function);
            B.timers[best] = B.timers[--B.timer_count];
        }
        JSValue r = JS_Call(B.context, function, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) script_exception("timer");
        JS_FreeValue(B.context, r);
        JS_FreeValue(B.context, function);
        if (B.stopping) return;
    }
}

static double next_timer_due(void) {
    double d = INFINITY;
    for (int i = 0; i < B.timer_count; i++)
        if (B.timers[i].due < d) d = B.timers[i].due;
    return d;
}

static void script_jobs(void) {
    double until = bench_loop_now() + 2;
    for (int n = 0; B.runtime && n < 32 && bench_loop_now() < until && JS_IsJobPending(B.runtime);
         n++) {
        JSContext *context;
        if (JS_ExecutePendingJob(B.runtime, &context) < 0) script_exception("promise job");
    }
}

/* ---- running systems ---------------------------------------------------------------------- */

/* ---- effects ------------------------------------------------------------------------------ */

/* Work triggered by an effect that touches the same system or runs scripts is
 * deferred until the batch is acknowledged: effect views are only valid until
 * the next mutating call on that system (tinyactors/system.h). */
enum {
    OPERATION_COMMUNICATION_ERROR = 1,
    OPERATION_TO_BENCH,
    OPERATION_HOST_IO,
    OPERATION_SYSTEM,
    OPERATION_DONE_SPAWN,
    OPERATION_WAKE,
    OPERATION_JOB_DONE
};
typedef struct bench_operation {
    int kind;
    uint64_t actor;  /* session of the sender (the spawner for OPERATION_DONE_SPAWN) */
    uint64_t serial; /* OPERATION_DONE_SPAWN: the finished session */
    char *event, *target, *sendid, *json, *type;
    ta_payload payload; /* retained */
    ta_host_effect job; /* OPERATION_JOB_DONE */
} bench_operation;
static bench_operation *ops;
static int operation_count, cap_ops;

static char *dup_str(ta_string_view v) {
    char *p = (char *)malloc(v.length + 1);
    if (v.length) memcpy(p, v.pointer, v.length);
    p[v.length] = 0;
    return p;
}

static char *payload_json(ta_datamodel_ecmascript *ecmascript, ta_payload payload) {
    if (!payload) return NULL;
    char small[4096];
    size_t n = ta_datamodel_ecmascript_payload_to_json(ecmascript, payload, small, sizeof small);
    if (n == (size_t)-1) return NULL;
    char *out = (char *)malloc(n + 1);
    if (n < sizeof small)
        memcpy(out, small, n + 1);
    else
        ta_datamodel_ecmascript_payload_to_json(ecmascript, payload, out, n + 1);
    return out;
}

static bench_operation *operation_push(int kind, uint64_t actor) {
    if (operation_count == cap_ops) {
        cap_ops = cap_ops ? cap_ops * 2 : 64;
        ops = (bench_operation *)realloc(ops, sizeof(bench_operation) * (size_t)cap_ops);
    }
    bench_operation *operation = &ops[operation_count++];
    memset(operation, 0, sizeof *operation);
    operation->kind = kind;
    operation->actor = actor;
    return operation;
}

static void retain(bench_system *system, ta_payload payload) {
    if (payload) ta_datamodel_ecmascript_vtable()->retain(system->ecmascript, payload);
}
static void release(bench_system *system, ta_payload payload) {
    if (payload) ta_datamodel_ecmascript_vtable()->release(system->ecmascript, payload);
}

/* -- spawned-actor map (session -> spawner) */
static uint32_t spawn_slot(const bench_system *system, uint64_t session) {
    return (uint32_t)((session * 0x9E3779B97F4A7C15ull) >> 32) & (system->spawn_capacity - 1);
}
static void spawn_put(bench_system *system, bench_spawn spawn) {
    if ((system->spawn_count + 1) * 2 > system->spawn_capacity) { /* grow (also drops tombstones) */
        bench_spawn *old = system->spawns;
        uint32_t oc = system->spawn_capacity;
        system->spawn_capacity = oc ? oc * 2 : 256;
        system->spawns = (bench_spawn *)calloc(system->spawn_capacity, sizeof(bench_spawn));
        system->spawn_count = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (old[i].session && old[i].session != UINT64_MAX) spawn_put(system, old[i]);
        free(old);
    }
    uint32_t i = spawn_slot(system, spawn.session);
    while (system->spawns[i].session && system->spawns[i].session != UINT64_MAX)
        i = (i + 1) & (system->spawn_capacity - 1);
    system->spawns[i] = spawn;
    system->spawn_count++;
}
static int spawn_take(bench_system *system, uint64_t session, bench_spawn *out) {
    if (!system->spawn_capacity || !session) return 0;
    for (uint32_t i = spawn_slot(system, session), n = 0;
         system->spawns[i].session && n < system->spawn_capacity;
         i = (i + 1) & (system->spawn_capacity - 1), n++) {
        if (system->spawns[i].session != session) continue;
        *out = system->spawns[i];
        system->spawns[i].session = UINT64_MAX; /* tombstone keeps probe chains intact */
        system->spawns[i].sendid = NULL;
        system->spawn_count--;
        return 1;
    }
    return 0;
}

/* The in-process transport (docs/protocol.md, Transports): every system is a
 * trusted peer of every other; an envelope one router hands out
 * (TA_HOST_EFFECT_PEER) goes into the other system's router (ta_host_deliver),
 * an error back (TA_HOST_EFFECT_PEER_ERROR) into the sender's (ta_host_peer_error).
 * Payloads cross as neutral JSON: the systems have separate datamodels. */
static void peers_connect(int si) {
    bench_system *system = &B.system[si];
    for (int k = 0; k < MAX_SYSTEMS; k++) {
        bench_system *o = &B.system[k];
        if (k == si || !o->live) continue;
        ta_peer_config peer_config;
        memset(&peer_config, 0, sizeof peer_config);
        peer_config.size = sizeof peer_config;
        peer_config.flags = TA_PEER_TRUSTED;
        peer_config.id = (ta_string_view){o->name, (uint32_t)strlen(o->name)};
        ta_host_peer_add(system->host, &peer_config, &system->peer_of[k]);
        peer_config.id = (ta_string_view){system->name, (uint32_t)strlen(system->name)};
        ta_host_peer_add(o->host, &peer_config, &o->peer_of[si]);
    }
}
static void peers_disconnect(int si) {
    for (int k = 0; k < MAX_SYSTEMS; k++) {
        bench_system *o = &B.system[k];
        if (k == si || !o->live || !o->peer_of[si]) continue;
        ta_host_peer_remove(o->host, o->peer_of[si]); /* queued sends: error.communication */
        o->peer_of[si] = 0;
        o->busy = 1;
    }
}
static int peer_index(bench_system *system, uint32_t peer) {
    for (int k = 0; k < MAX_SYSTEMS; k++)
        if (system->peer_of[k] == peer && B.system[k].live) return k;
    return -1;
}

static void deliver_remote(bench_system *from, const ta_host_effect *effect) {
    int ti = peer_index(from, effect->peer);
    bench_system *to = ti >= 0 ? &B.system[ti] : NULL;
    int fi = (int)(from - B.system);
    if (!to) { /* the peer went away meanwhile */
        from->remote_fail++;
        ta_host_peer_error(from->host, effect->peer, effect->auxiliary, effect->msgid,
                           effect->sendid, TA_ROUTE_ERROR_UNREACHABLE);
        return;
    }
    char *json = payload_json(from->ecmascript, effect->payload);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.hops = effect->flags;
    envelope.target = effect->target;
    envelope.name = effect->event;
    envelope.sendid = effect->sendid;
    envelope.origin = effect->auxiliary;
    envelope.msgid = effect->msgid;
    envelope.data = json ? ta_datamodel_ecmascript_payload_from_json(
                               to->ecmascript, (ta_string_view){json, (uint32_t)strlen(json)})
                         : 0;
    envelope.datamodel = 0; /* the ECMAScript datamodel */
    free(json);
    uint32_t outcome;
    ta_host_deliver(to->host, to->peer_of[fi], &envelope, &outcome);
    if (outcome == TA_ROUTE_DELIVERED || outcome == TA_ROUTE_RELAYED) {
        from->remote_out++;
        to->remote_in++;
        to->busy = 1;
    } else {
        if (outcome != TA_ROUTE_DUPLICATE) from->remote_fail++;
        release(to, envelope.data);
        to->busy = 1; /* its router may have an error for us */
    }
}

/* an `err` for one of our envelopes */
static void deliver_remote_error(bench_system *from, const ta_host_effect *effect) {
    int ti = peer_index(from, effect->peer);
    if (ti < 0) return;
    bench_system *to = &B.system[ti];
    ta_host_peer_error(to->host, to->peer_of[(int)(from - B.system)], effect->auxiliary,
                       effect->msgid, effect->sendid, effect->token);
    to->remote_fail++;
    to->busy = 1;
}

/* the system I/O processor (after the effect batch was acknowledged) */
static void system_operation(int si, bench_operation *operation) {
    bench_system *system = &B.system[si];
    if (!strcmp(operation->event, "spawn")) {
        const char *t = operation->target;
        if (!strncmp(t, "./", 2))
            t += 2;
        else if (!strncmp(t, "file:", 5))
            t += 5;
        uint32_t segment = 0, chart = 0;
        if (!system_chart(system, t, strlen(t), &segment, &chart)) {
            system->spawn_failed++;
            release(system, operation->payload);
            communication_error_to(system, operation->actor, operation->sendid);
            return;
        }
        const char *name = operation->sendid ? operation->sendid : "";
        uint64_t session = 0;
        ta_status status = ta_host_spawn(system->host, segment, chart,
                                         (ta_string_view){name, (uint32_t)strlen(name)},
                                         operation->payload, &session);
        if (status != TA_OK) {
            system->spawn_failed++;
            release(system, operation->payload); /* a failed spawn leaves it with us */
            communication_error_to(system, operation->actor, operation->sendid);
            return;
        }
        bench_spawn spawn = {session, operation->actor, operation->sendid};
        operation->sendid = NULL; /* owned by the map now */
        spawn_put(system, spawn);
        system->spawned++;
        system->busy = 1;
        return;
    }
    if (!strcmp(operation->event, "terminate")) {
        ta_address address; /* "#_scxml_<id>" or a full address of this system */
        ta_string_view t = {operation->target, (uint32_t)strlen(operation->target)};
        uint64_t serial = (ta_address_parse_local(t, &address) == TA_OK ||
                           ta_address_parse(t, &address) == TA_OK) &&
                                  address.kind == TA_ADDRESS_SESSION
                              ? address.session
                              : 0;
        release(system, operation->payload);
        if (!serial || ta_host_terminate(system->host, serial) != TA_OK)
            communication_error_to(system, operation->actor, operation->sendid);
        system->busy = 1;
        return;
    }
    release(system, operation->payload); /* unknown operation */
    communication_error_to(system, operation->actor, operation->sendid);
}

static void done_spawn(bench_system *system, bench_operation *operation) {
    char name[256], origin[64];
    snprintf(name, sizeof name, operation->sendid ? "done.spawn.%s" : "done.spawn",
             operation->sendid);
    snprintf(origin, sizeof origin, "#_scxml_%llu", (unsigned long long)operation->serial);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    if (operation->sendid)
        event.sendid = (ta_string_view){operation->sendid, (uint32_t)strlen(operation->sendid)};
    event.origin = (ta_string_view){origin, (uint32_t)strlen(origin)};
    event.origintype = (ta_string_view){SYSTEM_TYPE, (uint32_t)strlen(SYSTEM_TYPE)};
    event.data = operation->payload;
    if (ta_host_send(system->host, operation->actor, &event) != TA_OK)
        release(system, operation->payload); /* spawner gone */
    system->busy = 1;
    system->spawn_done++;
}

static JSValue json_value(const char *json) {
    if (!json) return JS_UNDEFINED;
    JSValue v = JS_ParseJSON(B.context, json, strlen(json), "<event>");
    if (JS_IsException(v)) {
        JS_FreeValue(B.context, JS_GetException(B.context));
        return JS_UNDEFINED;
    }
    return v;
}

static void hib_wake(bench_system *s, uint64_t session);

static void run_operation(int si, bench_operation *operation) {
    bench_system *system = &B.system[si];
    switch (operation->kind) {
    case OPERATION_COMMUNICATION_ERROR:
        communication_error_to(system, operation->actor, operation->sendid);
        break;
    case OPERATION_TO_BENCH: { /* <send type="bench" event="x">: bench.on("x", (data, from) => ...)
                                */
        JSValue argv[3] = {JS_NewString(B.context, operation->event), json_value(operation->json),
                           JS_NewFloat64(B.context, actor_id(si, operation->actor))};
        JS_FreeValue(B.context, call_global("__fromChart", 3, argv));
        for (int i = 0; i < 3; i++)
            JS_FreeValue(B.context, argv[i]);
        break;
    }
    case OPERATION_HOST_IO: { /* scenario-defined processors: bench.ioprocessor(type, msg => ...) */
        JSValue message = JS_NewObject(B.context);
        JS_SetPropertyStr(B.context, message, "event", JS_NewString(B.context, operation->event));
        JS_SetPropertyStr(B.context, message, "target", JS_NewString(B.context, operation->target));
        JS_SetPropertyStr(B.context, message, "data", json_value(operation->json));
        JS_SetPropertyStr(B.context, message, "sendid",
                          operation->sendid ? JS_NewString(B.context, operation->sendid)
                                            : JS_UNDEFINED);
        JS_SetPropertyStr(B.context, message, "from",
                          JS_NewFloat64(B.context, actor_id(si, operation->actor)));
        JSValue argv[2] = {JS_NewString(B.context, operation->type), message};
        JS_FreeValue(B.context, call_global("__hostSend", 2, argv));
        JS_FreeValue(B.context, argv[0]);
        JS_FreeValue(B.context, argv[1]);
        break;
    }
    case OPERATION_SYSTEM:
        system_operation(si, operation);
        break;
    case OPERATION_DONE_SPAWN:
        done_spawn(system, operation);
        break;
    case OPERATION_WAKE:
        hib_wake(system, operation->actor);
        break;
    case OPERATION_JOB_DONE:
        job_done(system, &operation->job);
        break;
    }
}

/* ---- hibernation store (the host side; the system never does I/O) ---- */

static unsigned char *hibernation_buffer;
static size_t hibernation_buffer_capacity;

static int hib_open(bench_system *system) {
    if (system->hibernation_fd > 0) return 1;
    char path[256];
    const char *dir = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/torture-hib-%d-%s-XXXXXX", dir && *dir ? dir : "/tmp",
             (int)getpid(), system->name);
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    unlink(path); /* lives until closed */
    system->hibernation_fd = fd;
    return 1;
}

/* the blob index: session -> place in the file */
static bench_hibernate *hib_find(bench_system *system, uint64_t session, int add) {
    if (add && (system->hibernation_count + 1) * 2 > system->hibernation_capacity) {
        bench_hibernate *old = system->hibernations;
        uint32_t oc = system->hibernation_capacity;
        system->hibernation_capacity = oc ? oc * 2 : 4096;
        system->hibernations =
            (bench_hibernate *)calloc(system->hibernation_capacity, sizeof(bench_hibernate));
        system->hibernation_count = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (old[i].session && old[i].session != UINT64_MAX)
                *hib_find(system, old[i].session, 1) = old[i];
        free(old);
    }
    if (!system->hibernation_capacity) return NULL;
    uint32_t i =
        (uint32_t)((session * 0x9E3779B97F4A7C15ull) >> 32) & (system->hibernation_capacity - 1);
    bench_hibernate *tomb = NULL;
    for (uint32_t n = 0; n < system->hibernation_capacity && system->hibernations[i].session;
         n++, i = (i + 1) & (system->hibernation_capacity - 1)) {
        if (system->hibernations[i].session == session) return &system->hibernations[i];
        if (system->hibernations[i].session == UINT64_MAX && !tomb) tomb = &system->hibernations[i];
    }
    if (!add) return NULL;
    bench_hibernate *hibernation = tomb ? tomb : &system->hibernations[i];
    memset(hibernation, 0, sizeof *hibernation);
    hibernation->session = session;
    system->hibernation_count++;
    return hibernation;
}

/* Hibernate up to `max` cold actors found by scanning up to `scan` slots
 * (ta_host_sweep: a CLOCK sweep over all versions, at most one revolution;
 * it returns fewer than asked for once the revolution is complete). */
static uint32_t hib_sweep(bench_system *system, uint32_t max, uint32_t scan) {
    if (!hib_open(system)) return 0;
    static uint64_t cold[4096];
    uint32_t done = 0;
    for (;;) {
        uint32_t n = ta_host_sweep(system->host, scan, cold, 4096);
        for (uint32_t k = 0; k < n && done < max; k++) {
            size_t length = 0;
            if (!hibernation_buffer) {
                hibernation_buffer_capacity = 1 << 16;
                hibernation_buffer = (unsigned char *)malloc(hibernation_buffer_capacity);
            }
            ta_status status = ta_host_hibernate(system->host, cold[k], hibernation_buffer,
                                                 hibernation_buffer_capacity, &length);
            if (status == TA_ERROR_NO_MEMORY && length > hibernation_buffer_capacity) {
                hibernation_buffer_capacity = length * 2;
                hibernation_buffer =
                    (unsigned char *)realloc(hibernation_buffer, hibernation_buffer_capacity);
                status = ta_host_hibernate(system->host, cold[k], hibernation_buffer,
                                           hibernation_buffer_capacity, &length);
            }
            if (status != TA_OK) continue; /* e.g. functions in its data: stays resident */
            bench_hibernate *hibernation = hib_find(system, cold[k], 1);
            uint64_t t = ta_runtime_now_ns();
            ssize_t w = hibernation ? pwrite(system->hibernation_fd, hibernation_buffer, length,
                                             (off_t)system->hibernation_end)
                                    : -1;
            system->hibernation_write_ns += ta_runtime_now_ns() - t;
            if (w != (ssize_t)length) { /* cannot persist: take it back at once */
                ta_host_wake(system->host, cold[k], hibernation_buffer, length);
                system->hibernations_failed++;
                continue;
            }
            hibernation->offset = system->hibernation_end;
            hibernation->length = (uint32_t)length;
            system->hibernation_end += length;
            system->hibernations_live += length;
            done++;
        }
        if (n < 4096 || done >= max) break;
    }
    return done;
}

static void hib_wake(bench_system *system, uint64_t session) {
    bench_hibernate *hibernation = hib_find(system, session, 0);
    if (!hibernation) return; /* woken already (a migration job and the system may both ask) */
    if (hibernation->length > hibernation_buffer_capacity) {
        hibernation_buffer_capacity = hibernation->length * 2u;
        hibernation_buffer =
            (unsigned char *)realloc(hibernation_buffer, hibernation_buffer_capacity);
    }
    uint64_t t = ta_runtime_now_ns();
    ssize_t r = pread(system->hibernation_fd, hibernation_buffer, hibernation->length,
                      (off_t)hibernation->offset);
    system->hibernation_read_ns += ta_runtime_now_ns() - t;
    if (r != (ssize_t)hibernation->length) {
        bench_log("error", system->name, "wake: short read", 16);
        return;
    }
    ta_status status = ta_host_wake(system->host, session, hibernation_buffer, hibernation->length);
    if (status != TA_OK) {
        char m[64];
        snprintf(m, sizeof m, "wake failed (%d)", status);
        bench_log("error", system->name, m, strlen(m));
        return;
    }
    system->hibernations_live -= hibernation->length;
    hibernation->session = UINT64_MAX;
    system->hibernation_count--;
    system->busy = 1;
}

/* hibernate(system, max, scan) -> number of actors unloaded */
static JSValue h_hibernate(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    int32_t i = -1;
    double max = 1e9, scan = 1e6;
    if (argc) JS_ToInt32(context, &i, argv[0]);
    if (argc > 1) JS_ToFloat64(context, &max, argv[1]);
    if (argc > 2) JS_ToFloat64(context, &scan, argv[2]);
    if (i < 0 || i >= MAX_SYSTEMS || !B.system[i].live) return JS_NewInt32(context, 0);
    return JS_NewFloat64(context, hib_sweep(&B.system[i], (uint32_t)(max > 4e9 ? 4e9 : max),
                                            (uint32_t)(scan > 4e9 ? 4e9 : scan)));
}

static void drain_effects(int si) {
    bench_system *system = &B.system[si];
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(system->host, &effects);
    operation_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        const ta_host_effect *effect = &effects[i];
        switch (effect->kind) {
        case TA_EFFECT_LOG:
            system->logs++;
            if (B.logs_this_second < 20) { /* a sample of chart logs reaches the dashboard */
                B.logs_this_second++;
                char source[96];
                snprintf(source, sizeof source, "%s/%.*s", system->name,
                         (int)effect->auxiliary.length,
                         effect->auxiliary.length ? effect->auxiliary.pointer : "log");
                bench_log("chart", source, effect->event.pointer, effect->event.length);
            }
            break;
        case TA_HOST_EFFECT_PEER:
            deliver_remote(system, effect);
            break;
        case TA_HOST_EFFECT_PEER_ERROR:
            deliver_remote_error(system, effect);
            break;
        case TA_EFFECT_SEND:
            if (effect->io_processor == IO_BENCH) {
                system->to_bench++;
                bench_operation *operation = operation_push(OPERATION_TO_BENCH, effect->session);
                operation->event = dup_str(effect->event);
                operation->json = payload_json(system->ecmascript, effect->payload);
            } else if (effect->io_processor == IO_SYSTEM || effect->io_processor == IO_SYSTEM + 1) {
                bench_operation *operation = operation_push(OPERATION_SYSTEM, effect->session);
                operation->event = dup_str(effect->event);
                operation->target = dup_str(effect->target);
                operation->sendid = effect->sendid.length ? dup_str(effect->sendid) : NULL;
                operation->payload = effect->payload;
                retain(system, effect->payload);
            } else if (effect->io_processor >= IO_HOST &&
                       effect->io_processor < system->io_processor_count) {
                system->host_io++;
                bench_operation *operation = operation_push(OPERATION_HOST_IO, effect->session);
                operation->type = strdup(system->io_names[effect->io_processor]);
                operation->event = dup_str(effect->event);
                operation->target = dup_str(effect->target);
                operation->sendid = effect->sendid.length ? dup_str(effect->sendid) : NULL;
                operation->json = payload_json(system->ecmascript, effect->payload);
            }
            break;
        case TA_EFFECT_DONE: {
            system->done++;
            bench_spawn spawn;
            if (spawn_take(system, effect->session, &spawn)) {
                bench_operation *operation = operation_push(OPERATION_DONE_SPAWN, spawn.spawner);
                operation->serial = spawn.session;
                operation->sendid = spawn.sendid;
                operation->payload = effect->payload;
                retain(system, effect->payload);
            }
            break;
        }
        case TA_EFFECT_WAKE: /* a hibernated actor has work, or a migration wants it: load it */
            operation_push(OPERATION_WAKE, effect->session);
            break;
        case TA_EFFECT_ERROR: {
            system->faults++;
            bench_spawn spawn;
            if (spawn_take(system, effect->session, &spawn)) free(spawn.sendid);
            char m[96];
            snprintf(m, sizeof m, "actor fault %u", effect->flags);
            bench_log("error", system->name, m, strlen(m));
            break;
        }
        case TA_HOST_EFFECT_JOB_DONE: {
            bench_operation *operation = operation_push(OPERATION_JOB_DONE, 0);
            operation->job = *effect;
            break;
        }
        case TA_HOST_EFFECT_SEGMENT_FREED: {
            if (effect->segment < system->image_count && system->images[effect->segment]) {
                free(system->images[effect->segment]->bytes);
                free(system->images[effect->segment]);
                system->images[effect->segment] = NULL;
            }
            system->retired++;
            char m[128];
            snprintf(m, sizeof m, "version %u retired (no actors left)", effect->segment + 1);
            bench_log("info", system->name, m, strlen(m));
            break;
        }
        default:
            break;
        }
    }
    ta_host_effects_ack(system->host);
    int count = operation_count;
    bench_operation *batch = NULL;
    if (count) { /* scripts may run and drain other systems: take the batch */
        batch = (bench_operation *)malloc(sizeof(bench_operation) * (size_t)count);
        memcpy(batch, ops, sizeof(bench_operation) * (size_t)count);
        operation_count = 0;
    }
    for (int i = 0; i < count; i++) {
        if (B.system[si].live) run_operation(si, &batch[i]); /* (a script may have destroyed it) */
        free(batch[i].event);
        free(batch[i].target);
        free(batch[i].sendid);
        free(batch[i].json);
        free(batch[i].type);
    }
    free(batch);
}

static void run_systems(void) {
    static int cursor;
    for (int visited = 0; visited < MAX_SYSTEMS; visited++) {
        int i = cursor;
        cursor = (cursor + 1) % MAX_SYSTEMS;
        bench_system *system = &B.system[i];
        if (!system->live) continue;
        ta_host_advance_time(system->host, (ta_time)(B.now * 1000));
        if (!system->busy && system->deadline > (uint64_t)(B.now * 1000)) continue;
        uint64_t t = ta_runtime_now_ns();
        ta_run result = ta_host_run(system->host, BUDGET);
        system->busy_ms += (double)(ta_runtime_now_ns() - t) / 1e6;
        system->busy = 0; /* operations of this batch may make it busy again */
        drain_effects(i);
        if (!system->live) continue; /* a chart-triggered script may have destroyed it */
        system->busy |= result.status != TA_RUN_IDLE;
        system->deadline = result.next_deadline;
        if (result.status == TA_RUN_FAULT) bench_log("error", system->name, "system fault", 12);
        return; /* one fuel-bounded system slice, then poll I/O */
    }
}

/* ---- samples -------------------------------------------------------------------------------- */

static size_t js_heap_of(JSRuntime *rt) {
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(rt, &u);
    return (size_t)u.malloc_size;
}

static void sample(void) {
    double dt = (B.now - B.last_sample) / 1000;
    if (dt <= 0) dt = 1e-9;
    int heap = B.now >= B.heap_next;
    if (heap) {
        B.heap_next = B.now + 1000;
        B.script_heap = js_heap_of(B.runtime);
    }
    JSValue r = call_global("__beforeSample", 0, NULL);
    JS_FreeValue(B.context, r);
    string_builder b = {0};
    sb_printf(&b, "{\"type\":\"sample\",\"t\":%.3f,\"systems\":[", B.now / 1000);
    double tot_rate = 0, tot_remote = 0, tot_busy = 0;
    uint64_t tot_actors = 0, tot_region = 0, tot_heap = 0, tot_faults = 0;
    int first = 1;
    for (int i = 0; i < MAX_SYSTEMS; i++) {
        bench_system *system = &B.system[i];
        if (!system->live) continue;
        system->js_heap = ta_datamodel_ecmascript_heap_bytes(system->ecmascript); /* O(1) */
        ta_system_stats stats;
        system_stats(system, &stats);
        double delivered = (double)(stats.delivered - system->previous.delivered) / dt;
        double fuel = (double)(stats.fuel - system->previous.fuel) / dt;
        double turns = (double)(stats.turns - system->previous.turns) / dt;
        double tf = (double)(stats.timers_fired - system->previous.timers_fired) / dt;
        double ro = (double)(system->remote_out - system->previous_remote_out) / dt;
        double ri = (double)(system->remote_in - system->previous_remote_in) / dt;
        double lg = (double)(system->logs - system->previous_logs) / dt;
        double to_bench_rate = (double)(system->to_bench - system->previous_to_bench) / dt;
        double hs = (double)(system->host_sent - system->previous_host_sent) / dt;
        double busy = (system->busy_ms - system->previous_busy) / (dt * 1000);
        system->previous = stats;
        system->previous_remote_out = system->remote_out;
        system->previous_remote_in = system->remote_in;
        system->previous_logs = system->logs;
        system->previous_to_bench = system->to_bench;
        system->previous_host_sent = system->host_sent;
        system->previous_busy = system->busy_ms;
        sb_printf(&b,
                  "%s{\"name\":\"%s\",\"actors\":%u,\"actorsCap\":%u,\"runnable\":%u,"
                  "\"events\":%u,\"eventsCap\":%u,\"timers\":%u,\"timersCap\":%u,"
                  "\"workspaces\":%u,\"workspacesCap\":%u,\"slabBytes\":%llu,\"slabCap\":%llu,"
                  "\"regionBytes\":%llu,\"delivered\":%.1f,\"fuel\":%.1f,\"turns\":%.1f,"
                  "\"timersFired\":%.1f,\"remoteOut\":%.1f,\"remoteIn\":%.1f,\"logs\":%.1f,"
                  "\"toBench\":%.1f,\"hostSent\":%.1f,\"busy\":%.4f,\"jsHeap\":%zu,"
                  "\"deliveredTotal\":%llu,\"remoteFailed\":%llu,\"done\":%llu,\"faults\":%llu}",
                  first ? "" : ",", system->name, stats.actors, stats.actors_capacity,
                  stats.runnable, stats.events, stats.events_capacity, stats.timers,
                  stats.timers_capacity, stats.workspaces, stats.workspaces_capacity,
                  (unsigned long long)stats.slab_pages * stats.slab_page_bytes,
                  (unsigned long long)stats.slab_pages_capacity * stats.slab_page_bytes,
                  (unsigned long long)stats.region_bytes, delivered, fuel, turns, tf, ro, ri, lg,
                  to_bench_rate, hs, busy > 1 ? 1.0 : busy, system->js_heap,
                  (unsigned long long)stats.delivered, (unsigned long long)system->remote_fail,
                  (unsigned long long)system->done, (unsigned long long)system->faults);
        first = 0;
        tot_rate += delivered;
        tot_remote += ro;
        tot_busy += busy;
        tot_actors += stats.actors;
        tot_region += stats.region_bytes;
        tot_heap += system->js_heap;
        tot_faults += system->faults;
    }
    sb_printf(&b,
              "],\"totals\":{\"delivered\":%.1f,\"remote\":%.1f,\"busy\":%.4f,\"actors\":%llu,"
              "\"regionBytes\":%llu,\"jsHeap\":%llu,\"faults\":%llu},\"script\":{\"heap\":%zu,"
              "\"errors\":%llu,\"timers\":%d},\"loop\":{\"script\":%.4f,\"run\":%.4f,"
              "\"sample\":%.4f},\"metrics\":{",
              tot_rate, tot_remote, tot_busy, (unsigned long long)tot_actors,
              (unsigned long long)tot_region, (unsigned long long)tot_heap,
              (unsigned long long)tot_faults, B.script_heap, (unsigned long long)B.script_errors,
              B.timer_count, (B.time_script - B.previous_script) / (dt * 1000),
              (B.time_run - B.previous_run) / (dt * 1000),
              (B.time_sample - B.previous_sample) / (dt * 1000));
    B.previous_script = B.time_script;
    B.previous_run = B.time_run;
    B.previous_sample = B.time_sample;
    for (int i = 0; i < B.metric_count; i++) {
        sb_json_str(&b, B.metric_names[i], strlen(B.metric_names[i]));
        double v = B.metric_values[i];
        sb_printf(&b, ":%.6g%s", isfinite(v) ? v : 0.0, i + 1 < B.metric_count ? "," : "");
    }
    sb_printf(&b, "},\"paused\":%s}", B.paused ? "true" : "false");
    emit(b.data, b.length, 1);
    free(b.data);
    B.samples++;
    B.last_sample = B.now;
}

/* ---- scenario lifecycle ---------------------------------------------------------------- */

static void scenario_stop(void) {
    postgres_stop_loads(); /* the scenario's pgbench processes */
    postgres_scenario_reset();
    for (int i = 0; i < MAX_SYSTEMS; i++)
        if (B.system[i].live) system_free(&B.system[i]);
    if (B.context) {
        for (int i = 0; i < B.timer_count; i++)
            JS_FreeValue(B.context, B.timers[i].function);
        if (B.finishing) JS_FreeValue(B.context, B.finish_result);
        JS_FreeContext(B.context);
        JS_FreeRuntime(B.runtime);
    }
    B.context = NULL;
    B.runtime = NULL;
    B.timer_count = 0;
    for (int i = 0; i < B.history_length; i++)
        free(B.history[(B.history_head + i) % HISTORY]);
    for (int i = 0; i < B.log_length; i++)
        free(B.logs[(B.log_head + i) % LOG_RING]);
    B.history_length = B.history_head = B.log_length = B.log_head = 0;
    free(B.summary);
    B.summary = NULL;
    B.metric_count = 0;
    B.script_errors = B.samples = 0;
    B.running = B.finished = B.paused = B.stopping = 0;
    B.finishing = 0;
    B.current[0] = 0;
}

static void send_hello(int client); /* client < 0: everyone */

static int scenario_start(const char *path) {
    char dir[sizeof B.directory];
    snprintf(dir, sizeof dir, "%s", path); /* (path may be B.directory itself) */
    scenario_stop();
    snprintf(B.directory, sizeof B.directory, "%s", dir);
    const char *base = strrchr(dir, '/');
    snprintf(B.current, sizeof B.current, "%.100s", base && base[1] ? base + 1 : dir);
    size_t cl = strlen(B.current);
    if (cl && B.current[cl - 1] == '/') B.current[cl - 1] = 0;
    B.scenario_origin = bench_loop_now();
    B.now = 0;
    B.last_sample = 0;
    B.next_sample = bench_loop_now() + B.interval;
    B.heap_next = 0;
    B.log_window = 0;
    B.duration = B.cli_duration; /* the scenario's own duration once it is loaded */
    if (!script_init()) return 0;
    B.running = 1;
    JSValue r = call_global("__start", 0, NULL); /* scenario.json: systems, actors, scripts */
    JS_FreeValue(B.context, r);
    B.duration = B.cli_duration;
    if (B.duration <= 0) {
        JSValue d = call_global("__scenarioDuration", 0, NULL);
        double dd = 0;
        JS_ToFloat64(B.context, &dd, d);
        JS_FreeValue(B.context, d);
        B.duration = dd > 0 ? dd : 30;
    }
    if (B.net) send_hello(-1);
    return 1;
}

/* final sample, summary and expectations; the systems are released */
static void scenario_finish(void) {
    B.now = now_ms();
    if (!B.finishing) {
        B.finishing = 1;
        B.finish_result = call_global("__finish", 0, NULL);
    }
    if (JS_PromiseState(B.context, B.finish_result) == JS_PROMISE_PENDING) return;
    JSValue summary = JS_PromiseResult(B.context, B.finish_result);
    if (JS_PromiseState(B.context, B.finish_result) == JS_PROMISE_REJECTED) {
        JS_Throw(B.context, JS_DupValue(B.context, summary));
        script_exception("finish");
    }
    if (B.now - B.last_sample >= 1) sample(); /* include asynchronous final measurements */
    JS_FreeValue(B.context, B.finish_result);
    B.finishing = 0;
    const char *s = JS_IsString(summary) ? JS_ToCString(B.context, summary) : NULL;
    int failed = B.script_errors > 0;
    if (s) {
        emit(s, strlen(s), 0);
        printf("%s\n", s);
        fflush(stdout);
        failed |= strstr(s, "\"ok\":false") != NULL;
        B.summary = strdup(s);
        JS_FreeCString(B.context, s);
    }
    JS_FreeValue(B.context, summary);
    B.failed |= failed;
    B.finished = 1;
    postgres_stop_loads();
    postgres_close_connection();
    for (int i = 0; i < MAX_SYSTEMS; i++)
        if (B.system[i].live) system_free(&B.system[i]);
    for (int i = 0; i < B.timer_count; i++)
        JS_FreeValue(B.context, B.timers[i].function);
    B.timer_count = 0;
}

static int valid_id(const char *id) {
    if (!id[0] || strlen(id) > 100) return 0;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_'))
            return 0;
    char path[1400];
    snprintf(path, sizeof path, "%s/%s/scenario.json", B.catalog, id);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* GET /api/scenarios: { current, state, scenarios: [{ id, scenario }] } */
static char *on_http(void *user, const char *path, size_t *length, const char **type) {
    (void)user;
    if (!strcmp(path, "/api/services")) { /* { postgres: {...} } */
        *type = "application/json";
        string_builder b = {0};
        sb_printf(&b, "{\"postgres\":");
        postgres_status_json(&b, 0);
        sb_printf(&b, "}");
        *length = b.length;
        return b.data;
    }
    if (strcmp(path, "/api/scenarios")) return NULL;
    *type = "application/json";
    string_builder b = {0};
    sb_printf(&b, "{\"services\":{\"postgres\":");
    postgres_status_json(&b, 0);
    sb_printf(&b, "},\"current\":");
    if (B.current[0])
        sb_json_str(&b, B.current, strlen(B.current));
    else
        sb_printf(&b, "null");
    sb_printf(&b, ",\"state\":\"%s\",\"scenarios\":[",
              !B.running   ? "idle"
              : B.finished ? "finished"
                           : "running");
    DIR *d = opendir(B.catalog);
    char *names[64];
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64)
        if (e->d_name[0] != '.' && valid_id(e->d_name)) names[n++] = strdup(e->d_name);
    if (d) closedir(d);
    for (int i = 1; i < n; i++) /* sorted by id */
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char *t = names[j];
            names[j] = names[j - 1];
            names[j - 1] = t;
        }
    int first = 1;
    for (int i = 0; i < n; i++) {
        char file[1400];
        snprintf(file, sizeof file, "%s/%s/scenario.json", B.catalog, names[i]);
        size_t fl = 0;
        char *text = read_file(file, &fl);
        const char *t = text;
        while (t && (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t'))
            t++;
        if (t && *t == '{') { /* scenario.json is embedded as is */
            sb_printf(&b, "%s{\"id\":", first ? "" : ",");
            sb_json_str(&b, names[i], strlen(names[i]));
            sb_printf(&b, ",\"scenario\":");
            sb_put(&b, t, fl - (size_t)(t - text));
            sb_put(&b, "}", 1);
            first = 0;
        }
        free(text);
        free(names[i]);
    }
    sb_printf(&b, "]}");
    *length = b.length;
    return b.data;
}

/* ---- the PostgreSQL service (bindings/postgres; `mise run postgres:start`)
 * --------------------------- Scenarios that declare "services": ["postgres"] use the instance the
 * PostgreSQL binding runs in this orb (127.0.0.1:54320, database tinyactors).
 * The bench probes it every second while a dashboard is open and pushes
 * {type: "service"} messages; {cmd: "start-service"} runs `mise run postgres:start`.
 * Scenario scripts query it (bench.postgres.query) and put load on it with
 * background pgbench processes (bench.postgres.load). */
typedef struct postgres_request {
    struct postgres_request *next;
    char *sql;
    JSValue settle[2];
} postgres_request;
static struct {
    char state[16]; /* unknown | unavailable | starting | missing | available | failed */
    char detail[300], version[32], extension[16];
    int port;
    pid_t starter; /* `mise run postgres:start` while it runs */
    double next_probe;
    bench_postgres *connection; /* the scenario's connection */
    bench_postgres *probe;
    int wanted, inflight, query_count;
    postgres_request *first, *last;
    pid_t loads[16];
    int load_count;
    char database[64]; /* the scenario's database (bench.postgres.use); the probe always checks
                          tinyactors */
} POSTGRES = {.state = "unknown", .port = 54320, .database = "tinyactors"};

static const char *postgres_repository(void) {
#ifdef BENCH_REPO_DIR
    return BENCH_REPO_DIR;
#else
    return ".";
#endif
}

static void postgres_status_json(string_builder *b, int with_type) {
    sb_printf(b,
              "{%s\"name\":\"postgres\",\"state\":\"%s\",\"address\":\"127.0.0.1:%d\",\"version\":",
              with_type ? "\"type\":\"service\"," : "", POSTGRES.state, POSTGRES.port);
    sb_json_str(b, POSTGRES.version, strlen(POSTGRES.version));
    sb_printf(b, ",\"extension\":");
    sb_json_str(b, POSTGRES.extension, strlen(POSTGRES.extension));
    sb_printf(b, ",\"detail\":");
    sb_json_str(b, POSTGRES.detail, strlen(POSTGRES.detail));
    sb_printf(b, ",\"start\":\"mise run pg:start\"}");
}

static void postgres_announce(int client) {
    if (!B.net) return;
    string_builder b = {0};
    postgres_status_json(&b, 1);
    if (client < 0)
        bench_net_broadcast(B.net, b.data, b.length);
    else
        bench_net_send(B.net, client, b.data, b.length);
    free(b.data);
}

static void postgres_set(const char *state, const char *detail) {
    int changed = strcmp(POSTGRES.state, state) || strcmp(POSTGRES.detail, detail);
    snprintf(POSTGRES.state, sizeof POSTGRES.state, "%s", state);
    snprintf(POSTGRES.detail, sizeof POSTGRES.detail, "%s", detail);
    if (changed) postgres_announce(-1);
}

/* the "key":"value" of a one-row JSON result (no escapes in these values) */
static void row_field(const char *json, const char *key, char *out, size_t capacity) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    out[0] = 0;
    if (!p) return;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (e && (size_t)(e - p) < capacity) {
        memcpy(out, p, (size_t)(e - p));
        out[e - p] = 0;
    }
}

static void postgres_probe_done(void *user, const char *json, const char *error) {
    (void)user;
    bench_postgres_close(POSTGRES.probe);
    POSTGRES.probe = NULL;
    if (error) {
        if (POSTGRES.starter)
            postgres_set("starting", "mise run pg:start is setting it up");
        else if (strstr(error, "does not exist"))
            postgres_set("missing", "PostgreSQL runs, but has no tinyactors database yet");
        else
            postgres_set("unavailable", error);
        return;
    }
    row_field(json, "v", POSTGRES.version, sizeof POSTGRES.version);
    row_field(json, "e", POSTGRES.extension, sizeof POSTGRES.extension);
    if (POSTGRES.extension[0])
        postgres_set("available", "");
    else
        postgres_set("missing", "PostgreSQL runs, but the tinyactors extension is not created");
}

static void postgres_probe(void) {
    if (POSTGRES.probe) return;
    POSTGRES.next_probe = bench_loop_now() + 1000;
    POSTGRES.probe = bench_postgres_connect(B.event_loop, POSTGRES.port, "tinyactors", 300);
    if (!POSTGRES.probe ||
        bench_postgres_query(
            POSTGRES.probe,
            "SELECT current_setting('server_version') AS v, coalesce((SELECT extversion FROM "
            "pg_extension WHERE extname = 'tinyactors'), '') AS e",
            postgres_probe_done, NULL))
        postgres_probe_done(NULL, NULL, "cannot start PostgreSQL probe");
}

static void postgres_child_done(void *user, pid_t pid, int status) {
    if (user) {
        unlink(user);
        free(user);
    }
    for (int i = 0; i < POSTGRES.load_count; i++)
        if (POSTGRES.loads[i] == pid) {
            POSTGRES.loads[i] = POSTGRES.loads[--POSTGRES.load_count];
            break;
        }
    if (POSTGRES.starter == pid) {
        POSTGRES.starter = 0;
        POSTGRES.next_probe = 0;
        if (!WIFEXITED(status) || WEXITSTATUS(status))
            postgres_set("failed",
                         "mise failed; see /tmp/torture-pg-start.log or torture-pg-restart.log");
    }
}

/* run `mise run postgres:start` in the background (repository root); the probe follows it */
static void postgres_start(void) {
    if (POSTGRES.starter || !strcmp(POSTGRES.state, "available")) return;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(postgres_repository())) _exit(126);
        if (!freopen("/tmp/torture-pg-start.log", "w", stdout)) _exit(126);
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execlp("mise", "mise", "run", "pg:start", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        setpgid(pid, pid);
        bench_loop_child(B.event_loop, pid, postgres_child_done, NULL);
        POSTGRES.starter = pid;
        postgres_set("starting", "mise run pg:start");
    } else {
        postgres_set("failed", "cannot start mise");
    }
}

static void postgres_stop_loads(void) {
    for (int i = 0; i < POSTGRES.load_count; i++)
        bench_loop_terminate(B.event_loop, POSTGRES.loads[i], 1000);
    POSTGRES.load_count = 0;
}

static void postgres_close_connection(void) {
    if (POSTGRES.connection) bench_postgres_close(POSTGRES.connection);
    POSTGRES.connection = NULL;
    while (POSTGRES.first) {
        postgres_request *q = POSTGRES.first;
        POSTGRES.first = q->next;
        JS_FreeValue(B.context, q->settle[0]);
        JS_FreeValue(B.context, q->settle[1]);
        free(q->sql);
        free(q);
    }
    POSTGRES.last = NULL;
    POSTGRES.query_count = POSTGRES.inflight = 0;
}

static void postgres_scenario_reset(void) { /* the next scenario starts on tinyactors */
    postgres_close_connection();
    POSTGRES.wanted = 0;
    snprintf(POSTGRES.database, sizeof POSTGRES.database, "tinyactors");
}

/* bench.postgres.status() */
static JSValue h_postgres_status(JSContext *context, JSValueConst this_value, int argc,
                                 JSValueConst *argv) {
    (void)this_value;
    (void)argc;
    (void)argv;
    POSTGRES.wanted = 1; /* cached status; the loop probes, including in headless runs */
    string_builder b = {0};
    postgres_status_json(&b, 0);
    JSValue r = JS_NewStringLen(context, b.data, b.length);
    free(b.data);
    return r;
}

static void postgres_query_done(void *user, const char *json, const char *error) {
    (void)user;
    postgres_request *q = POSTGRES.first;
    POSTGRES.first = q->next;
    if (!POSTGRES.first) POSTGRES.last = NULL;
    POSTGRES.query_count--;
    POSTGRES.inflight = 0;
    JSValue arg;
    if (error) {
        arg = JS_NewError(B.context);
        JS_SetPropertyStr(B.context, arg, "message", JS_NewString(B.context, error));
    } else
        arg = JS_NewString(B.context, json);
    JSValue r = JS_Call(B.context, q->settle[error ? 1 : 0], JS_UNDEFINED, 1, &arg);
    JS_FreeValue(B.context, r);
    JS_FreeValue(B.context, arg);
    JS_FreeValue(B.context, q->settle[0]);
    JS_FreeValue(B.context, q->settle[1]);
    free(q->sql);
    free(q);
}

static void postgres_work(void) {
    if (bench_loop_now() >= POSTGRES.next_probe &&
        (POSTGRES.wanted || POSTGRES.starter || (B.net && bench_net_clients(B.net))))
        postgres_probe();
    if (!POSTGRES.first || POSTGRES.inflight) return;
    if (POSTGRES.connection && bench_postgres_broken(POSTGRES.connection)) {
        bench_postgres_close(POSTGRES.connection);
        POSTGRES.connection = NULL;
    }
    if (!POSTGRES.connection)
        POSTGRES.connection =
            bench_postgres_connect(B.event_loop, POSTGRES.port, POSTGRES.database, 1000);
    POSTGRES.inflight = 1;
    if (!POSTGRES.connection ||
        bench_postgres_query(POSTGRES.connection, POSTGRES.first->sql, postgres_query_done, NULL))
        postgres_query_done(NULL, NULL, "cannot start PostgreSQL query");
}

/* bench.postgres.query(sql) -> Promise<JSON text>; FIFO on the scenario connection. */
static JSValue h_postgres_query(JSContext *context, JSValueConst this_value, int argc,
                                JSValueConst *argv) {
    (void)this_value;
    const char *sql = argc ? JS_ToCString(context, argv[0]) : NULL;
    if (!sql) return JS_ThrowTypeError(context, "pgQuery(sql)");
    postgres_request *q = POSTGRES.query_count < 128 ? calloc(1, sizeof *q) : NULL;
    if (q) q->sql = strdup(sql);
    JS_FreeCString(context, sql);
    if (!q || !q->sql) {
        free(q);
        return JS_ThrowInternalError(context, "PostgreSQL query queue full");
    }
    JSValue promise = JS_NewPromiseCapability(context, q->settle);
    if (JS_IsException(promise)) {
        free(q->sql);
        free(q);
        return promise;
    }
    if (POSTGRES.last)
        POSTGRES.last->next = q;
    else
        POSTGRES.first = q;
    POSTGRES.last = q;
    POSTGRES.query_count++;
    return promise;
}

/* bench.postgres.load(clients, seconds, script) -> pid: pgbench in the background */
static JSValue h_postgres_load(JSContext *context, JSValueConst this_value, int argc,
                               JSValueConst *argv) {
    (void)this_value;
    int32_t clients = 4, seconds = 10;
    if (argc > 0) JS_ToInt32(context, &clients, argv[0]);
    if (argc > 1) JS_ToInt32(context, &seconds, argv[1]);
    const char *script = argc > 2 ? JS_ToCString(context, argv[2]) : NULL;
    if (!script || POSTGRES.load_count >= 16 || bench_loop_children(B.event_loop) >= 63) {
        if (script) JS_FreeCString(context, script);
        return JS_ThrowTypeError(context, "pgLoad(clients, seconds, script): at most 16 at a time");
    }
    static unsigned serial;
    char file[80];
    snprintf(file, sizeof file, "/tmp/torture-pgbench-%d-%u.sql", (int)getpid(), ++serial);
    FILE *f = fopen(file, "w");
    if (f) {
        fputs(script, f);
        fclose(f);
    }
    JS_FreeCString(context, script);
    if (!f) return JS_ThrowInternalError(context, "cannot write pgbench script");
    char port[16], c[16], j[16], t[16];
    if (clients < 1) clients = 1;
    snprintf(port, sizeof port, "%d", POSTGRES.port);
    snprintf(c, sizeof c, "%d", clients);
    snprintf(j, sizeof j, "%d", clients < 4 ? clients : 4);
    snprintf(t, sizeof t, "%d", seconds < 1 ? 1 : seconds);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(postgres_repository())) _exit(126);
        if (!freopen("/dev/null", "w", stdout)) _exit(126);
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execlp("mise", "mise", "-C", "bindings/postgres", "exec", "--", "pgbench", "-h",
               "127.0.0.1", "-p", port, "-U", "postgres", "-n", "-c", c, "-j", j, "-T", t, "-f",
               file, POSTGRES.database, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        unlink(file);
        return JS_ThrowInternalError(context, "cannot start pgbench");
    }
    setpgid(pid, pid);
    bench_loop_child(B.event_loop, pid, postgres_child_done, strdup(file));
    POSTGRES.loads[POSTGRES.load_count++] = pid;
    return JS_NewInt32(context, pid);
}

/* bench.postgres.use(db): the database later queries and loads go to (a scenario's
 * own, e.g. one holding millions of actors); reset when the scenario stops */
static JSValue h_postgres_use(JSContext *context, JSValueConst this_value, int argc,
                              JSValueConst *argv) {
    (void)this_value;
    const char *db = argc ? JS_ToCString(context, argv[0]) : NULL;
    int ok = db && db[0] && strlen(db) < sizeof POSTGRES.database;
    for (const char *p = db; ok && *p; p++)
        ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_';
    if (ok && strcmp(db, POSTGRES.database)) {
        if (POSTGRES.first) {
            JS_FreeCString(context, db);
            return JS_ThrowTypeError(context, "await pending queries before pg.use()");
        }
        postgres_close_connection();
        snprintf(POSTGRES.database, sizeof POSTGRES.database, "%s", db);
    }
    if (db) JS_FreeCString(context, db);
    if (!ok) return JS_ThrowTypeError(context, "pgUse(db): a database name of [a-z0-9_]");
    return JS_UNDEFINED;
}

static JSValue h_postgres_stop_load(JSContext *context, JSValueConst this_value, int argc,
                                    JSValueConst *argv) {
    (void)context;
    (void)this_value;
    (void)argc;
    (void)argv;
    postgres_stop_loads();
    return JS_UNDEFINED;
}

static JSValue h_postgres_start(JSContext *context, JSValueConst this_value, int argc,
                                JSValueConst *argv) {
    (void)context;
    (void)this_value;
    (void)argc;
    (void)argv;
    postgres_start();
    return JS_UNDEFINED;
}

/* bench.postgres.restart(): `mise run postgres:restart` in the background */
static JSValue h_postgres_restart(JSContext *context, JSValueConst this_value, int argc,
                                  JSValueConst *argv) {
    (void)this_value;
    (void)argc;
    (void)argv;
    if (POSTGRES.starter) return JS_UNDEFINED;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(postgres_repository())) _exit(126);
        if (!freopen("/tmp/torture-pg-restart.log", "w", stdout)) _exit(126);
        dup2(STDOUT_FILENO, STDERR_FILENO);
        execlp("mise", "mise", "run", "pg:restart", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return JS_ThrowInternalError(context, "cannot start mise");
    setpgid(pid, pid);
    bench_loop_child(B.event_loop, pid, postgres_child_done, NULL);
    postgres_set("starting", "restarting (mise run pg:restart)");
    POSTGRES.starter = pid; /* the probe follows it */
    return JS_UNDEFINED;
}

/* ---- dashboard protocol ------------------------------------------------------------------ */

static void send_to(int client, const char *message, size_t length) {
    if (client < 0)
        bench_net_broadcast(B.net, message, length);
    else
        bench_net_send(B.net, client, message, length);
}

static void send_hello(int client) {
    JSValue info = B.context ? call_global("__scenarioInfo", 0, NULL) : JS_UNDEFINED;
    const char *s = JS_IsString(info) ? JS_ToCString(B.context, info) : NULL;
    string_builder b = {0};
    sb_printf(&b,
              "{\"type\":\"hello\",\"id\":%s%s%s,\"state\":\"%s\",\"interval\":%.0f,"
              "\"duration\":%.3f,\"t\":%.3f,\"scenario\":%s}",
              B.current[0] ? "\"" : "", B.current[0] ? B.current : "null", B.current[0] ? "\"" : "",
              !B.running   ? "idle"
              : B.finished ? "finished"
                           : "running",
              B.interval, B.loop ? 0.0 : B.duration, B.now / 1000, s ? s : "{}");
    send_to(client, b.data, b.length);
    if (s) JS_FreeCString(B.context, s);
    if (B.context) JS_FreeValue(B.context, info); /* (no context while idle) */
    b.length = 0;
    sb_printf(&b, "{\"type\":\"history\",\"samples\":[");
    for (int i = 0; i < B.history_length; i++) {
        const char *h = B.history[(B.history_head + i) % HISTORY];
        if (i) sb_put(&b, ",", 1);
        sb_put(&b, h, strlen(h));
    }
    sb_printf(&b, "],\"logs\":[");
    for (int i = 0; i < B.log_length; i++) {
        const char *h = B.logs[(B.log_head + i) % LOG_RING];
        if (i) sb_put(&b, ",", 1);
        sb_put(&b, h, strlen(h));
    }
    sb_put(&b, "]}", 2);
    send_to(client, b.data, b.length);
    if (B.summary) send_to(client, B.summary, strlen(B.summary));
    free(b.data);
}

static void on_open(void *user, int client) {
    (void)user;
    send_hello(client);
    postgres_announce(client);
}

/* the value of "key":"value" in a small JSON command (no escapes needed) */
static int json_field(const char *message, size_t length, const char *key, char *out,
                      size_t capacity) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    char *copy = (char *)malloc(length + 1);
    memcpy(copy, message, length);
    copy[length] = 0;
    const char *p = strstr(copy, pat);
    int ok = 0;
    if (p) {
        p = strchr(p + strlen(pat), ':');
        if (p) p = strchr(p, '"');
        const char *e = p ? strchr(p + 1, '"') : NULL;
        if (e && (size_t)(e - p - 1) < capacity) {
            memcpy(out, p + 1, (size_t)(e - p - 1));
            out[e - p - 1] = 0;
            ok = 1;
        }
    }
    free(copy);
    return ok;
}

static void on_message(void *user, int client, const char *message, size_t length) {
    (void)user;
    char cmd[32], id[128];
    if (!json_field(message, length, "cmd", cmd, sizeof cmd)) return;
    if (!strcmp(cmd, "open")) { /* switch to a scenario (unless it is the one running) */
        if (json_field(message, length, "scenario", id, sizeof id) && valid_id(id) &&
            (strcmp(id, B.current) || !B.running))
            snprintf(B.pending, sizeof B.pending, "%s", id);
        else
            send_hello(client);
        return;
    }
    if (!strcmp(cmd, "restart")) {
        if (B.current[0]) snprintf(B.pending, sizeof B.pending, "%s", B.current);
        return;
    }
    if (!strcmp(cmd, "start-service")) { /* the dashboard's Start button (PostgreSQL) */
        if (json_field(message, length, "name", id, sizeof id) && !strcmp(id, "postgres"))
            postgres_start();
        return;
    }
    if (!B.context || !B.running || B.finished) return;
    JSValue arg = JS_NewStringLen(B.context, message, length);
    JSValue r = call_global("__command", 1, &arg);
    const char *s = JS_IsString(r) ? JS_ToCString(B.context, r) : NULL;
    if (s) { /* host-level commands returned by the prelude */
        if (!strcmp(s, "pause")) B.paused = 1;
        if (!strcmp(s, "resume")) B.paused = 0;
        if (!strcmp(s, "stop")) {
            B.stopping = 1;
            snprintf(B.stop_message, sizeof B.stop_message, "stopped from the dashboard");
        }
        JS_FreeCString(B.context, s);
    }
    JS_FreeValue(B.context, r);
    JS_FreeValue(B.context, arg);
}

/* ---- main ------------------------------------------------------------------------------------ */

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
    bench_loop_wakeup();
}

static void usage(void) {
    fprintf(stderr,
            "usage: torture-bench [SCENARIO_DIR] [--scenarios DIR] [--duration S] [--interval MS]\n"
            "         [--scale F] [--out FILE] [--port N] [--host ADDR] [--web DIR] [--headless]\n"
            "         [--loop] [--seed N] [--quiet] [--param KEY=VALUE]...\n");
}

int main(int argc, char **argv) {
    memset(&B, 0, sizeof B);
    B.cli_duration = -1;
    B.interval = 250;
    B.scale = 1;
    B.seed = 42;
    B.port = getenv("PORT") ? atoi(getenv("PORT")) : 8090; /* service managers set PORT */
    if (getenv("BENCH_POSTGRES_PORT")) POSTGRES.port = atoi(getenv("BENCH_POSTGRES_PORT"));
    snprintf(B.host, sizeof B.host, "127.0.0.1");
    snprintf(B.web, sizeof B.web, "%s", BENCH_WEB_DIR);
    const char *out = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--duration") && v)
            B.cli_duration = atof(argv[++i]);
        else if (!strcmp(a, "--interval") && v)
            B.interval = atof(argv[++i]);
        else if (!strcmp(a, "--scale") && v)
            B.scale = atof(argv[++i]);
        else if (!strcmp(a, "--out") && v)
            out = argv[++i];
        else if (!strcmp(a, "--port") && v)
            B.port = atoi(argv[++i]);
        else if (!strcmp(a, "--host") && v)
            snprintf(B.host, sizeof B.host, "%s", argv[++i]);
        else if (!strcmp(a, "--web") && v)
            snprintf(B.web, sizeof B.web, "%s", argv[++i]);
        else if (!strcmp(a, "--scenarios") && v)
            snprintf(B.catalog, sizeof B.catalog, "%s", argv[++i]);
        else if (!strcmp(a, "--seed") && v)
            B.seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--param") && v && B.param_count < 32)
            B.params[B.param_count++] = argv[++i];
        else if (!strcmp(a, "--headless"))
            B.headless = 1;
        else if (!strcmp(a, "--loop"))
            B.loop = 1;
        else if (!strcmp(a, "--quiet"))
            B.quiet = 1;
        else if (a[0] != '-' && !B.directory[0])
            snprintf(B.directory, sizeof B.directory, "%s", a);
        else {
            usage();
            return 2;
        }
    }
    size_t dl = strlen(B.directory);
    while (dl > 1 && B.directory[dl - 1] == '/')
        B.directory[--dl] = 0;
    if (!B.catalog[0]) { /* the scenario's parent directory, else the bundled scenarios */
        const char *slash = strrchr(B.directory, '/');
        if (slash)
            snprintf(B.catalog, sizeof B.catalog, "%.*s", (int)(slash - B.directory), B.directory);
        else if (B.directory[0])
            snprintf(B.catalog, sizeof B.catalog, ".");
        else
            snprintf(B.catalog, sizeof B.catalog, "%s/scenarios", BENCH_SOURCE_DIR);
    }
    if (!B.directory[0] && B.headless) {
        usage();
        return 2;
    }
    if (B.interval < 20) B.interval = 20;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (out && !(B.out = fopen(out, "w"))) {
        fprintf(stderr, "torture-bench: cannot write %s: %s\n", out, strerror(errno));
        return 2;
    }
    if (B.out) setvbuf(B.out, NULL, _IOLBF, 0);
    B.event_loop = bench_loop_new();
    if (!B.event_loop) return 2;
    B.scenario_origin = bench_loop_now();
    if (!B.headless) {
        bench_net_handlers h = {NULL, on_open, on_message, on_http};
        B.net = bench_net_listen(B.event_loop, B.host, B.port, B.web, h);
        if (!B.net) {
            fprintf(stderr, "torture-bench: cannot listen on %s:%d\n", B.host, B.port);
            return 2;
        }
        fprintf(stderr, "torture-bench: dashboard on http://%s:%d/ (WebSocket /ws)\n", B.host,
                bench_net_port(B.net));
    }
    if (B.directory[0] && !scenario_start(B.directory)) return 2;
    for (;;) {
        if (B.pending[0]) { /* switch scenarios (from the dashboard) */
            char dir[1400];
            snprintf(dir, sizeof dir, "%s/%s", B.catalog, B.pending);
            B.pending[0] = 0;
            fprintf(stderr, "torture-bench: switching to %s\n", dir);
            if (!scenario_start(dir)) scenario_stop();
        }
        int active = B.running && !B.finished;
        if (interrupted && !active) break;
        postgres_work();
        script_jobs();
        bench_loop_poll(B.event_loop, bench_loop_now());
        if (active) {
            B.now = now_ms();
            if ((B.now / 1000 >= B.duration && !B.loop) || B.stopping || interrupted ||
                B.finishing) {
                scenario_finish();
                if (B.finished) {
                    if (B.net) send_hello(-1);
                    if (B.headless || interrupted) break;
                    continue;
                }
            }
            if (B.now - B.log_window >= 1000) {
                B.log_window = B.now;
                B.logs_this_second = 0;
            }
            if (!B.paused || B.finishing) {
                double t0 = now_ms();
                fire_timers();
                double t1 = now_ms();
                B.time_script += t1 - t0;
                bench_loop_poll(B.event_loop, bench_loop_now());
                t1 = now_ms();
                B.now = t1;
                if (!B.finishing) run_systems();
                B.time_run += now_ms() - t1;
                bench_loop_poll(B.event_loop, bench_loop_now());
            }
            B.now = now_ms();
            if (bench_loop_now() >= B.next_sample) {
                double t0 = now_ms();
                sample();
                B.time_sample += now_ms() - t0;
                B.next_sample += B.interval;
                if (B.next_sample < bench_loop_now()) B.next_sample = bench_loop_now() + B.interval;
            }
        } else if (B.headless) {
            break;
        }
        int busy = interrupted || B.pending[0] || (B.runtime && JS_IsJobPending(B.runtime)) ||
                   (POSTGRES.first && !POSTGRES.inflight);
        double wake = active ? B.next_sample : INFINITY;
        if (!POSTGRES.probe &&
            (POSTGRES.wanted || POSTGRES.starter || (B.net && bench_net_clients(B.net))))
            wake = fmin(wake, POSTGRES.next_probe);
        if (active && !B.finishing && !B.loop)
            wake = fmin(wake, B.scenario_origin + B.duration * 1000);
        if (active && !B.paused && !B.finishing) {
            for (int i = 0; i < MAX_SYSTEMS; i++) {
                if (!B.system[i].live) continue;
                busy |= B.system[i].busy;
                if (B.system[i].deadline != UINT64_MAX)
                    wake = fmin(wake, B.scenario_origin + (double)B.system[i].deadline / 1000);
            }
        }
        if (active && (!B.paused || B.finishing)) wake = fmin(wake, next_timer_due());
        bench_loop_poll(B.event_loop, busy ? bench_loop_now() : wake);
    }
    if (B.net) {
        for (int i = 0; i < 20; i++) /* flush the last messages */
            bench_loop_poll(B.event_loop, bench_loop_now() + 5);
        bench_net_close(B.net);
        B.net = NULL;
    }
    scenario_stop();
    bench_postgres_close(POSTGRES.probe);
    if (POSTGRES.starter) bench_loop_terminate(B.event_loop, POSTGRES.starter, 1000);
    while (bench_loop_children(B.event_loop))
        bench_loop_poll(B.event_loop, INFINITY);
    bench_loop_free(B.event_loop);
    free(B.timers);
    free(ops);
    if (B.out) fclose(B.out);
    return B.failed ? 1 : 0;
}

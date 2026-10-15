/* test_durable.c — every persistence path preserves a session's durable state
 * (docs/durability.md).
 *
 * A property test: random event sequences drive sessions of several charts
 * (parallel and history states, late binding, delayed sends with payloads,
 * <cancel>, generated ids, invoked children and grandchildren with params,
 * autoforward and finalize, sends to #_<invokeid>, #_scxml_ and #_internal,
 * error events). Three worlds receive the same inputs:
 *
 *   reference  one system, never persisted;
 *   persisted  after every step, every session goes through
 *              export -> (ta_blob_migrate to an equivalent image, when it is a
 *              single session) -> adopt into new systems on a NEW datamodel
 *              host (sessions of one system are spread over two) ->
 *              hibernate -> wake (lazily on TA_EFFECT_WAKE, or before the next
 *              export) -> system + datamodel snapshot -> restore into new
 *              regions and yet another new datamodel host;
 *   host       the same through ta_host: ta_host_hibernate / TA_EFFECT_WAKE /
 *              ta_host_wake, a migration to a new version every third
 *              step, ta_host_snapshot -> ta_host_restore with a new
 *              datamodel host.
 *
 * After every step the observable state must match: each session's log (with
 * the step it happened in, so timers fire at the same time), the effects it
 * caused, its configuration, its datamodel (all <data> and the bound _event)
 * and the ids it generated. Sessions are named by a path (root name / child
 * number / ...) carried in their datamodel: the ids of sessions spawned later
 * are the spawning system's to allocate (not session state), so "#_scxml_N"
 * in observations is rewritten to "#_scxml_{path}". */
#include <stdint.h>
#include <stdlib.h>

#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include "../support/payload_text.h"
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include <tinyactors/system.h>
#include "../support/host_request.h"
#include "../support/host_config.h"

/* ---- charts (@HELLO@ and @LOG:label@ are expanded by expand()) ------------------------ */

// clang-format off
static const char *const CHARTS[][2] = {
{"alpha.scxml",
"<scxml @NS@ name='alpha' initial='top'><datamodel>"
" <data id='path' expr='_name'/><data id='n' expr='0'/><data id='sid' expr='null'/>"
" <data id='items' expr='[1, 2, 3]'/><data id='acc' expr='\"\"'/><data id='it' expr='null'/>"
" <data id='ix' expr='null'/><data id='seen' expr='[]'/></datamodel>"
"<state id='top' initial='run'><onentry>@HELLO@</onentry>"
" <state id='run' initial='p'>"
"  <history id='h' type='deep'><transition target='p'/></history>"
"  <parallel id='p'>"
"   <state id='x' initial='x1'>"
"    <state id='x1'><transition event='a' target='x2'><assign location='n' expr='n + 1'/>@LOG:a@</transition></state>"
"    <state id='x2'><transition event='a' target='x1'>@LOG:a2@</transition>"
"     <transition event='b' cond='n % 2 == 0' target='x1'>@LOG:b@</transition></state>"
"   </state>"
"   <state id='y' initial='y1'>"
"    <state id='y1'><transition event='b' target='y2'/></state>"
"    <state id='y2'><onentry><foreach array='items' item='it' index='ix'>"
"      <assign location='acc' expr='acc + it + ix'/></foreach></onentry>"
"     <transition event='c' target='y1'><script>seen.push(_event.name); items.push(n)</script></transition></state>"
"   </state>"
"   <transition event='d' target='off'>@LOG:d@</transition>"
"  </parallel>"
" </state>"
" <state id='off'><onentry><send event='back' delay='700ms' idlocation='sid'/><log label='off' expr='sid'/></onentry>"
"  <transition event='back' target='h'>@LOG:back@</transition>"
"  <transition event='d' target='h'>@LOG:d2@</transition>"
"  <transition event='c'><cancel sendidexpr='sid'/>@LOG:c@</transition>"
" </state>"
" <transition event='e' cond='In(\"x2\")'><raise event='inner'/>@LOG:e@</transition>"
" <transition event='inner'>@LOG:inner@</transition>"
" <transition event='quit' target='end'/>"
"</state>"
"<final id='end'><donedata><param name='n' expr='n'/></donedata></final></scxml>"},

{"beta.scxml",
"<scxml @NS@ name='beta' binding='late' initial='top'>"
"<state id='top' initial='idle'><datamodel>"
" <data id='path' expr='_name'/><data id='k' expr='0'/><data id='tid' expr='null'/>"
" <data id='got' expr='[]'/></datamodel><onentry>@HELLO@</onentry>"
" <transition event='tick'>@LOG:tick@<assign location='got' expr='got.concat([_event.data])'/></transition>"
" <transition event='tock'>@LOG:tock@</transition>"
" <transition event='quit' target='end'/>"
" <state id='idle'>"
"  <transition event='a' target='armed'/>"
"  <transition event='b'><send event='tick' delay='250ms' idlocation='tid'>"
"    <param name='k' expr='k'/><param name='when' expr='\"b\"'/></send>@LOG:b@</transition>"
"  <transition event='c'><cancel sendidexpr='tid'/>@LOG:c@</transition>"
" </state>"
" <state id='armed'>"
"  <datamodel><data id='armedAt' expr='k * 10'/></datamodel>"
"  <onentry><assign location='k' expr='k + 1'/>"
"   <send id='alarm' event='tock' delayexpr='(300 + 100 * (k % 3)) + \"ms\"'>"
"    <content expr='({k: k, at: armedAt, nested: {list: [k, \"x\"]}})'/></send></onentry>"
"  <transition event='c'><cancel sendid='alarm'/>@LOG:c2@</transition>"
"  <transition event='d' target='idle'/>"
"  <transition event='e'><send target='#_internal' event='self.inner'/>"
"   <send eventexpr='\"ev.\" + k' delay='1s' namelist='k armedAt'/></transition>"
"  <transition event='self.inner'>@LOG:inner@</transition>"
"  <transition event='ev'>@LOG:ev@</transition>"
" </state>"
"</state>"
"<final id='end'/></scxml>"},

{"gamma.scxml",
"<scxml @NS@ name='gamma' initial='top'><datamodel>"
" <data id='path' expr='_name'/><data id='kidID' expr='null'/><data id='nk' expr='0'/>"
" <data id='fromKid' expr='null'/><data id='pid' expr='null'/></datamodel>"
"<state id='top' initial='alone'><onentry>@HELLO@</onentry>"
" <transition event='quit' target='end'/>"
" <transition event='done.invoke'>@LOG:done@</transition>"
" <transition event='pong'>@LOG:pong@</transition>"
" <state id='alone'>"
"  <transition event='kid' target='withKid'><assign location='nk' expr='nk + 1'/></transition>"
"  <transition event='a'>@LOG:a@</transition>"
" </state>"
" <state id='withKid'>"
"  <invoke src='kid.scxml' idlocation='kidID' autoforward='true'>"
"   <param name='path' expr='path + \"/\" + nk'/>"
"   <param name='late' expr='({nk: nk, tag: \"late\"})'/>"
"   <finalize><assign location='fromKid' expr='_event.data'/></finalize>"
"  </invoke>"
"  <transition event='unkid' target='alone'/>"
"  <transition event='ping'><send targetexpr='\"#_\" + kidID' event='ping'><param name='from' expr='path'/></send>@LOG:kid@</transition>"
"  <transition event='later'><send targetexpr='\"#_\" + kidID' event='ping' delay='300ms' idlocation='pid'/>@LOG:later@</transition>"
"  <transition event='c'><cancel sendidexpr='pid'/></transition>"
"  <transition event='done.invoke' target='alone'>@LOG:kiddone@</transition>"
" </state>"
"</state>"
"<final id='end'/></scxml>"},

{"kid.scxml",
"<scxml @NS@ name='kid' binding='late' initial='k'>"
"<state id='k' initial='k1'><datamodel>"
" <data id='path' expr='\"?\"'/><data id='pings' expr='0'/><data id='leafID' expr='null'/>"
" <data id='nl' expr='0'/></datamodel><onentry>@HELLO@<send event='kt' delay='200ms'/></onentry>"
" <transition event='ping'><assign location='pings' expr='pings + 1'/>"
"  <send target='#_parent' event='pong'><param name='pings' expr='pings'/>"
"   <param name='origin' expr='_event.origin'/></send>@LOG:ping@</transition>"
" <transition event='kt'>@LOG:kt@</transition>"
" <transition event='bye' target='kend'/>"
" <state id='k1'><transition event='b' target='k2'/></state>"
" <state id='k2'>"
"  <datamodel><data id='late' expr='\"default\"'/></datamodel>"
"  <onentry><log label='late' expr='path + \" \" + JSON.stringify(late)'/></onentry>"
"  <transition event='b' target='k1'/>"
"  <transition event='deep' target='k3'><assign location='nl' expr='nl + 1'/></transition>"
" </state>"
" <state id='k3'>"
"  <invoke src='leaf.scxml' idlocation='leafID'><param name='path' expr='path + \"/leaf\" + nl'/></invoke>"
"  <transition event='stopleaf'><send targetexpr='\"#_\" + leafID' event='stop'/></transition>"
"  <transition event='done.invoke' target='k2'>@LOG:leafdone@</transition>"
"  <transition event='b' target='k1'/>"
" </state>"
"</state>"
"<final id='kend'><donedata><param name='pings' expr='pings'/><param name='path' expr='path'/></donedata></final></scxml>"},

{"leaf.scxml",
"<scxml @NS@ name='leaf' initial='l'><datamodel>"
" <data id='path' expr='\"?\"'/><data id='t' expr='0'/></datamodel>"
"<state id='l'><onentry>@HELLO@<send event='lt' delay='400ms' id='lt'/></onentry>"
" <transition event='lt'><assign location='t' expr='t + 1'/><send event='lt' delay='400ms' id='lt'/>@LOG:lt@</transition>"
" <transition event='stop' target='lend'/>"
"</state>"
"<final id='lend'><donedata><content expr='t'/></donedata></final></scxml>"},

{"delta.scxml",
"<scxml @NS@ name='delta' initial='top'><datamodel>"
" <data id='path' expr='_name'/><data id='q' expr='0'/><data id='errs' expr='0'/>"
" <data id='hist' expr='[]'/></datamodel>"
"<state id='top' initial='m'><onentry>@HELLO@</onentry>"
" <transition event='error.execution'><assign location='errs' expr='errs + 1'/>@LOG:err@</transition>"
" <transition event='quit' target='end'/>"
" <state id='m' initial='m1'>"
"  <history id='mh' type='shallow'><transition target='m1'/></history>"
"  <state id='m1'><transition event='a' target='m2'/></state>"
"  <state id='m2' initial='m2a'><state id='m2a'><transition event='a' target='m2b'/></state>"
"   <state id='m2b'/><transition event='b' target='m1'/></state>"
"  <transition event='c' target='aside'/>"
" </state>"
" <state id='aside'>"
"  <onentry><script>q = q + 1; hist.push(q)</script></onentry>"
"  <transition cond='q % 3 == 0' target='mh'/>"
"  <transition event='c' target='mh'/>"
"  <transition event='d'><assign location='nosuch.deep' expr='1'/></transition>"
"  <transition event='e'><send targetexpr='\"#_scxml_\" + _sessionid' event='viaSession' delay='150ms'>"
"   <param name='q' expr='q'/></send></transition>"
"  <transition event='viaSession' target='mh'>@LOG:via@</transition>"
" </state>"
"</state>"
"<final id='end'/></scxml>"},

{"epsilon.scxml", /* declares globals: an isolated session (its own realm) */
"<scxml @NS@ name='epsilon' initial='top'><datamodel>"
" <data id='path' expr='_name'/><data id='box' expr='({n: 0})'/></datamodel>"
"<script>var total = 0; let seen = []; const unit = 2; function bump(x) { total = total + x * unit; seen.push(x); return total; }</script>"
"<state id='top' initial='e1'><onentry>@HELLO@</onentry>"
" <transition event='quit' target='end'/>"
" <transition event='a'><script>bump(1); box.n = box.n + 1</script><log label='a' expr='total + \" \" + seen.length + \" \" + box.n'/></transition>"
" <state id='e1'><transition event='b' target='e2'/></state>"
" <state id='e2'><onentry><send event='later' delay='300ms'><param name='t' expr='total'/></send></onentry>"
"  <transition event='later' target='e1'>@LOG:later@<script>bump(_event.data.t)</script></transition>"
"  <transition event='c'><cancel sendid='nothing'/><script>seen = seen.concat([\"c\"])</script></transition></state>"
"</state>"
"<final id='end'/></scxml>"},
};
// clang-format on
#define N_CHARTS (sizeof CHARTS / sizeof CHARTS[0])
static const char *const ROOTS[] = {"alpha", "beta", "gamma", "delta", "epsilon"};

static const char NSATTR[] =
    "xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='ecmascript'";
static const char HELLO[] = "<log label='hello' expr='path + \" \" + _sessionid'/>";
static const char LOGFMT[] =
    "<log label='%.*s' expr='path + \" \" + _event.name + \" \" + JSON.stringify(_event.data) + "
    "\" \" + _event.origin + \" \" + _event.invokeid + \" \" + _event.sendid + \" \" + "
    "_event.type'/>";

static char *expand(const char *source) {
    size_t capacity = strlen(source) * 4 + 4096, n = 0;
    char *out = (char *)malloc(capacity);
    for (const char *p = source; *p;) {
        if (!strncmp(p, "@NS@", 4)) {
            n += (size_t)snprintf(out + n, capacity - n, "%s", NSATTR);
            p += 4;
        } else if (!strncmp(p, "@HELLO@", 7)) {
            n += (size_t)snprintf(out + n, capacity - n, "%s", HELLO);
            p += 7;
        } else if (!strncmp(p, "@LOG:", 5)) {
            const char *e = strchr(p + 5, '@');
            n += (size_t)snprintf(out + n, capacity - n, LOGFMT, (int)(e - p - 5), p + 5);
            p = e + 1;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return out;
}

/* Images A and B: the same charts; B has one more (so another hash): moving
 * a session from A to B goes through ta_blob_migrate. */
static void *test_image_bytes[2];
static ta_image test_images[2];

static int compile_images(void) {
    ta_source source[N_CHARTS + 1];
    char *texts[N_CHARTS + 1];
    for (size_t i = 0; i < N_CHARTS; i++) {
        texts[i] = expand(CHARTS[i][1]);
        source[i].name = (ta_string_view){CHARTS[i][0], (uint32_t)strlen(CHARTS[i][0])};
        source[i].text = (ta_string_view){texts[i], (uint32_t)strlen(texts[i])};
    }
    texts[N_CHARTS] = expand("<scxml @NS@ name='extra'><state id='s'/></scxml>");
    source[N_CHARTS].name = (ta_string_view){"extra.scxml", 11};
    source[N_CHARTS].text = (ta_string_view){texts[N_CHARTS], (uint32_t)strlen(texts[N_CHARTS])};
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
        ta_compile_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        ta_compile_result *result = NULL;
        ta_status status = ta_compile_charts(
            &options, (ta_source_list){source, (uint32_t)N_CHARTS + (uint32_t)k}, &result);
        if (status != TA_OK) {
            print_diagnostics(result);
            ok = 0;
        } else {
            ta_string_view b = ta_compile_image(result);
            test_image_bytes[k] = aligned_alloc(8, (b.length + 7) & ~(size_t)7);
            memcpy(test_image_bytes[k], b.pointer, b.length);
            ok = ta_image_open((ta_bytes){test_image_bytes[k], b.length}, &test_images[k]) == TA_OK;
        }
        ta_compile_free(result);
    }
    for (size_t i = 0; i <= N_CHARTS; i++)
        free(texts[i]);
    return ok;
}

static uint32_t chart_of(int k, const char *name) {
    return ta_image_chart_by_name(&test_images[k], (ta_string_view){name, (uint32_t)strlen(name)});
}

/* ---- a world: systems, a datamodel host, observations ---------------------------------- */

#define MAX_SYSTEMS 16
#define MAX_MAP     1024
#define MAX_OBS     256
#define MAX_HIB     256
#define PATH_MAX_   64

typedef struct string_buffer {
    char *data;
    size_t length, capacity;
} string_buffer;

static void sb_put(string_buffer *b, const char *s, size_t n) {
    if (b->length + n + 1 > b->capacity) {
        b->capacity = (b->length + n + 1) * 2;
        b->data = (char *)realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->length, s, n);
    b->length += n;
    b->data[b->length] = 0;
}
static void string_buffer_format(string_buffer *b, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void string_buffer_format(string_buffer *b, const char *format, ...) {
    char temporary[4096];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(temporary, sizeof temporary, format, ap);
    va_end(ap);
    sb_put(b, temporary, n < (int)sizeof temporary ? (size_t)n : sizeof temporary - 1);
}

typedef struct world {
    const char *label;
    ta_host *host;                  /* the host world: systems are its segments */
    int segment_image[MAX_SYSTEMS]; /* image of each segment */
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    struct {
        void *memory;
        size_t length;
        ta_system *system;
        int image;
    } systems[MAX_SYSTEMS];
    int system_count;
    ta_time now;
    uint64_t next_range;
    struct {
        uint64_t serial;
        char path[PATH_MAX_];
    } map[MAX_MAP];
    int map_count;
    struct {
        char path[PATH_MAX_];
        string_buffer log;
    } observed[MAX_OBS];
    int observed_count;
    struct {
        uint64_t serial;
        void *blob;
        size_t length;
    } hibernations[MAX_HIB];
    int hibernation_count;
    int step;
} world;

static ta_system_config system_config(uint64_t first_serial) {
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.ticks_per_second = 1000;
    config.max_actors = 64;
    config.max_events = 512;
    config.max_timers = 128;
    config.first_serial = first_serial;
    return config;
}

static int system_new(world *w, int k, uint64_t first_serial) {
    int i = w->system_count++;
    ta_system_config config = system_config(first_serial);
    if (ta_system_layout(&test_images[k], &config, &w->systems[i].length) != TA_OK) return -1;
    w->systems[i].memory = aligned_alloc(64, (w->systems[i].length + 63) & ~(size_t)63);
    w->systems[i].image = k;
    if (ta_system_init(&(ta_system_setup){{w->systems[i].memory, w->systems[i].length},
                                          &test_images[k],
                                          &config,
                                          &w->binding,
                                          1,
                                          0},
                       &w->systems[i].system) != TA_OK)
        return -1;
    ta_system_advance_time(w->systems[i].system, w->now);
    return i;
}

static void system_free_all(world *w) {
    for (int i = 0; i < w->system_count; i++) {
        ta_system_deinit(w->systems[i].system);
        free(w->systems[i].memory);
    }
    w->system_count = 0;
}

static void datamodel_new(world *w) {
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
}

static const char *path_of(world *w, uint64_t serial) {
    for (int i = 0; i < w->map_count; i++)
        if (w->map[i].serial == serial) return w->map[i].path;
    return NULL;
}

static string_buffer *obs_of(world *w, const char *path) {
    for (int i = 0; i < w->observed_count; i++)
        if (!strcmp(w->observed[i].path, path)) return &w->observed[i].log;
    if (w->observed_count == MAX_OBS) return NULL;
    int i = w->observed_count++;
    snprintf(w->observed[i].path, sizeof w->observed[i].path, "%s", path);
    memset(&w->observed[i].log, 0, sizeof w->observed[i].log);
    return &w->observed[i].log;
}

/* "#_scxml_<n>" -> "#_scxml_{path}": session ids of spawned sessions are the
 * system's to allocate */
static void canon(world *w, const char *in, size_t n, string_buffer *out) {
    for (size_t i = 0; i < n;) {
        if (n - i > 8 && !memcmp(in + i, "#_scxml_", 8) && in[i + 8] >= '0' && in[i + 8] <= '9') {
            uint64_t v = 0;
            size_t j = i + 8;
            while (j < n && in[j] >= '0' && in[j] <= '9')
                v = v * 10 + (uint64_t)(in[j++] - '0');
            const char *p = path_of(w, v);
            string_buffer_format(out, "#_scxml_{%s}", p ? p : "?");
            i = j;
        } else {
            sb_put(out, in + i, 1);
            i++;
        }
    }
}

static void record(world *w, uint64_t serial, const char *what, size_t n) {
    const char *p = path_of(w, serial);
    string_buffer *o = obs_of(w, p ? p : "?unknown");
    if (!o) return;
    string_buffer_format(o, "%d:", w->step);
    canon(w, what, n, o);
    sb_put(o, ";", 1);
}

static ta_status hib_put(world *w, uint64_t serial, void *blob, size_t length) {
    for (int i = 0; i < w->hibernation_count; i++)
        if (w->hibernations[i].serial == serial) {
            free(w->hibernations[i].blob);
            w->hibernations[i].blob = blob;
            w->hibernations[i].length = length;
            return TA_OK;
        }
    if (w->hibernation_count == MAX_HIB) return TA_ERROR_NO_MEMORY;
    w->hibernations[w->hibernation_count].serial = serial;
    w->hibernations[w->hibernation_count].blob = blob;
    w->hibernations[w->hibernation_count++].length = length;
    return TA_OK;
}

static int wake(world *w, int si, uint32_t actor) {
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    if (ta_actor_info_get(w->systems[si].system, actor, &actor_info) != TA_OK) return 0;
    for (int i = 0; i < w->hibernation_count; i++)
        if (w->hibernations[i].serial == actor_info.serial) {
            ta_status status =
                ta_actor_wake(w->systems[si].system, actor,
                              (ta_bytes){w->hibernations[i].blob, w->hibernations[i].length});
            if (status != TA_OK)
                fprintf(stderr, "  wake %llu: %d\n", (unsigned long long)actor_info.serial, status);
            return status == TA_OK;
        }
    fprintf(stderr, "  no blob for %llu\n", (unsigned long long)actor_info.serial);
    return 0;
}

/* An effect becomes an observation of the session that caused it. */
static void observe_effect(world *w, uint32_t kind, uint64_t session, ta_string_view target,
                           ta_string_view event, ta_string_view aux, ta_payload payload) {
    char buffer[4096];
    int length;
    if (kind == TA_EFFECT_LOG && aux.length == 5 && !memcmp(aux.pointer, "hello", 5)) {
        /* "<path> <sessionid>": names the session */
        const char *sp = memchr(event.pointer, ' ', event.length);
        if (sp && w->map_count < MAX_MAP) {
            int m = w->map_count++;
            snprintf(w->map[m].path, sizeof w->map[m].path, "%.*s", (int)(sp - event.pointer),
                     event.pointer);
            w->map[m].serial = strtoull(sp + 1, NULL, 10);
        }
        length = snprintf(buffer, sizeof buffer, "hello");
    } else if (kind == TA_EFFECT_LOG) {
        length = snprintf(buffer, sizeof buffer, "%.*s=%.*s", (int)aux.length, aux.pointer,
                          (int)event.length, event.pointer ? event.pointer : "");
    } else {
        char data[2048] = "";
        if (payload && payload_text(w->ecmascript, payload, data,
                                                               sizeof data) == (size_t)-1)
            snprintf(data, sizeof data, "<payload?>");
        length = snprintf(buffer, sizeof buffer, "fx%u:%.*s:%.*s:%.*s:%s", kind, (int)target.length,
                          target.pointer ? target.pointer : "", (int)event.length,
                          event.pointer ? event.pointer : "", (int)aux.length,
                          aux.pointer ? aux.pointer : "", data);
    }
    record(w, session, buffer, length < (int)sizeof buffer ? (size_t)length : sizeof buffer - 1);
}

static int host_run(world *w);

/* Run every system until all are idle; effects become observations. */
static int world_run(world *w) {
    if (w->host) return host_run(w);
    for (int round = 0; round < 200; round++) {
        int busy = 0;
        for (int i = 0; i < w->system_count; i++) {
            ta_system *system = w->systems[i].system;
            ta_run result = ta_system_run(system, 1u << 20);
            if (result.status == TA_RUN_FAULT || result.status == TA_RUN_EXHAUSTED) {
                fprintf(stderr, "  %s: run status %u detail %u\n", w->label, result.status,
                        result.detail);
                return 0;
            }
            const ta_effect *effect;
            uint32_t n = ta_system_effects(system, &effect), wakes[64], nw = 0;
            for (uint32_t k = 0; k < n; k++) {
                if (effect[k].kind == TA_EFFECT_WAKE) {
                    if (nw < 64) wakes[nw++] = effect[k].actor;
                    continue;
                }
                observe_effect(w, effect[k].kind, effect[k].session, effect[k].target,
                               effect[k].event, effect[k].auxiliary, effect[k].payload);
            }
            ta_system_effects_ack(system, n);
            for (uint32_t k = 0; k < nw; k++)
                if (!wake(w, i, wakes[k])) return 0;
            if (n || result.status != TA_RUN_IDLE) busy = 1;
        }
        if (!busy) return 1;
    }
    fprintf(stderr, "  %s: does not settle\n", w->label);
    return 0;
}

static uint32_t find_root(world *w, const char *name, int *si) {
    for (int i = 0; i < w->system_count; i++) {
        uint32_t a = ta_system_actor_by_name(w->systems[i].system,
                                             (ta_string_view){name, (uint32_t)strlen(name)});
        if (a) {
            *si = i;
            return a;
        }
    }
    return 0;
}

static void world_send(world *w, const char *root, const char *event_name, const char *json) {
    int si = 0;
    ta_string_view name = {root, (uint32_t)strlen(root)};
    uint64_t session = w->host ? ta_host_actor_by_name(w->host, name) : 0;
    uint32_t a = w->host ? (uint32_t)(session != 0) : find_root(w, root, &si);
    if (!a) return; /* finished */
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){event_name, (uint32_t)strlen(event_name)};
    if (json)
        event.data = ta_datamodel_ecmascript_payload_eval(
            w->ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    event.session = session;
    ta_status status =
        w->host ? ta_host_send(w->host, &event) : ta_system_send(w->systems[si].system, &event);
    CHECK_EQUAL(status, TA_OK);
}

static void world_advance(world *w, ta_time dt) {
    w->now += dt;
    if (w->host) ta_host_advance_time(w->host, (ta_host_time){w->now, 0});
    for (int i = 0; !w->host && i < w->system_count; i++)
        ta_system_advance_time(w->systems[i].system, w->now);
}

/* the host world's live segments as w->systems (for inspection) */
static void host_sync(world *w) {
    w->system_count = 0;
    for (uint32_t i = 0; i < ta_host_segment_count(w->host) && w->system_count < MAX_SYSTEMS; i++) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        if (ta_host_segment_info_get(w->host, i, &segment_info) != TA_OK || !segment_info.live)
            continue;
        w->systems[w->system_count].system = ta_host_system(w->host, i);
        w->systems[w->system_count].memory = NULL;
        w->systems[w->system_count++].image = w->segment_image[i];
    }
}

static int world_spawn(world *w, const char *chart, const char *name) {
    ta_string_view n = {name, (uint32_t)strlen(name)};
    if (w->host) {
        ta_host_chart_ref ref;
        uint64_t session;
        return ta_host_chart(w->host, (ta_string_view){chart, (uint32_t)strlen(chart)}, &ref) ==
                   TA_OK &&
               ta_host_spawn(w->host,
                             &(ta_spawn){ref.segment, ref.chart, n, 0, (ta_string_view){0, 0}},
                             &session) == TA_OK;
    }
    uint32_t a;
    return ta_system_spawn(
               w->systems[0].system,
               &(ta_spawn){0, chart_of(w->systems[0].image, chart), n, 0, (ta_string_view){0, 0}},
               &a) == TA_OK;
}

/* Observable state of every live session, one line each, sorted by path:
 * "path|config|datamodel|_event" ("path|H" while hibernated). */
typedef struct line {
    char path[PATH_MAX_];
    string_buffer text;
} line;

static int by_path(const void *a, const void *b) {
    return strcmp(((const line *)a)->path, ((const line *)b)->path);
}

static int observe(world *w, line *out, int capacity) {
    int n = 0;
    if (w->host) host_sync(w);
    for (int i = 0; i < w->system_count; i++) {
        ta_system *system = w->systems[i].system;
        const ta_image *image = &test_images[w->systems[i].image];
        uint32_t cursor = 0, actors[64], na;
        while ((na = ta_system_actors(system, &cursor, (ta_uint32_array){actors, 64})) != 0) {
            for (uint32_t k = 0; k < na && n < capacity; k++) {
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                if (ta_actor_info_get(system, actors[k], &actor_info) != TA_OK) continue;
                line *l = &out[n++];
                memset(l, 0, sizeof *l);
                const char *p = path_of(w, actor_info.serial);
                snprintf(l->path, sizeof l->path, "%s", p ? p : "?");
                if (actor_info.state == TA_ACTOR_HIBERNATED) {
                    string_buffer_format(&l->text, "H");
                    continue;
                }
                uint32_t states[128],
                    ns = ta_actor_config(system, actors[k], (ta_uint32_array){states, 128});
                for (uint32_t j = 0; j < ns && j < 128; j++) {
                    ta_string_view nm = ta_image_state_name(image, actor_info.chart, states[j]);
                    string_buffer_format(&l->text, "%s%.*s", j ? "," : "", (int)nm.length,
                                         nm.pointer);
                }
                /* every <data>, then _event */
                string_buffer ex = {0};
                string_buffer_format(&ex, "({");
                uint32_t nd = ta_image_data_count(image, actor_info.chart);
                for (uint32_t d = 0; d < nd; d++) {
                    ta_data_info data_info;
                    if (ta_image_data(image, (ta_chart_item){actor_info.chart, d}, &data_info) !=
                        TA_OK)
                        continue;
                    string_buffer_format(&ex, "%s%.*s: %.*s", d ? ", " : "",
                                         (int)data_info.id.length, data_info.id.pointer,
                                         (int)data_info.id.length, data_info.id.pointer);
                }
                string_buffer_format(&ex, "})");
                const char *event = "(typeof _event === 'undefined' ? null : _event)";
                const char *expressions[2] = {ex.data, event};
                for (int e = 0; e < 2; e++) {
                    static char buffer[1 << 16];
                    size_t r = eval_text(
                        w->ecmascript, actor_info.datamodel_instance,
                        (ta_string_view){expressions[e], (uint32_t)strlen(expressions[e])}, buffer,
                        sizeof buffer);
                    sb_put(&l->text, "|", 1);
                    if (r == (size_t)-1)
                        string_buffer_format(&l->text, "<error>");
                    else
                        canon(w, buffer, r < sizeof buffer ? r : sizeof buffer - 1, &l->text);
                }
                free(ex.data);
            }
        }
    }
    qsort(out, (size_t)n, sizeof *out, by_path);
    return n;
}

static void lines_free(line *l, int n) {
    for (int i = 0; i < n; i++)
        free(l[i].text.data);
}

/* ---- the persistence chain ------------------------------------------------------------------- */

/* how often each path ran, over all seeds (the property must not hold vacuously) */
static struct {
    unsigned long moved, migrated, hibernated, woken, host_migrated, host_hibernated;
} ran;

static uint64_t rng_state;
static uint32_t rnd(void) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng_state >> 33);
}

static int wake_all(world *w) {
    for (int i = 0; i < w->system_count; i++) {
        uint32_t cursor = 0, actors[64], na;
        while ((na = ta_system_actors(w->systems[i].system, &cursor,
                                      (ta_uint32_array){actors, 64})) != 0)
            for (uint32_t k = 0; k < na; k++) {
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                if (ta_actor_info_get(w->systems[i].system, actors[k], &actor_info) == TA_OK &&
                    actor_info.state == TA_ACTOR_HIBERNATED && !wake(w, i, actors[k]))
                    return 0;
            }
    }
    return world_run(w);
}

typedef struct exported_blob {
    void *data;
    size_t length;
    int image;
    char chart[32]; /* the root's chart name */
} exported_blob;

/* export every tree -> new datamodel host and systems -> (migrate) -> adopt */
static int move_all(world *w, int *migrated) {
    static exported_blob blobs[64];
    int nb = 0;
    for (int i = 0; i < w->system_count; i++) {
        ta_system *system = w->systems[i].system;
        for (;;) { /* roots, until the system is empty */
            uint32_t cursor = 0, actors[64],
                     na = ta_system_actors(system, &cursor, (ta_uint32_array){actors, 64}),
                     root = 0;
            ta_actor_info actor_info;
            actor_info.size = sizeof actor_info;
            for (uint32_t k = 0; k < na && !root; k++)
                if (ta_actor_info_get(system, actors[k], &actor_info) == TA_OK &&
                    !actor_info.parent)
                    root = actors[k];
            if (!root) {
                if (na) fprintf(stderr, "  sessions without a root are left\n");
                if (na) return 0;
                break;
            }
            ta_string_view cn =
                ta_image_chart_name(&test_images[w->systems[i].image], actor_info.chart);
            snprintf(blobs[nb].chart, sizeof blobs[nb].chart, "%.*s", (int)cn.length, cn.pointer);
            size_t length = 0;
            ta_buffer written = {NULL, 0, 0};
            ta_status status = ta_actor_export(system, root, &written);
            length = written.length;
            if (status != TA_ERROR_NO_MEMORY) {
                fprintf(stderr, "  export (size): %d\n", status);
                return 0;
            }
            blobs[nb].data = aligned_alloc(8, (length + 7) & ~(size_t)7);
            blobs[nb].image = w->systems[i].image;
            written = (ta_buffer){blobs[nb].data, length, 0};
            status = ta_actor_export(system, root, &written);
            blobs[nb].length = written.length;
            if (status != TA_OK) {
                fprintf(stderr, "  export: %d\n", status);
                free(blobs[nb].data);
                return 0;
            }
            nb++;
            ran.moved++;
        }
    }
    /* the old systems and datamodel host are gone before the blobs come back:
     * blobs carry values, not handles */
    system_free_all(w);
    ta_datamodel_ecmascript_free(w->ecmascript);
    datamodel_new(w);
    for (int k = 0; k < 2; k++)
        if (system_new(w, k, ++w->next_range << 32) < 0) return 0;
    *migrated = 0;
    for (int b = 0; b < nb; b++) {
        int to = (int)(rnd() % 2);
        void *p = blobs[b].data, *mig = NULL;
        size_t length = blobs[b].length;
        if (to != blobs[b].image) {
            uint32_t chart = chart_of(to, blobs[b].chart);
            size_t ml = 0;
            ta_buffer translated;
            translated = (ta_buffer){NULL, 0, 0};
            ta_status status = ta_blob_migrate(
                &(ta_blob_migration){
                    &test_images[blobs[b].image], &test_images[to], chart, 0, {p, length}},
                &translated);
            ml = translated.length;
            if (status == TA_ERROR_NO_MEMORY) {
                mig = aligned_alloc(8, (ml + 7) & ~(size_t)7);
                translated = (ta_buffer){mig, ml, 0};
                status = ta_blob_migrate(
                    &(ta_blob_migration){
                        &test_images[blobs[b].image], &test_images[to], chart, 0, {p, length}},
                    &translated);
                ml = translated.length;
            }
            if (status == TA_OK) {
                p = mig;
                length = ml;
                (*migrated)++;
            } else if (status == TA_ERROR_UNSUPPORTED) {
                to = blobs[b].image; /* a session tree moves unchanged */
            } else {
                fprintf(stderr, "  migrate: %d\n", status);
                free(mig);
                for (; b < nb; b++)
                    free(blobs[b].data);
                return 0;
            }
        }
        ta_status status = ta_system_adopt(w->systems[to].system, (ta_bytes){p, length}, NULL);
        free(mig);
        free(blobs[b].data);
        if (status != TA_OK) {
            fprintf(stderr, "  adopt: %d\n", status);
            while (++b < nb)
                free(blobs[b].data);
            return 0;
        }
    }
    return 1;
}

/* hibernate about two thirds of the sessions that can be */
static int hibernate_some(world *w) {
    for (int i = 0; i < w->system_count; i++) {
        ta_system *system = w->systems[i].system;
        uint32_t cursor = 0, actors[64], na;
        while ((na = ta_system_actors(system, &cursor, (ta_uint32_array){actors, 64})) != 0)
            for (uint32_t k = 0; k < na; k++) {
                if (rnd() % 3 == 0) continue;
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                if (ta_actor_info_get(system, actors[k], &actor_info) != TA_OK) return 0;
                size_t length = 0;
                ta_buffer written = {NULL, 0, 0};
                ta_status status = ta_actor_hibernate(system, actors[k], &written);
                length = written.length;
                if (status == TA_ERROR_STATE) continue; /* busy or has invoked children */
                if (status != TA_ERROR_NO_MEMORY) {
                    fprintf(stderr, "  hibernate (size): %d\n", status);
                    return 0;
                }
                void *p = malloc(length);
                written = (ta_buffer){p, length, 0};
                status = ta_actor_hibernate(system, actors[k], &written);
                length = written.length;
                if (status != TA_OK || hib_put(w, actor_info.serial, p, length) != TA_OK) {
                    fprintf(stderr, "  hibernate: %d\n", status);
                    free(p);
                    return 0;
                }
            }
    }
    return 1;
}

/* snapshot every system and the datamodel host; restore all of it into new
 * regions and a new datamodel host */
static int snapshot_restore(world *w) {
    void *snaps[MAX_SYSTEMS];
    size_t lens[MAX_SYSTEMS];
    int images[MAX_SYSTEMS], n = w->system_count;
    for (int i = 0; i < n; i++) {
        ta_buffer written = {NULL, 0, 0};
        if (ta_system_snapshot(w->systems[i].system, &written) != TA_ERROR_NO_MEMORY) return 0;
        lens[i] = written.length;
        snaps[i] = aligned_alloc(64, (lens[i] + 63) & ~(size_t)63);
        written = (ta_buffer){snaps[i], lens[i], 0};
        if (ta_system_snapshot(w->systems[i].system, &written) != TA_OK) return 0;
        lens[i] = written.length;
        images[i] = w->systems[i].image;
    }
    size_t dl = 0;
    ta_buffer written = {NULL, 0, 0};
    ta_status status = ta_datamodel_ecmascript_snapshot(w->ecmascript, &written);
    dl = written.length;
    if (status != TA_ERROR_NO_MEMORY) {
        fprintf(stderr, "  datamodel snapshot: %d %s\n", status,
                ta_datamodel_ecmascript_last_error(w->ecmascript));
        for (int i = 0; i < n; i++)
            free(snaps[i]);
        return 0;
    }
    void *dsnap = malloc(dl ? dl : 1);
    written = (ta_buffer){dsnap, dl, 0};
    if (ta_datamodel_ecmascript_snapshot(w->ecmascript, &written) != TA_OK) return 0;
    dl = written.length;
    system_free_all(w);
    ta_datamodel_ecmascript_free(w->ecmascript);
    datamodel_new(w);
    for (int i = 0; i < n; i++) {
        ta_system_config config = system_config(0);
        w->systems[i].length = lens[i];
        w->systems[i].memory = aligned_alloc(64, (lens[i] + 63) & ~(size_t)63);
        w->systems[i].image = images[i];
        status = ta_system_restore(&(ta_system_setup){{w->systems[i].memory, lens[i]},
                                                      &test_images[images[i]],
                                                      &config,
                                                      &w->binding,
                                                      1,
                                                      0},
                                   (ta_bytes){snaps[i], lens[i]}, &w->systems[i].system);
        free(snaps[i]);
        if (status != TA_OK) {
            fprintf(stderr, "  system restore: %d\n", status);
            return 0;
        }
        w->system_count = i + 1;
    }
    status = ta_datamodel_ecmascript_restore(w->ecmascript, (ta_bytes){dsnap, dl});
    free(dsnap);
    if (status != TA_OK) fprintf(stderr, "  datamodel restore: %d\n", status);
    return status == TA_OK;
}

/* ---- the host world: the same chain through ta_host -------------------------------------------
 */

static test_host_config host_config(world *w) {
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"d", 1};
    config.system = system_config(0);
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    return config;
}

static int host_run(world *w) {
    for (int round = 0; round < 200; round++) {
        ta_run result = ta_host_run(w->host, 1u << 20);
        if (result.status == TA_RUN_FAULT) return 0;
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(w->host, &effects), nw = 0;
        uint64_t wakes[64];
        for (uint32_t k = 0; k < n; k++) {
            const ta_host_effect *effect = &effects[k];
            if (effect->kind == TA_EFFECT_WAKE && nw < 64) wakes[nw++] = effect->session;
            if (effect->kind == TA_EFFECT_WAKE || effect->kind >= TA_HOST_EFFECT_MIGRATED) continue;
            observe_effect(w, effect->kind, effect->session, effect->target, effect->event,
                           effect->auxiliary, effect->payload);
        }
        ta_host_effects_ack(w->host);
        for (uint32_t k = 0; k < nw; k++) {
            int found = 0;
            for (int i = 0; i < w->hibernation_count && !found; i++)
                if (w->hibernations[i].serial == wakes[k]) {
                    found = 1;
                    ta_status status = ta_host_wake(
                        w->host, wakes[k],
                        (ta_bytes){w->hibernations[i].blob, w->hibernations[i].length});
                    if (status != TA_OK) {
                        fprintf(stderr, "  host wake %llu: %d\n", (unsigned long long)wakes[k],
                                status);
                        return 0;
                    }
                }
            if (!found) return 0;
        }
        ta_host_stats stats;
        stats.size = sizeof stats;
        CHECK_EQUAL(host_stats_fill(w->host, &stats), TA_OK);
        if (!n && result.status == TA_RUN_IDLE && !stats.migrations) return 1;
    }
    fprintf(stderr, "  %s: does not settle\n", w->label);
    return 0;
}

static int host_wake_all(world *w) {
    host_sync(w);
    for (int i = 0; i < w->system_count; i++) {
        uint32_t cursor = 0, actors[64], na;
        while ((na = ta_system_actors(w->systems[i].system, &cursor,
                                      (ta_uint32_array){actors, 64})) != 0)
            for (uint32_t k = 0; k < na; k++) {
                ta_actor_info actor_info;
                actor_info.size = sizeof actor_info;
                if (ta_actor_info_get(w->systems[i].system, actors[k], &actor_info) != TA_OK ||
                    actor_info.state != TA_ACTOR_HIBERNATED)
                    continue;
                int found = 0;
                for (int b = 0; b < w->hibernation_count && !found; b++)
                    if (w->hibernations[b].serial == actor_info.serial)
                        found = ta_host_wake(w->host, actor_info.serial,
                                             (ta_bytes){w->hibernations[b].blob,
                                                        w->hibernations[b].length}) == TA_OK;
                if (!found) return 0;
            }
    }
    return host_run(w);
}

/* a new version (the other image) and a migration to it */
static int host_migrate(world *w) {
    uint32_t segment;
    int k = w->segment_image[ta_host_segment_count(w->host) - 1] ^ 1;
    if (ta_host_segment_count(w->host) >= MAX_SYSTEMS ||
        ta_host_add_image(w->host, ta_image_bytes(&test_images[k]), &segment) != TA_OK)
        return 0;
    w->segment_image[segment] = k;
    /* one migration per root without running invocations (a session tree waits for
     * its invocations to finish: it would keep the migration open) */
    uint64_t cursor = 0, sessions[64];
    uint32_t n;
    while ((n = host_actors_page(w->host, &cursor, (ta_uint64_array){sessions, 64})) != 0)
        for (uint32_t i = 0; i < n; i++) {
            ta_host_location location;
            uint32_t migration;
            ta_actor_info actor_info;
            actor_info.size = sizeof actor_info;
            if (ta_host_locate(w->host, sessions[i], &location) != TA_OK ||
                location.segment == segment ||
                ta_actor_info_get(ta_host_system(w->host, location.segment), location.actor,
                                  &actor_info) != TA_OK ||
                actor_info.parent || actor_info.invocations)
                continue;
            if (ta_host_migrate(w->host,
                                &(ta_host_migration){(ta_string_view){0, 0}, sessions[i],
                                                     TA_HOST_MIGRATE_WAKE, 0, 0},
                                &migration) != TA_OK)
                return 0;
        }
    return host_run(w);
}

static int host_hibernate_some(world *w) {
    uint64_t cursor = 0, sessions[64];
    uint32_t n;
    while ((n = host_actors_page(w->host, &cursor, (ta_uint64_array){sessions, 64})) != 0)
        for (uint32_t k = 0; k < n; k++) {
            if (rnd() % 3 == 0) continue;
            ta_bytes written; /* the host's, until the ack */
            ta_status status = ta_host_hibernate(w->host, sessions[k], &written);
            if (status == TA_ERROR_STATE) continue;
            if (status != TA_OK) return 0;
            void *p = malloc(written.length ? written.length : 1);
            memcpy(p, written.pointer, written.length);
            ta_host_effects_ack(w->host); /* the output is copied: its batch ends */
            if (hib_put(w, sessions[k], p, written.length) != TA_OK) {
                free(p);
                return 0;
            }
        }
    return 1;
}

static int host_snapshot_restore(world *w) {
    size_t hl = 0, dl = 0;
    ta_buffer written = {NULL, 0, 0};
    if (ta_host_snapshot(w->host, &written) != TA_ERROR_NO_MEMORY) return 0;
    hl = written.length;
    void *hs = aligned_alloc(8, (hl + 7) & ~(size_t)7);
    written = (ta_buffer){hs, hl, 0};
    if (ta_host_snapshot(w->host, &written) != TA_OK) return 0;
    hl = written.length;
    written = (ta_buffer){NULL, 0, 0};
    if (ta_datamodel_ecmascript_snapshot(w->ecmascript, &written) != TA_ERROR_NO_MEMORY) return 0;
    dl = written.length;
    void *ds = malloc(dl ? dl : 1);
    written = (ta_buffer){ds, dl, 0};
    if (ta_datamodel_ecmascript_snapshot(w->ecmascript, &written) != TA_OK) return 0;
    dl = written.length;
    ta_bytes images[MAX_SYSTEMS];
    uint32_t ns = ta_host_segment_count(w->host);
    for (uint32_t i = 0; i < ns; i++) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        ta_host_segment_info_get(w->host, i, &segment_info);
        images[i] = segment_info.live ? ta_image_bytes(&test_images[w->segment_image[i]])
                                      : (ta_bytes){NULL, 0};
    }
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    datamodel_new(w);
    test_host_config config = host_config(w);
    ta_status status =
        test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, images, ns, 0}, &w->host);
    if (status == TA_OK)
        status = ta_datamodel_ecmascript_restore(w->ecmascript, (ta_bytes){ds, dl});
    free(hs);
    free(ds);
    if (status != TA_OK) fprintf(stderr, "  host restore: %d\n", status);
    return status == TA_OK;
}

static void world_init_host(world *w, const char *label) {
    memset(w, 0, sizeof *w);
    w->label = label;
    datamodel_new(w);
    test_host_config config = host_config(w);
    uint32_t segment;
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    CHECK_EQUAL(ta_host_add_image(w->host, ta_image_bytes(&test_images[0]), &segment), TA_OK);
    w->segment_image[segment] = 0;
    ta_host_advance_time(w->host, (ta_host_time){0, 0});
}

static void world_init(world *w, const char *label) {
    memset(w, 0, sizeof *w);
    w->label = label;
    datamodel_new(w);
    system_new(w, 0, 0);
}

static void world_free(world *w) {
    if (w->host)
        ta_host_free(w->host);
    else
        system_free_all(w);
    ta_datamodel_ecmascript_free(w->ecmascript);
    for (int i = 0; i < w->observed_count; i++)
        free(w->observed[i].log.data);
    for (int i = 0; i < w->hibernation_count; i++)
        free(w->hibernations[i].blob);
}

/* ---- comparing --------------------------------------------------------------------------------
 */

static const char *UPGRADE = "\"name\":\"error.platform.upgrade\"";

/* the datamodel and _event parts of a line ("config|data|event") */
static int same_state(const char *ref, const char *per) {
    if (!strcmp(ref, per)) return 1;
    /* a migrated session processed error.platform.upgrade (its documented
     * effect): _event is that event until the next one comes */
    const char *rb = strrchr(ref, '|'), *pb = strrchr(per, '|');
    return rb && pb && strstr(pb, UPGRADE) && rb - ref == pb - per &&
           !strncmp(ref, per, (size_t)(rb - ref));
}

static int compare(world *ref, world *per, int seed, const char *trail) {
    static line lr[128], lp[128];
    int nr = observe(ref, lr, 128), np = observe(per, lp, 128), ok = nr == np;
    for (int i = 0; ok && i < nr; i++) {
        ok = !strcmp(lr[i].path, lp[i].path) &&
             (!strcmp(lp[i].text.data ? lp[i].text.data : "", "H") ||
              same_state(lr[i].text.data ? lr[i].text.data : "",
                         lp[i].text.data ? lp[i].text.data : ""));
        if (!ok)
            fprintf(stderr, "seed %d step %d: state of %s differs\n  reference: %s\n  %s: %s\n",
                    seed, ref->step, lr[i].path, lr[i].text.data, per->label, lp[i].text.data);
    }
    if (nr != np) {
        fprintf(stderr, "seed %d step %d: %d vs %d sessions\n", seed, ref->step, nr, np);
        for (int i = 0; i < nr; i++)
            fprintf(stderr, "  reference %s\n", lr[i].path);
        for (int i = 0; i < np; i++)
            fprintf(stderr, "  persisted %s\n", lp[i].path);
    }
    lines_free(lr, nr);
    lines_free(lp, np);
    for (int i = 0; ok && i < ref->observed_count; i++) {
        string_buffer *o = obs_of(per, ref->observed[i].path);
        const char *a = ref->observed[i].log.data ? ref->observed[i].log.data : "",
                   *b = o && o->data ? o->data : "";
        if (strcmp(a, b)) {
            size_t d = 0;
            while (a[d] && a[d] == b[d])
                d++;
            size_t from = d > 200 ? d - 200 : 0;
            fprintf(stderr,
                    "seed %d step %d: log of %s differs at %zu\n  reference: ...%s\n"
                    "  %s: ...%s\n",
                    seed, ref->step, ref->observed[i].path, d, a + from, per->label, b + from);
            ok = 0;
        }
    }
    if (ok && ref->observed_count != per->observed_count) {
        fprintf(stderr, "seed %d step %d: %d vs %d logs\n", seed, ref->step, ref->observed_count,
                per->observed_count);
        ok = 0;
    }
    if (!ok) fprintf(stderr, "  inputs: %s\n", trail);
    if (!ok && getenv("TA_DURABLE_SEED"))
        for (int i = 0; i < ref->observed_count; i++) {
            string_buffer *o = obs_of(per, ref->observed[i].path);
            fprintf(stderr, "  %s reference: %s\n  %s persisted: %s\n", ref->observed[i].path,
                    ref->observed[i].log.data, ref->observed[i].path, o ? o->data : "");
        }
    return ok;
}

/* ---- building blocks, one deterministic test each ------------------------------------------ */

/* One chart, one system on an ECMAScript datamodel host of its own. */
typedef struct mini {
    void *bytes;
    ta_image image;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    void *memory;
    ta_system *system;
    ta_time now;
    char log[4096];
    void *hibernations;
    size_t hibernation_length;
} mini;

static int mini_system(mini *m, uint64_t first_serial) {
    ta_system_config config = system_config(first_serial);
    size_t length;
    if (ta_system_layout(&m->image, &config, &length) != TA_OK) return 0;
    m->memory = aligned_alloc(64, (length + 63) & ~(size_t)63);
    if (ta_system_init(
            &(ta_system_setup){{m->memory, length}, &m->image, &config, &m->binding, 1, 0},
            &m->system) != TA_OK)
        return 0;
    ta_system_advance_time(m->system, m->now);
    return 1;
}

static int mini_open(mini *m, const char *chart) {
    memset(m, 0, sizeof *m);
    char *text = expand(chart);
    size_t length;
    m->bytes = compile_chart("m.scxml", text, NULL, &length, 0);
    free(text);
    if (!m->bytes || ta_image_open((ta_bytes){m->bytes, length}, &m->image) != TA_OK) return 0;
    m->ecmascript = ta_datamodel_ecmascript_new(NULL);
    m->binding = ta_datamodel_ecmascript_binding(m->ecmascript);
    return mini_system(m, 0);
}

/* the system and the datamodel host go; new ones come (blobs must carry values) */
static int mini_new_host(mini *m, uint64_t first_serial) {
    ta_system_deinit(m->system);
    free(m->memory);
    ta_datamodel_ecmascript_free(m->ecmascript);
    m->ecmascript = ta_datamodel_ecmascript_new(NULL);
    m->binding = ta_datamodel_ecmascript_binding(m->ecmascript);
    return mini_system(m, first_serial);
}

static void mini_close(mini *m) {
    if (m->system) ta_system_deinit(m->system);
    free(m->memory);
    if (m->ecmascript) ta_datamodel_ecmascript_free(m->ecmascript);
    free(m->bytes);
    free(m->hibernations);
}

/* run until idle; logs as "text;", TA_EFFECT_WAKE answered from m->hibernations */
static void mini_run(mini *m) {
    for (int i = 0; i < 20; i++) {
        ta_run result = ta_system_run(m->system, 1u << 20);
        const ta_effect *effect;
        uint32_t n = ta_system_effects(m->system, &effect), wake = 0;
        for (uint32_t k = 0; k < n; k++) {
            size_t l = strlen(m->log);
            if (effect[k].kind == TA_EFFECT_LOG)
                snprintf(m->log + l, sizeof m->log - l, "%.*s;", (int)effect[k].event.length,
                         effect[k].event.pointer ? effect[k].event.pointer : "");
            if (effect[k].kind == TA_EFFECT_WAKE) wake = effect[k].actor;
        }
        ta_system_effects_ack(m->system, n);
        if (wake)
            CHECK_EQUAL(
                ta_actor_wake(m->system, wake, (ta_bytes){m->hibernations, m->hibernation_length}),
                TA_OK);
        if (!n && !wake && result.status == TA_RUN_IDLE) return;
    }
}

static void mini_send(mini *m, uint32_t a, const char *event_name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){event_name, (uint32_t)strlen(event_name)};
    CHECK_EQUAL(ta_system_send(m->system, &event), TA_OK);
    mini_run(m);
}

static void mini_advance(mini *m, ta_time dt) {
    m->now += dt;
    ta_system_advance_time(m->system, m->now);
    mini_run(m);
}

static uint32_t mini_spawn(mini *m, const char *name) {
    uint32_t a = 0;
    CHECK_EQUAL(ta_system_spawn(m->system,
                                &(ta_spawn){0, 0, (ta_string_view){name, (uint32_t)strlen(name)}, 0,
                                            (ta_string_view){0, 0}},
                                &a),
                TA_OK);
    mini_run(m);
    return a;
}

static void mini_hibernate(mini *m, uint32_t a) {
    size_t length = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_actor_hibernate(m->system, a, &written), TA_ERROR_NO_MEMORY);
    length = written.length;
    free(m->hibernations);
    m->hibernations = malloc(length);
    written = (ta_buffer){m->hibernations, length, 0};
    CHECK_EQUAL(ta_actor_hibernate(m->system, a, &written), TA_OK);
    m->hibernation_length = written.length;
}

/* export a root, replace system and datamodel host, adopt */
static uint32_t mini_move(mini *m, uint32_t a, uint64_t first_serial) {
    size_t length = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_actor_export(m->system, a, &written), TA_ERROR_NO_MEMORY);
    length = written.length;
    void *b = malloc(length);
    written = (ta_buffer){b, length, 0};
    CHECK_EQUAL(ta_actor_export(m->system, a, &written), TA_OK);
    length = written.length;
    CHECK(mini_new_host(m, first_serial));
    uint32_t na = 0;
    CHECK_EQUAL(ta_system_adopt(m->system, (ta_bytes){b, length}, &na), TA_OK);
    free(b);
    return na;
}

#define CHECK_LOG(m, want)                                                                         \
    do {                                                                                           \
        CHECK(strcmp((m)->log, (want)) == 0);                                                      \
        if (strcmp((m)->log, (want)))                                                              \
            fprintf(stderr, "  log \"%s\", want \"%s\"\n", (m)->log, want);                        \
    } while (0)

// clang-format off
static const char KID_GONE[] =
"<scxml @NS@ name='p' initial='s'><datamodel><data id='kid' expr='null'/></datamodel>"
"<state id='s' initial='with'>"
" <transition event='error.communication'><log expr='\"err \" + _event.sendid'/></transition>"
" <state id='with'>"
"  <invoke idlocation='kid' autoforward='true'><content><scxml @NS@ initial='k'>"
"   <state id='k'><transition event='bye' target='f'/></state><final id='f'/></scxml></content></invoke>"
"  <transition event='later'><send targetexpr='\"#_\" + kid' event='hi' delay='100ms' id='late'/></transition>"
"  <transition event='done.invoke' target='alone'/>"
" </state>"
" <state id='alone'/>"
"</state></scxml>";
// clang-format on

/* A delayed send whose target is gone by the time it fires raises
 * error.communication in its sender at once — also when the sender is idle
 * (it used to wait for the next event, and the session could not be stored
 * meanwhile) or hibernated (it used to be written into an instance the actor
 * no longer had). */
static void test_failed_delayed_send(void) {
    for (int hibernated = 0; hibernated < 2; hibernated++) {
        mini m;
        CHECK(mini_open(&m, KID_GONE));
        uint32_t a = mini_spawn(&m, "p");
        mini_send(&m, a, "later");
        mini_send(&m, a, "bye"); /* the child finishes; the send to it is still pending */
        if (hibernated) mini_hibernate(&m, a);
        mini_advance(&m, 100);
        CHECK_LOG(&m, "err late;");
        ta_actor_info actor_info;
        actor_info.size = sizeof actor_info;
        CHECK_EQUAL(ta_actor_info_get(m.system, a, &actor_info), TA_OK);
        CHECK_EQUAL(actor_info.state, TA_ACTOR_IDLE);
        CHECK_EQUAL(ta_actor_export(m.system, a, &(ta_buffer){NULL, 0, 0}),
                    TA_ERROR_NO_MEMORY); /* storable */
        mini_close(&m);
    }
}

// clang-format off
static const char IDS_CHART[] =
"<scxml @NS@ name='i' initial='s'><datamodel><data id='sid' expr='null'/></datamodel>"
"<state id='s'><transition event='go'><send event='x' delay='1s' idlocation='sid'/>"
"<log expr='sid'/></transition></state></scxml>";
// clang-format on

/* Generated ids are session state: they depend neither on other sessions nor
 * on the system the session lives in (a system-wide counter made them both). */
static void test_ids_are_session_state(void) {
    mini m;
    CHECK(mini_open(&m, IDS_CHART));
    uint32_t a = mini_spawn(&m, "A"), b = mini_spawn(&m, "B");
    mini_send(&m, a, "go");
    mini_send(&m, b, "go");
    mini_send(&m, b, "go");
    mini_send(&m, a, "go");
    CHECK_LOG(&m, "send.1;send.1;send.2;send.2;");
    m.log[0] = 0;
    a = mini_move(&m, a, 1ull << 32);
    uint32_t c = mini_spawn(&m, "C");
    mini_send(&m, c, "go");
    mini_send(&m, a, "go");
    CHECK_LOG(&m, "send.1;send.3;");
    mini_close(&m);
}

// clang-format off
static const char PAYLOAD_CHART[] =
"<scxml @NS@ name='q' initial='s'><state id='s'>"
" <onentry><send event='t' delay='1s'><content expr='({d: new Date(0), m: new Map([[1, [2]]]), s: \"x\"})'/></send></onentry>"
" <transition event='t'><log expr='_event.data.d.getTime() + \" \" + _event.data.m.get(1)[0] + \" \" + _event.data.s'/></transition>"
"</state></scxml>";
// clang-format on

/* A delayed send's payload travels in the export blob as a value: the blob is
 * adopted by another datamodel host (the writing one is gone). */
static void test_payload_is_a_value(void) {
    mini m;
    CHECK(mini_open(&m, PAYLOAD_CHART));
    uint32_t a = mini_spawn(&m, "q");
    a = mini_move(&m, a, 1ull << 32);
    mini_advance(&m, 1000);
    CHECK_LOG(&m, "0 2 x;");
    mini_close(&m);
}

// clang-format off
static const char PARAMS_CHART[] =
"<scxml @NS@ name='p' initial='s'><state id='s'>"
" <invoke autoforward='true'><param name='late' expr='({from: \"parent\"})'/><content>"
"  <scxml @NS@ binding='late' initial='k1'>"
"   <state id='k1'><transition event='b' target='k2'/></state>"
"   <state id='k2'><datamodel><data id='late' expr='\"default\"'/></datamodel>"
"    <onentry><log expr='JSON.stringify(late)'/></onentry></state></scxml></content></invoke>"
"</state></scxml>";
// clang-format on

/* Late binding applies <invoke> params to <data> of states entered later:
 * they are part of the session's state (hibernation and export keep them). */
static void test_invoke_params_survive(void) {
    for (int path = 0; path < 2; path++) {
        mini m;
        CHECK(mini_open(&m, PARAMS_CHART));
        uint32_t a = mini_spawn(&m, "p");
        if (path == 0) { /* hibernate the child */
            uint32_t cursor = 0, actors[4],
                     n = ta_system_actors(m.system, &cursor, (ta_uint32_array){actors, 4});
            CHECK_EQUAL(n, 2);
            for (uint32_t k = 0; k < n; k++)
                if (actors[k] != a) mini_hibernate(&m, actors[k]);
        } else { /* export the tree to another datamodel host */
            a = mini_move(&m, a, 1ull << 32);
        }
        mini_send(&m, a, "b");
        CHECK_LOG(&m, "{\"from\":\"parent\"};");
        mini_close(&m);
    }
}

// clang-format off
static const char ORDER_CHART[] =
"<scxml @NS@ name='o' initial='s'><state id='s'>"
" <invoke><content><scxml @NS@ initial='k'><state id='k'>"
"  <onentry><send event='c' delay='1s'/></onentry>"
"  <transition event='c'><log expr='\"child\"'/></transition></state></scxml></content></invoke>"
" <transition event='go'><send event='p' delay='1s'/></transition>"
" <transition event='p'><log expr='\"parent\"'/></transition>"
"</state></scxml>";
// clang-format on

/* Delayed sends of a session tree due at the same tick fire in the order they
 * were made, also after the tree moved (each records its rank). */
static void test_tree_timers_keep_order(void) {
    for (int moved = 0; moved < 2; moved++) {
        mini m;
        CHECK(mini_open(&m, ORDER_CHART));
        uint32_t a = mini_spawn(&m, "o"); /* the child's send is made now */
        mini_send(&m, a, "go");           /* then the parent's, due at the same tick */
        if (moved) a = mini_move(&m, a, 1ull << 32);
        mini_advance(&m, 1000);
        CHECK_LOG(&m, "child;parent;");
        mini_close(&m);
    }
}

/* ---- the property -------------------------------------------------------------------------------
 */

static const char *const EVENTS[] = {"a",    "b",     "c",    "d",    "e",        "kid",
                                     "ping", "later", "deep", "bye",  "stopleaf", "unkid",
                                     "a",    "b",     "c",    "deep", "kid",      "later"};
static const ta_time DELAYS[] = {0, 50, 100, 150, 200, 250, 300, 400, 700, 1000, 2000};

static int run_seed(int seed, int steps) {
    world *ref = (world *)calloc(1, sizeof *ref), *per = (world *)calloc(1, sizeof *per),
          *hst = (world *)calloc(1, sizeof *hst), *worlds[3] = {ref, per, hst};
    world_init(ref, "reference");
    world_init(per, "persisted");
    world_init_host(hst, "host");
    rng_state = (uint64_t)seed * 0x9E3779B97F4A7C15ull + 1;
    int root_count = 2 + (int)(rnd() % 3), ok = 1;
    static char trail[1 << 15];
    trail[0] = 0;
    size_t tl = 0;
    char names[4][16];
    for (int r = 0; r < root_count; r++) {
        const char *chart = ROOTS[rnd() % (sizeof ROOTS / sizeof *ROOTS)];
        snprintf(names[r], sizeof names[r], "R%d", r);
        tl += (size_t)snprintf(trail + tl, sizeof trail - tl, "%s=%s ", names[r], chart);
        for (int k = 0; k < 3; k++)
            CHECK(world_spawn(worlds[k], chart, names[r]));
    }
    for (int k = 0; k < 3 && ok; k++)
        ok = world_run(worlds[k]);
    ok = ok && compare(ref, per, seed, trail) && compare(ref, hst, seed, trail);
    for (int step = 1; ok && step <= steps; step++) {
        for (int k = 0; k < 3; k++)
            worlds[k]->step = step;
        if (rnd() % 10 < 6) {
            const char *root = names[rnd() % (uint32_t)root_count];
            const char *event =
                rnd() % 60 == 0 ? "quit" : EVENTS[rnd() % (sizeof EVENTS / sizeof *EVENTS)];
            char json[32];
            int with = rnd() % 2;
            snprintf(json, sizeof json, "{\"v\": %u}", rnd() % 100);
            for (int k = 0; k < 3; k++)
                world_send(worlds[k], root, event, with ? json : NULL);
            tl += (size_t)snprintf(trail + tl, sizeof trail - tl, "%d:%s<-%s%s ", step, root, event,
                                   with ? json : "");
        } else {
            ta_time dt = DELAYS[rnd() % (sizeof DELAYS / sizeof *DELAYS)];
            for (int k = 0; k < 3; k++)
                world_advance(worlds[k], dt);
            tl += (size_t)snprintf(trail + tl, sizeof trail - tl, "%d:+%llu ", step,
                                   (unsigned long long)dt);
        }
        if (tl > sizeof trail - 256) tl = 0;
        for (int k = 0; k < 3 && ok; k++)
            ok = world_run(worlds[k]);
        ok = ok && compare(ref, per, seed, trail) && compare(ref, hst, seed, trail);
        /* the persistence chains, every step: systems directly ... */
        int migrated = 0;
        if (ok) ok = wake_all(per) && compare(ref, per, seed, trail);
        if (ok) ok = move_all(per, &migrated);
        ran.migrated += (unsigned long)migrated;
        if (ok) ok = hibernate_some(per);
        if (ok) ok = snapshot_restore(per);
        /* ... and through the host: a new version every third step */
        if (ok) ok = host_wake_all(hst) && compare(ref, hst, seed, trail);
        if (ok && step % 3 == 0) ok = host_migrate(hst);
        if (ok) ok = host_hibernate_some(hst);
        if (ok) ok = host_snapshot_restore(hst);
        if (!ok)
            fprintf(stderr, "seed %d step %d: persistence failed\n  inputs: %s\n", seed, step,
                    trail);
    }
    host_sync(hst);
    for (int k = 1; k < 3; k++)
        for (int i = 0; i < worlds[k]->system_count; i++) {
            ta_system_stats stats;
            stats.size = sizeof stats;
            ta_system_stats_get(worlds[k]->systems[i].system, &stats);
            ran.woken += stats.wakes;
        }
    {
        ta_host_stats host_stats;
        host_stats.size = sizeof host_stats;
        CHECK_EQUAL(host_stats_fill(hst->host, &host_stats), TA_OK);
        ran.host_migrated += host_stats.migrated;
    }
    ran.hibernated += (unsigned long)per->hibernation_count;
    ran.host_hibernated += (unsigned long)hst->hibernation_count;
    for (int k = 0; k < 3; k++) {
        world_free(worlds[k]);
        free(worlds[k]);
    }
    return ok;
}

static void test_every_path_preserves_durable_state(void) {
    CHECK(compile_images());
    const char *environment = getenv("TA_DURABLE_SEEDS"), *one = getenv("TA_DURABLE_SEED");
    int seeds = environment ? atoi(environment) : 24, failed = 0;
    for (int seed = one ? atoi(one) : 0; seed < (one ? atoi(one) + 1 : seeds) && failed < 3; seed++)
        if (!run_seed(seed, 40)) failed++;
    CHECK_EQUAL(failed, 0);
    fprintf(stderr,
            "  paths: %lu exported, %lu migrated, %lu hibernating sessions, %lu wakes in "
            "the last systems; host: %lu migrated, %lu hibernating sessions\n",
            ran.moved, ran.migrated, ran.hibernated, ran.woken, ran.host_migrated,
            ran.host_hibernated);
    CHECK(ran.moved && ran.migrated && ran.hibernated && ran.host_migrated && ran.host_hibernated);
    free(test_image_bytes[0]);
    free(test_image_bytes[1]);
}

int main(void) {
    RUN(test_failed_delayed_send);
    RUN(test_ids_are_session_state);
    RUN(test_payload_is_a_value);
    RUN(test_invoke_params_survive);
    RUN(test_tree_timers_keep_order);
    RUN(test_every_path_preserves_durable_state);
    TEST_MAIN_END();
}

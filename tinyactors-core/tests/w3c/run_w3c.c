/* W3C SCXML IRP runner.
 *
 * Usage: run_w3c <converted-dir> [options]
 *   --dm ecmascript|test   datamodel (default: test = small subset in tests/support)
 *   --realm isolated       ecmascript: one JS context per session (default: shared realm)
 *   --min N                fail unless >= N mandatory tests pass
 *   --skip FILE            ids listed there (with reasons) are not run
 *   --require-all          fail if any non-skipped test fails
 *   --only ID              run one test verbosely
 * Each test (plus its dependency documents, linked into the same image) is
 * compiled from disk, run under simulated time, and judged by the "Outcome"
 * log the W3C stylesheet places in the pass/fail final states. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../support/datamodel_test.h"
#include <tinyactors/datamodel_ecmascript.h>
#include "../support/payload_text.h"
#include <tinyactors/basic_http.h>
#include <tinyactors/runtime.h>

typedef enum { R_PASS, R_FAIL, R_COMPILE, R_TIMEOUT } test_outcome;
static const char *rname[] = {"pass", "FAIL", "COMPILE", "TIMEOUT"};

static int use_ecmascript;
static uint32_t ecmascript_realm; /* TA_DATAMODEL_ECMASCRIPT_REALM_* */

/* ---- BasicHTTP loopback host ---------------------------------------------------------
 * The runner plays the host: it owns the BasicHTTP "transport". Sends to our
 * own location are encoded as real HTTP POST messages (tinyactors/basic_http.h), then
 * parsed back as incoming requests and delivered to the sender; anything
 * else is unreachable (error.communication). */
#define HTTP_LOC "http://localhost:8080/scxml"

typedef struct pending {
    uint32_t actor;
    int error;
    char sendid[128];
    char message[8192];
    size_t message_length;
} pending;

/* Flat JSON object -> form fields (values stringified). */
static uint32_t json_fields(const char *j, ta_http_field *field, uint32_t capacity, char *store,
                            size_t scap) {
    uint32_t n = 0;
    size_t used = 0;
    const char *p = j;
    while (*p && *p != '{')
        p++;
    if (*p != '{') return 0;
    p++;
    while (*p && n < capacity) {
        while (*p == ' ' || *p == ',')
            p++;
        if (*p != '"') break;
        const char *ks = ++p;
        while (*p && *p != '"')
            p++;
        size_t kl = (size_t)(p - ks);
        if (*p) p++;
        while (*p == ' ' || *p == ':')
            p++;
        const char *vs = p;
        int depth = 0, instr = 0;
        while (*p && (depth || instr || (*p != ',' && *p != '}'))) {
            if (instr) {
                if (*p == '\\' && p[1])
                    p++;
                else if (*p == '"')
                    instr = 0;
            } else if (*p == '"')
                instr = 1;
            else if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']')
                depth--;
            p++;
        }
        size_t vl = (size_t)(p - vs);
        if (vl >= 2 && vs[0] == '"') { /* unquote simple strings */
            vs++;
            vl -= 2;
        }
        if (used + kl + vl + 2 > scap) break;
        memcpy(store + used, ks, kl);
        field[n].name = (ta_string_view){store + used, (uint32_t)kl};
        used += kl;
        memcpy(store + used, vs, vl);
        field[n].value = (ta_string_view){store + used, (uint32_t)vl};
        used += vl;
        n++;
    }
    return n;
}

static void http_send(ta_datamodel_ecmascript *ecmascript, const ta_effect *effect, pending *pd) {
    memset(pd, 0, sizeof *pd);
    pd->actor = effect->actor;
    snprintf(pd->sendid, sizeof pd->sendid, "%.*s", (int)effect->sendid.length,
             effect->sendid.pointer ? effect->sendid.pointer : "");
    size_t ll = strlen(HTTP_LOC);
    if (!effect->target.length || effect->target.length < ll ||
        memcmp(effect->target.pointer, HTTP_LOC, ll) != 0) {
        pd->error = 1; /* no or unreachable target */
        return;
    }
    char json[4096] = "";
    if (effect->payload && ecmascript)
        payload_text(ecmascript, effect->payload, json, sizeof json);
    ta_http_field fields[32];
    char store[4096];
    uint32_t nf = json[0] == '{' ? json_fields(json, fields, 32, store, sizeof store) : 0;
    ta_string_view body = {"", 0};
    char text[4096];
    if (json[0] == '"') { /* <content> string */
        size_t l = strlen(json);
        snprintf(text, sizeof text, "%.*s", (int)(l - 2), json + 1);
        body = (ta_string_view){text, (uint32_t)strlen(text)};
    }
    ta_http_post post = {effect->target, effect->event, fields, nf, 0, body};
    ta_buffer out = {pd->message, sizeof pd->message, 0};
    if (ta_http_encode_post(&post, &out) != TA_OK) pd->error = 1;
    pd->message_length = out.length;
}

static void http_deliver(ta_system *system, ta_datamodel_ecmascript *ecmascript,
                         const pending *pd) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = pd->actor;
    if (pd->error) {
        event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
        event.sendid = (ta_string_view){pd->sendid, (uint32_t)strlen(pd->sendid)};
        ta_system_send(system, &event);
        return;
    }
    char scratch[8192];
    ta_http_request request;
    if (ta_http_parse_request((ta_bytes){pd->message, pd->message_length},
                              &(ta_buffer){scratch, sizeof scratch, 0}, &request) != TA_OK)
        return;
    char json[8192];
    size_t n = 0;
    n += (size_t)snprintf(json + n, sizeof json - n, "{");
    for (uint32_t i = 0; i < request.field_count && n < sizeof json - 64; i++) {
        n += (size_t)snprintf(json + n, sizeof json - n, "%s\"%.*s\":\"", i ? "," : "",
                              (int)request.fields[i].name.length, request.fields[i].name.pointer);
        for (uint32_t k = 0; k < request.fields[i].value.length && n < sizeof json - 8; k++) {
            char c = request.fields[i].value.pointer[k];
            if (c == '"' || c == '\\') json[n++] = '\\';
            json[n++] = c;
        }
        n += (size_t)snprintf(json + n, sizeof json - n, "\"");
    }
    snprintf(json + n, sizeof json - n, "}");
    event.name = request.event;
    event.origin = (ta_string_view){HTTP_LOC, (uint32_t)strlen(HTTP_LOC)};
    event.origintype = (ta_string_view){TA_BASIC_HTTP_TYPE, (uint32_t)strlen(TA_BASIC_HTTP_TYPE)};
    event.raw = (ta_string_view){pd->message, (uint32_t)pd->message_length};
    if (ecmascript)
        event.data = ta_datamodel_ecmascript_payload_eval(
            ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    ta_system_send(system, &event);
}

static char *slurp(const char *path, size_t *length) {
    ta_bytes b;
    if (ta_runtime_load_file(path, &b) != TA_OK) return NULL;
    *length = b.length;
    return (char *)b.pointer;
}

static test_outcome run_one(const char *dir, const char *start, const char *deps, int verbose) {
    char path[2048];
    ta_source source[8];
    char *texts[8];
    char names[8][2048];
    uint32_t n = 0;
    snprintf(path, sizeof path, "%s/%s", dir, start);
    size_t length;
    texts[0] = slurp(path, &length);
    if (!texts[0]) return R_COMPILE;
    snprintf(names[0], sizeof names[0], "%s", path);
    source[0].name = (ta_string_view){names[0], (uint32_t)strlen(names[0])};
    source[0].text = (ta_string_view){texts[0], (uint32_t)length};
    n = 1;
    if (strcmp(deps, "-") != 0) { /* dependency documents become charts of the image */
        char buffer[1024];
        snprintf(buffer, sizeof buffer, "%s", deps);
        for (char *tok = strtok(buffer, ","); tok && n < 8; tok = strtok(NULL, ",")) {
            snprintf(path, sizeof path, "%s/%s", dir, tok);
            texts[n] = slurp(path, &length);
            if (!texts[n]) continue;
            const char *base = strrchr(tok, '/');
            snprintf(names[n], sizeof names[n], "%s", base ? base + 1 : tok);
            source[n].name = (ta_string_view){names[n], (uint32_t)strlen(names[n])};
            source[n].text = (ta_string_view){texts[n], (uint32_t)length};
            n++;
        }
    }
    ta_runtime_files *files = ta_runtime_files_new();
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.resolve = ta_runtime_resolve;
    options.resolve_context = files;
    ta_compile_result *result;
    ta_status cst = ta_compile_charts(&options, (ta_source_list){source, n}, &result);
    for (uint32_t i = 0; i < n; i++)
        ta_runtime_free_file((ta_bytes){texts[i], 0});
    ta_runtime_files_free(files);
    if (cst != TA_OK) {
        if (verbose) {
            const ta_diagnostic *diagnostic;
            uint32_t nd = ta_compile_diagnostics(result, &diagnostic);
            for (uint32_t i = 0; i < nd; i++)
                fprintf(stderr, "    %.*s:%u: %.*s\n", (int)diagnostic[i].file.length,
                        diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "",
                        diagnostic[i].line, (int)diagnostic[i].message.length,
                        diagnostic[i].message.pointer);
        }
        ta_compile_free(result);
        return R_COMPILE;
    }
    ta_string_view bytes = ta_compile_image(result);
    void *copy = malloc(bytes.length + 8);
    memcpy(copy, bytes.pointer, bytes.length);
    ta_compile_free(result);
    ta_image image;
    if (ta_image_open((ta_bytes){copy, bytes.length}, &image) != TA_OK) {
        free(copy);
        return R_COMPILE;
    }
    datamodel_test *datamodel = use_ecmascript ? NULL : datamodel_test_new();
    ta_datamodel_ecmascript_config ec = {sizeof ec, 0, 0, 0, ecmascript_realm, NULL};
    ta_datamodel_ecmascript *ecmascript = use_ecmascript ? ta_datamodel_ecmascript_new(&ec) : NULL;
    ta_datamodel_binding binding = use_ecmascript ? ta_datamodel_ecmascript_binding(ecmascript)
                                                  : datamodel_test_binding(datamodel);
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.ticks_per_second = 1000;
    config.max_events = 1024;
    config.max_actors = 64;
    ta_io_processor_descriptor http = {{TA_BASIC_HTTP_TYPE, (uint32_t)strlen(TA_BASIC_HTTP_TYPE)},
                                       {HTTP_LOC, (uint32_t)strlen(HTTP_LOC)},
                                       TA_IO_PROCESSOR_SYNC,
                                       0};
    config.io_processors = &http;
    config.io_processor_count = 1;
    static pending pend[16];
    ta_system *system;
    test_outcome res = R_TIMEOUT;
    if (ta_runtime_system_new(&(ta_system_setup){{0, 0}, &image, &config, &binding, 1, 0},
                              &system) != TA_OK) {
        if (datamodel) datamodel_test_free(datamodel);
        if (ecmascript) ta_datamodel_ecmascript_free(ecmascript);
        free(copy);
        return R_COMPILE;
    }
    ta_system_boot(system);
    /* bounded work per test: a runaway test must not exhaust the host */
    for (int iter = 0; iter < 200 && res == R_TIMEOUT; iter++) {
        ta_run run_result = ta_system_run(system, 20000);
        const ta_effect *effect;
        uint32_t ne = ta_system_effects(system, &effect);
        for (uint32_t i = 0; i < ne; i++) {
            if (effect[i].kind == TA_EFFECT_LOG && verbose)
                fprintf(stderr, "    log %.*s: %.*s\n", (int)effect[i].auxiliary.length,
                        effect[i].auxiliary.pointer, (int)effect[i].event.length,
                        effect[i].event.pointer);
            if (effect[i].kind == TA_EFFECT_LOG && effect[i].auxiliary.length == 7 &&
                !memcmp(effect[i].auxiliary.pointer, "Outcome", 7))
                res = (effect[i].event.length == 4 && !memcmp(effect[i].event.pointer, "pass", 4))
                          ? R_PASS
                          : R_FAIL;
            if (verbose && effect[i].kind != TA_EFFECT_LOG)
                fprintf(stderr, "    effect kind=%u target=%.*s event=%.*s\n", effect[i].kind,
                        (int)effect[i].target.length,
                        effect[i].target.pointer ? effect[i].target.pointer : "",
                        (int)effect[i].event.length,
                        effect[i].event.pointer ? effect[i].event.pointer : "");
        }
        uint32_t np = 0;
        for (uint32_t i = 0; i < ne && np < 16; i++)
            if (effect[i].kind == TA_EFFECT_SEND && effect[i].io_processor == 0)
                http_send(ecmascript, &effect[i], &pend[np++]);
        ta_system_effects_ack(system, ne);
        for (uint32_t i = 0; i < np; i++)
            http_deliver(system, ecmascript, &pend[i]);
        if (np && run_result.status == TA_RUN_IDLE) continue;
        if (run_result.status == TA_RUN_EXHAUSTED || run_result.status == TA_RUN_FAULT) break;
        if (run_result.status == TA_RUN_IDLE) {
            if (run_result.next_deadline == UINT64_MAX) break;
            ta_system_advance_time(system, run_result.next_deadline);
        }
    }
    ta_runtime_system_free(system);
    if (datamodel) datamodel_test_free(datamodel);
    if (ecmascript) ta_datamodel_ecmascript_free(ecmascript);
    free(copy);
    return res;
}

static int skipped(const char *list, const char *id) {
    if (!list) return 0;
    size_t l = strlen(id);
    for (const char *p = list; (p = strstr(p, id));
         p++) /* "#" lines never match: id at line start */
        if ((p == list || p[-1] == '\n') && (p[l] == ' ' || p[l] == '\t' || p[l] == '\n')) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <converted-dir> [--dm ecmascript|test] [--min N] [--skip FILE] "
                "[--require-all] [--only ID]\n",
                argv[0]);
        return 2;
    }
    const char *dir = argv[1], *only = NULL, *skipfile = NULL;
    int min_pass = 0, require_all = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dm") && i + 1 < argc)
            use_ecmascript = !strcmp(argv[++i], "ecmascript");
        else if (!strcmp(argv[i], "--realm") && i + 1 < argc)
            ecmascript_realm = strcmp(argv[++i], "isolated")
                                   ? TA_DATAMODEL_ECMASCRIPT_REALM_AUTO
                                   : TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED;
        else if (!strcmp(argv[i], "--min") && i + 1 < argc)
            min_pass = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
            skipfile = argv[++i];
        else if (!strcmp(argv[i], "--require-all"))
            require_all = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            only = argv[++i];
    }
    char path[2048];
    snprintf(path, sizeof path, "%s/index.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "no %s (run: mise run w3c:fetch w3c:convert)\n", path);
        return require_all ? 1 : 0;
    }
    size_t sl = 0;
    char *skips = skipfile ? slurp(skipfile, &sl) : NULL;
    char line[4096], id[64], conf[64], relative_path[1024], deps[1024];
    int counts[2][4] = {{0}}, nskip = 0, failures = 0;
    while (fgets(line, sizeof line, f)) {
        int k = sscanf(line, "%63s %63s %1023s %1023s", id, conf, relative_path, deps);
        if (k < 3) continue;
        if (k == 3) strcpy(deps, "-");
        if (only && strcmp(only, id) != 0) continue;
        if (!only && skipped(skips, id)) {
            nskip++;
            continue;
        }
        test_outcome r = run_one(dir, relative_path, deps, only != NULL);
        int mand = strcmp(conf, "mandatory") == 0;
        counts[mand][r]++;
        if (r != R_PASS) failures++;
        if (r != R_PASS || only) printf("%-6s %-9s %s\n", id, conf, rname[r]);
        fflush(stdout);
    }
    fclose(f);
    if (skips) ta_runtime_free_file((ta_bytes){skips, sl});
    printf("datamodel: %s; skipped: %d\n",
           !use_ecmascript    ? "test subset"
           : ecmascript_realm ? "ecmascript (quickjs-ng, isolated realms)"
                              : "ecmascript (quickjs-ng, shared realm)",
           nskip);
    printf("mandatory: %d pass, %d fail, %d compile, %d timeout\n", counts[1][0], counts[1][1],
           counts[1][2], counts[1][3]);
    printf("optional:  %d pass, %d fail, %d compile, %d timeout\n", counts[0][0], counts[0][1],
           counts[0][2], counts[0][3]);
    if (require_all && failures) return 1;
    return counts[1][0] >= min_pass ? 0 : 1;
}

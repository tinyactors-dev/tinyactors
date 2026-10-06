/* ta_compile: SCXML -> image structure. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include "image/records.h"

static const char *CHART =
    "<?xml version='1.0'?>\n"
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='ecmascript'\n"
    "       initial='p' name='demo'>\n"
    "  <datamodel><data id='x' expr='0'/><data id='y'>[1, 2]</data></datamodel>\n"
    "  <parallel id='p'>\n"
    "    <state id='a' initial='a2'>\n"
    "      <onentry><raise event='go.now'/><log label='hi' expr='x'/></onentry>\n"
    "      <onexit><assign location='x' expr='x + 1'/></onexit>\n"
    "      <state id='a1'><transition event='go' target='a2'/></state>\n"
    "      <state id='a2'>\n"
    "        <transition event='error.* go.later' cond='x &gt; 1' target='a1'>\n"
    "          <if cond='x == 1'><raise event='one'/><elseif cond='x == 2'/><raise event='two'/>\n"
    "          <else/><raise event='many'/></if>\n"
    "          <foreach array='y' item='i' index='j'><send event='item' "
    "target='#_parent'/></foreach>\n"
    "        </transition>\n"
    "      </state>\n"
    "      <history id='h' type='deep'><transition target='a1'/></history>\n"
    "    </state>\n"
    "    <state id='b'>\n"
    "      <transition target='done' cond='true'/>\n"
    "      <transition event='*' type='internal'/>\n"
    "      <invoke id='w' src='worker.scxml'><param name='n' expr='x'/></invoke>\n"
    "      <invoke id='inl'><content><scxml initial='z'><final "
    "id='z'/></scxml></content></invoke>\n"
    "    </state>\n"
    "  </parallel>\n"
    "  <final id='done'><donedata><param name='r' expr='x'/></donedata></final>\n"
    "</scxml>\n";

static const test_file FILES[] = {{"worker.scxml",
                                   "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='w'>"
                                   "<state id='w'><transition event='stop' target='f'/></state>"
                                   "<final id='f'/></scxml>"},
                                  {NULL, NULL}};

static uint32_t sid_of(const ta_image *image, uint32_t chart, const char *id) {
    ta_string_view s = {id, (uint32_t)strlen(id)};
    return ta_image_state_by_name(image, chart, s);
}

static int name_matches(const ta_image_view *view, uint32_t lo, uint32_t hi, const char *name) {
    int exact;
    uint32_t e = ta_event_name_lookup(view, name, (uint32_t)strlen(name), &exact);
    return lo <= e && e < hi;
}

static void test_structure(void) {
    size_t length;
    void *bytes = compile_chart("demo.scxml", CHART, FILES, &length, 0);
    CHECK(bytes != NULL);
    if (!bytes) return;
    ta_image image;
    CHECK_EQUAL(ta_image_open(bytes, length, &image), TA_OK);
    const ta_image_view *view = ta_image_view_of(&image);
    CHECK_EQUAL(view->chart_count, 3); /* demo, worker (linked), inline */
    ta_string_view datamodel = ta_image_chart_datamodel(&image, 0);
    CHECK(datamodel.length == 10 && memcmp(datamodel.pointer, "ecmascript", 10) == 0);
    CHECK_EQUAL(ta_image_chart_by_name(&image, (ta_string_view){"demo", 4}), 0);
    CHECK_EQUAL(ta_image_chart_by_name(&image, (ta_string_view){"worker.scxml", 12}), 1);

    const ta_chart_record *chart = &view->charts[0];
    const ta_state_record *state = ta_chart_states(view, chart);
    /* document order: root, p, a, a1, a2, h, b, done */
    CHECK_EQUAL(chart->state_count, 8);
    uint32_t p = sid_of(&image, 0, "p"), a = sid_of(&image, 0, "a"), a1 = sid_of(&image, 0, "a1");
    uint32_t a2 = sid_of(&image, 0, "a2"), h = sid_of(&image, 0, "h"), b = sid_of(&image, 0, "b");
    uint32_t done = sid_of(&image, 0, "done");
    CHECK_EQUAL(p, 1);
    CHECK_EQUAL(a, 2);
    CHECK_EQUAL(a1, 3);
    CHECK_EQUAL(a2, 4);
    CHECK_EQUAL(h, 5);
    CHECK_EQUAL(b, 6);
    CHECK_EQUAL(done, 7);
    CHECK_EQUAL(state[0].kind, TA_STATE_COMPOUND);
    CHECK_EQUAL(state[p].kind, TA_STATE_PARALLEL);
    CHECK_EQUAL(state[a].kind, TA_STATE_COMPOUND);
    CHECK_EQUAL(state[a1].kind, TA_STATE_ATOMIC);
    CHECK_EQUAL(state[h].kind, TA_STATE_HISTORY_DEEP);
    CHECK_EQUAL(state[done].kind, TA_STATE_FINAL);
    CHECK_EQUAL(state[p].end, done);
    CHECK_EQUAL(state[a].end, b);
    CHECK_EQUAL(state[a1].depth, 3);
    CHECK_EQUAL(chart->max_depth, 3);
    CHECK(state[b].flags & TA_STATE_FLAG_HAS_EVENTLESS_TRANSITIONS);
    CHECK(view->u32[chart->eventless_mask] & (1u << b));
    CHECK(state[done].donedata != TA_NIL);
    CHECK_EQUAL(chart->data_count, 2);
    CHECK_EQUAL(chart->max_foreach, 1);
    CHECK(chart->history_word_count >= 1);

    /* initial transitions */
    const ta_transition_record *transition_record = ta_chart_transitions(view, chart);
    const ta_transition_record *ri = &transition_record[state[0].initial];
    CHECK(ri->flags & TA_TRANSITION_FLAG_INITIAL);
    CHECK_EQUAL(ri->domain, 0);
    CHECK_EQUAL(view->u32[ri->targets_offset], p);
    CHECK_EQUAL(view->u32[transition_record[state[a].initial].targets_offset], a2);
    CHECK_EQUAL(transition_record[state[h].initial].domain, a);

    /* a2's transition: two descriptors, cond, LCCA domain = a */
    const ta_transition_record *transition = &transition_record[state[a2].first_transition];
    CHECK_EQUAL(transition->event_count, 2);
    CHECK(transition->condition != TA_NIL);
    CHECK_EQUAL(transition->domain, a);
    CHECK(transition->action != TA_NIL);
    uint32_t lo0 = view->u32[transition->events_offset],
             hi0 = view->u32[transition->events_offset + 1];
    uint32_t lo1 = view->u32[transition->events_offset + 2],
             hi1 = view->u32[transition->events_offset + 3];
    CHECK(name_matches(view, lo0, hi0, "error.execution"));
    CHECK(name_matches(view, lo0, hi0, "error.anything.at.all"));
    CHECK(!name_matches(view, lo0, hi0, "errors"));
    CHECK(name_matches(view, lo1, hi1, "go.later"));
    CHECK(name_matches(view, lo1, hi1, "go.later.still"));
    CHECK(!name_matches(view, lo1, hi1, "go"));
    /* b: eventless external transition to done -> domain root; '*' internal targetless */
    const ta_transition_record *transitions_b = &transition_record[state[b].first_transition];
    CHECK_EQUAL(transitions_b[0].event_count, 0);
    CHECK_EQUAL(transitions_b[0].domain, 0);
    CHECK_EQUAL(transitions_b[1].domain, TA_NIL);
    CHECK(transitions_b[1].flags & TA_TRANSITION_FLAG_INTERNAL);
    CHECK(name_matches(view, view->u32[transitions_b[1].events_offset],
                       view->u32[transitions_b[1].events_offset + 1], "whatever"));

    /* done.state.<id> name ids exist for compound/parallel states */
    ta_string_view dn = ta_image_event_name(&image, state[a].done_event);
    CHECK(dn.length == 12 && memcmp(dn.pointer, "done.state.a", 12) == 0);

    /* invokes: linked src chart, inline chart, static id slots */
    CHECK_EQUAL(chart->invoke_count, 2);
    const ta_invoke_record *invoke = &view->invokes[chart->first_invoke];
    CHECK_EQUAL(invoke[0].state, b);
    CHECK_EQUAL(invoke[0].source_chart, 1);
    CHECK(invoke[0].capture != TA_NIL);
    CHECK_EQUAL(invoke[1].source_chart, 2);
    CHECK_EQUAL(view->charts[2].state_count, 2);

    /* sends: #_parent target */
    CHECK_EQUAL(chart->send_count, 1);
    CHECK_EQUAL(view->sends[chart->first_send].target_kind, TA_SEND_TARGET_PARENT);

    /* bytecode of onentry: BLOCK end RAISE <go.now> LOG ... END */
    const uint32_t *code = view->code;
    uint32_t pc = state[a].onentry;
    CHECK_EQUAL(code[pc], TA_OPCODE_BLOCK);
    CHECK_EQUAL(code[pc + 2], TA_OPCODE_RAISE);
    ta_string_view rn = ta_image_event_name(&image, code[pc + 3]);
    CHECK(rn.length == 6 && memcmp(rn.pointer, "go.now", 6) == 0);
    CHECK_EQUAL(code[pc + 4], TA_OPCODE_LOG);

    /* expressions carry their kind and source */
    ta_expression_info expression_info;
    CHECK_EQUAL(ta_image_expression(&image, 0, transition->condition, &expression_info), TA_OK);
    CHECK_EQUAL(expression_info.kind, TA_EXPRESSION_CONDITION);
    CHECK(expression_info.source.length == 5 &&
          memcmp(expression_info.source.pointer, "x > 1", 5) == 0);
    ta_data_info data_info;
    CHECK_EQUAL(ta_image_data(&image, 0, 1, &data_info), TA_OK);
    CHECK(data_info.id.length == 1 && data_info.id.pointer[0] == 'y');
    CHECK_EQUAL(ta_image_expression(&image, 0, data_info.expression, &expression_info), TA_OK);
    CHECK_EQUAL(expression_info.kind, TA_EXPRESSION_CONTENT);
    CHECK(expression_info.source.length == 6 &&
          memcmp(expression_info.source.pointer, "[1, 2]", 6) == 0);
    free(bytes);
}

static void test_errors(void) {
    CHECK(compile_error_contains("<scxml><state id='a'><transition target='nope'/></state></scxml>",
                                 "unknown state id"));
    CHECK(compile_error_contains("<scxml><state id='a'></scxml>", "XML"));
    CHECK(compile_error_contains("<scxml><state id='a'/><state id='a'/></scxml>", "duplicate"));
    CHECK(compile_error_contains("<scxml><state id='a'><onentry><bogus/></onentry></state></scxml>",
                                 "unknown executable"));
    CHECK(compile_error_contains("<scxml><state id='a'><onentry><send event='e' delay='5 parsecs'/>"
                                 "</onentry></state></scxml>",
                                 "invalid delay"));
    CHECK(compile_error_contains("<foo/>", "root element"));
    CHECK(compile_error_contains("<scxml/>", "at least one state"));
    CHECK(compile_error_contains(
        "<scxml><state id='a'><invoke src='missing.scxml'/></state></scxml>", "src"));
    CHECK(compile_error_contains(
        "<scxml><state id='a'><history id='h'/><state id='b'/></state></scxml>", "default"));
}

/* Copies `text` to `p` and answers the end, for documents sized up front. */
static char *append(char *p, const char *text) {
    size_t length = strlen(text);
    memcpy(p, text, length);
    return p + length;
}

/* A growable string for generated documents. */
static char *repeat_document(const char *head, const char *unit, int n, const char *mid,
                             const char *unit2, int n2, const char *tail) {
    size_t length = strlen(head) + strlen(unit) * (size_t)n + strlen(mid) +
                    strlen(unit2) * (size_t)n2 + strlen(tail) + 1;
    char *d = (char *)malloc(length), *p = d;
    p = append(p, head);
    for (int i = 0; i < n; i++)
        p = append(p, unit);
    p = append(p, mid);
    for (int i = 0; i < n2; i++)
        p = append(p, unit2);
    p = append(p, tail);
    *p = '\0';
    return d;
}

static int compiles(const char *document) {
    size_t length;
    void *b = compile_chart("gen.scxml", document, NULL, &length, 1);
    free(b);
    return b != NULL;
}

/* Nesting and count limits: rejected with a diagnostic, never wrapped
 * around or recursed into. */
static void test_limits(void) {
    char *d;
    /* <if>/<foreach> nesting: TA_COMPILE_MAX_CONTENT_NESTING levels */
    d = repeat_document("<scxml><state id='a'><onentry>", "<if cond='c'>", 64, "<raise event='e'/>",
                        "</if>", 64, "</onentry></state></scxml>");
    CHECK(compiles(d));
    free(d);
    d = repeat_document("<scxml><state id='a'><onentry>", "<if cond='c'>", 65, "", "</if>", 65,
                        "</onentry></state></scxml>");
    CHECK(compile_error_contains(d, "<if> nested too deep"));
    free(d);
    d = repeat_document("<scxml><state id='a'><onentry>",
                        "<foreach array='a' item='i'><if cond='c'>", 33, "", "</if></foreach>", 33,
                        "</onentry></state></scxml>");
    CHECK(compile_error_contains(d, "nested too deep"));
    free(d);
    d = repeat_document("<scxml><state id='a'><onentry>",
                        "<foreach array='a' item='i'><if cond='c'>", 32, "", "</if></foreach>", 32,
                        "</onentry></state></scxml>");
    CHECK(compiles(d));
    free(d);
    /* state nesting: 200 levels below <scxml> */
    d = repeat_document("<scxml>", "<state>", 200, "", "</state>", 200, "</scxml>");
    CHECK(compiles(d));
    free(d);
    d = repeat_document("<scxml>", "<state>", 201, "", "</state>", 201, "</scxml>");
    CHECK(compile_error_contains(d, "state nesting too deep"));
    free(d);
    /* descriptor and target lists are stored with 16-bit counts */
    d = repeat_document("<scxml><state id='a'><transition event='", "e ", 65535, "' target='a'/>",
                        "", 0, "</state></scxml>");
    CHECK(compiles(d));
    free(d);
    d = repeat_document("<scxml><state id='a'><transition event='", "e ", 65536, "' target='a'/>",
                        "", 0, "</state></scxml>");
    CHECK(compile_error_contains(d, "more than 65535 event descriptors"));
    free(d);
    d = repeat_document("<scxml><parallel id='p'><state id='a'/><state id='b'/></parallel>"
                        "<state id='t'><transition event='go' target='",
                        "a b ", 32768, "'/>", "", 0, "</state></scxml>");
    CHECK(compile_error_contains(d, "more than 65535 targets"));
    free(d);
    /* event names: at most 64 dot-separated parts (every prefix is stored) */
    d = repeat_document("<scxml><state id='a'><transition event='e", ".x", 63, "' target='a'/>", "",
                        0, "</state></scxml>");
    CHECK(compiles(d));
    free(d);
    d = repeat_document("<scxml><state id='a'><onentry><raise event='e", ".x", 64, "'/></onentry>",
                        "", 0, "</state></scxml>");
    CHECK(compile_error_contains(d, "more than 64 dot-separated parts"));
    free(d);
    /* an error on every line: a bounded list of diagnostics */
    d = repeat_document("<scxml>", "<state id='a'/>\n", 1000, "", "", 0, "</scxml>");
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    ta_source source = {{"dup.scxml", 9}, {d, (uint32_t)strlen(d)}};
    ta_compile_result *result = NULL;
    CHECK_EQUAL(ta_compile_charts(&options, &source, 1, &result), TA_ERROR_INVALID);
    const ta_diagnostic *diagnostic;
    CHECK_EQUAL(ta_compile_diagnostics(result, &diagnostic), 200);
    CHECK(diagnostic[198].severity == TA_DIAGNOSTIC_ERROR && diagnostic[198].line == 200);
    CHECK(diagnostic[199].severity == TA_DIAGNOSTIC_NOTE && diagnostic[199].message.length > 0 &&
          memcmp(diagnostic[199].message.pointer, "too many diagnostics", 20) == 0);
    ta_compile_free(result);
    free(d);
}

/* Inline documents nested beyond what fits a 256-byte chart name still get
 * a chart each (a truncated name used to alias the deeper ones). */
static void test_deep_inline_documents(void) {
    char *d = repeat_document("<scxml>", "<state><invoke><content><scxml>", 60, "<final id='z'/>",
                              "</scxml></content></invoke></state>", 60, "</scxml>");
    size_t length;
    void *b =
        compile_chart("a-long-source-name-for-the-inline-documents.scxml", d, NULL, &length, 0);
    CHECK(b != NULL);
    if (b) {
        ta_image image;
        CHECK_EQUAL(ta_image_open(b, length, &image), TA_OK);
        const ta_image_view *view = ta_image_view_of(&image);
        CHECK_EQUAL(view->chart_count, 61);
        CHECK_EQUAL(view->invoke_count, 60);
        for (uint32_t i = 0; i < view->invoke_count; i++) /* chart i invokes chart i + 1 */
            CHECK_EQUAL(view->invokes[i].source_chart, i + 1);
    }
    free(b);
    free(d);
}

/* Prefixes and default namespaces are scoped per element. */
static void test_namespaces(void) {
    const char *document =
        "<s:scxml xmlns:s='http://www.w3.org/2005/07/scxml' xmlns='urn:other' initial='a'>"
        "<s:state id='a'><s:onentry><foo xmlns:s='urn:shadow'><s:raise event='x'/></foo>"
        "<s:raise event='y'/></s:onentry><s:transition event='x' target='b'/></s:state>"
        "<s:state id='b' xmlns='http://www.w3.org/2005/07/scxml'><state id='b1'/>"
        "<state xmlns='' id='b2'/><s:final id='b3' xmlns:s='urn:x'/></s:state>"
        "<s:final id='f'/><state id='other'/></s:scxml>";
    size_t length;
    void *b = compile_chart("ns.scxml", document, NULL, &length, 1);
    CHECK(b != NULL);
    if (!b) return;
    ta_image image;
    CHECK_EQUAL(ta_image_open(b, length, &image), TA_OK);
    CHECK_EQUAL(ta_image_state_count(&image, 0), 6); /* root a b b1 b2 f */
    CHECK(sid_of(&image, 0, "b2") != TA_NIL);
    CHECK_EQUAL(sid_of(&image, 0, "b3"), TA_NIL);    /* s: rebound to urn:x */
    CHECK_EQUAL(sid_of(&image, 0, "other"), TA_NIL); /* the default namespace is urn:other */
    const ta_image_view *view = ta_image_view_of(&image);
    const ta_chart_record *chart = &view->charts[0];
    uint32_t raises = 0;
    for (uint32_t p = chart->first_code; p < chart->first_code + chart->code_count;
         p += ta_opcode_length[view->code[p]])
        raises += view->code[p] == TA_OPCODE_RAISE;
    CHECK_EQUAL(raises, 1); /* <s:raise event='x'/> inside <foo> is foreign */
    free(b);
}

/* Generated ids: the smallest free _ta_s<k> with k >= the state's index. */
static void test_generated_ids(void) {
    const char *document = "<scxml><state/><state id='_ta_s2'/><state/><state id='_ta_s1'/><state/>"
                           "<state id='_ta_s6'/><state/></scxml>";
    size_t length;
    void *b = compile_chart("ids.scxml", document, NULL, &length, 0);
    CHECK(b != NULL);
    if (!b) return;
    ta_image image;
    CHECK_EQUAL(ta_image_open(b, length, &image), TA_OK);
    static const char *const want[] = {"",       "_ta_s3", "_ta_s2", "_ta_s4",
                                       "_ta_s1", "_ta_s5", "_ta_s6", "_ta_s7"};
    for (uint32_t i = 0; i < 8; i++) {
        ta_string_view n = ta_image_state_name(&image, 0, i);
        CHECK(n.length == strlen(want[i]) && memcmp(n.pointer, want[i], n.length) == 0);
    }
    free(b);
}

/* The host's abort flag stops a compile: before it starts, from inside it
 * (here: set by the resolver), and is ignored when `size` does not cover it. */
static volatile int abort_now;
static ta_status abort_resolve(void *context, ta_string_view base, ta_string_view source,
                               ta_source *out) {
    abort_now = 1;
    return test_resolve(context, base, source, out);
}

static ta_status compile_with_flag(uint32_t size, ta_resolve_function resolve, const char *document,
                                   ta_compile_result **result) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = size;
    options.resolve = resolve;
    options.resolve_context = (void *)FILES;
    options.abort_flag = &abort_now;
    ta_source source = {{"demo.scxml", 10}, {document, (uint32_t)strlen(document)}};
    return ta_compile_charts(&options, &source, 1, result);
}

static void test_abort(void) {
    ta_compile_result *result = NULL;
    const ta_diagnostic *diagnostic;
    abort_now = 1;
    CHECK_EQUAL(compile_with_flag(sizeof(ta_compile_options), test_resolve, CHART, &result),
                TA_ERROR_ABORTED);
    CHECK_EQUAL(ta_compile_image(result).length, 0);
    CHECK_EQUAL(ta_compile_diagnostics(result, &diagnostic), 1);
    CHECK(diagnostic[0].severity == TA_DIAGNOSTIC_ERROR && diagnostic[0].message.length == 19 &&
          memcmp(diagnostic[0].message.pointer, "compilation aborted", 19) == 0);
    ta_compile_free(result);
    /* a caller built against the older, shorter struct: no flag */
    CHECK_EQUAL(compile_with_flag((uint32_t)offsetof(ta_compile_options, abort_flag), test_resolve,
                                  CHART, &result),
                TA_OK);
    ta_compile_free(result);
    /* set while compiling (CHART's <invoke src> calls the resolver) */
    abort_now = 0;
    CHECK_EQUAL(compile_with_flag(sizeof(ta_compile_options), abort_resolve, CHART, &result),
                TA_ERROR_ABORTED);
    CHECK_EQUAL(ta_compile_image(result).length, 0);
    ta_compile_free(result);
    /* and in the parser of a big document */
    abort_now = 0;
    char *big = repeat_document("<scxml>", "<state/>", 100000, "", "", 0, "</scxml>");
    abort_now = 1;
    CHECK_EQUAL(compile_with_flag(sizeof(ta_compile_options), test_resolve, big, &result),
                TA_ERROR_ABORTED);
    ta_compile_free(result);
    free(big);
    /* a system manifest */
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.abort_flag = &abort_now;
    const char *manifest = "<system><chart src='x.scxml'/></system>";
    ta_source m = {{"sys.xml", 7}, {manifest, (uint32_t)strlen(manifest)}};
    CHECK_EQUAL(ta_compile_system(&options, &m, &result), TA_ERROR_ABORTED);
    ta_compile_free(result);
    abort_now = 0;
    CHECK_EQUAL(compile_with_flag(sizeof(ta_compile_options), test_resolve, CHART, &result), TA_OK);
    ta_compile_free(result);
}

int ta_parse_duration_ns(const char *s, uint64_t *out);

static void test_durations(void) {
    uint64_t ns = 0;
    CHECK(ta_parse_duration_ns("2s", &ns) && ns == 2000000000ull);
    CHECK(ta_parse_duration_ns(" 1.5s ", &ns) && ns == 1500000000ull);
    CHECK(ta_parse_duration_ns("250ms", &ns) && ns == 250000000ull);
    CHECK(ta_parse_duration_ns(".5s", &ns) && ns == 500000000ull);
    /* beyond 64 bits of nanoseconds: saturated ("never"), not wrapped around */
    CHECK(ta_parse_duration_ns("300000000000s", &ns) && ns == UINT64_MAX);
    CHECK(ta_parse_duration_ns("18446744072s", &ns) && ns == 18446744072000000000ull);
    CHECK(!ta_parse_duration_ns("5", &ns));
    CHECK(!ta_parse_duration_ns("s", &ns));
    CHECK(!ta_parse_duration_ns("1h", &ns));
}

static void test_manifest(void) {
    static const test_file files[] = {{"ping.scxml", "<scxml initial='s'><state id='s'/></scxml>"},
                                      {"pong.scxml", "<scxml initial='s'><state id='s'/></scxml>"},
                                      {NULL, NULL}};
    const char *manifest = "<system><chart name='ping' src='ping.scxml'/>"
                           "<chart src='pong.scxml'/>"
                           "<actor name='p1' chart='ping'/><actor name='p2' chart='pong.scxml'/>"
                           "<actor name='p3' chart='ping'/></system>";
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.resolve = test_resolve;
    options.resolve_context = (void *)files;
    ta_source m = {{"sys.xml", 7}, {manifest, (uint32_t)strlen(manifest)}};
    ta_compile_result *result;
    CHECK_EQUAL(ta_compile_system(&options, &m, &result), TA_OK);
    ta_string_view bytes = ta_compile_image(result);
    void *copy = aligned_alloc(8, (bytes.length + 7) & ~7u);
    memcpy(copy, bytes.pointer, bytes.length);
    ta_image image;
    CHECK_EQUAL(ta_image_open(copy, bytes.length, &image), TA_OK);
    ta_image_info info;
    ta_image_get_info(&image, &info);
    CHECK_EQUAL(info.chart_count, 2);
    CHECK_EQUAL(info.boot_actor_count, 3);
    CHECK_EQUAL(ta_image_chart_by_name(&image, (ta_string_view){"ping", 4}), 0);
    const ta_image_view *view = ta_image_view_of(&image);
    CHECK_EQUAL(view->actors[1].chart, 1);
    /* the public view of the boot list */
    ta_string_view name;
    uint32_t chart = 99;
    CHECK_EQUAL(ta_image_boot_actor(&image, 1, &name, &chart), TA_OK);
    CHECK(name.length == 2 && memcmp(name.pointer, "p2", 2) == 0 && chart == 1);
    CHECK_EQUAL(ta_image_boot_actor(&image, 2, &name, &chart), TA_OK);
    CHECK(name.length == 2 && memcmp(name.pointer, "p3", 2) == 0 && chart == 0);
    CHECK_EQUAL(ta_image_boot_actor(&image, 3, &name, &chart), TA_ERROR_INVALID);
    free(copy);
    ta_compile_free(result);
}

int main(void) {
    RUN(test_structure);
    RUN(test_errors);
    RUN(test_durations);
    RUN(test_manifest);
    RUN(test_limits);
    RUN(test_deep_inline_documents);
    RUN(test_namespaces);
    RUN(test_generated_ids);
    RUN(test_abort);
    TEST_MAIN_END();
}

/* The analysis layer (analysis/analysis.h): each lint rule shown finding
 * what it is for and keeping quiet on a chart that is well, the shortest
 * paths through compound, parallel and history states, and an arena that
 * refuses at every size. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "../support/compile_util.h"
#include "analysis/analysis.h"

typedef struct test_heap {
    size_t budget; /* bytes it gives before it refuses */
} test_heap;

static void *test_alloc(void *context, size_t size, size_t align) {
    (void)align;
    test_heap *heap = (test_heap *)context;
    if (size > heap->budget) return NULL;
    heap->budget -= size;
    return malloc(size);
}

static void test_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

typedef struct test_arena {
    test_heap heap;
    ta_allocator allocator;
    ta_growable_arena arena;
} test_arena;

static void arena_begin(test_arena *a, size_t budget) {
    memset(a, 0, sizeof *a);
    a->heap.budget = budget;
    a->allocator = (ta_allocator){&a->heap, test_alloc, test_free};
    ta_growable_arena_config config = {sizeof config, 0, 1024, &a->allocator};
    ta_growable_arena_init(&a->arena, &config);
}

/* A compiled chart, opened. */
typedef struct compiled {
    void *bytes;
    ta_image image;
    const ta_image_view *view;
} compiled;

static int compile(const char *name, const char *text, const test_file *files, compiled *out) {
    size_t length = 0;
    out->bytes = compile_chart(name, text, files, &length, 0);
    if (!out->bytes || ta_image_open((ta_bytes){out->bytes, (uint32_t)length}, &out->image) != TA_OK) return 0;
    out->view = ta_image_view_of(&out->image);
    return 1;
}

static uint32_t chart_named(const compiled *c, const char *name) {
    return ta_image_chart_by_name(&c->image, (ta_string_view){name, (uint32_t)strlen(name)});
}

static uint32_t state_named(const compiled *c, uint32_t chart, const char *id) {
    const ta_chart_record *record = &c->view->charts[chart];
    for (uint32_t s = 0; s < record->state_count; s++) {
        ta_string_view name = ta_image_view_string(c->view, c->view->states[record->first_state + s].id);
        if (name.length == strlen(id) && memcmp(name.pointer, id, name.length) == 0) return s;
    }
    return TA_NIL;
}

static uint32_t event_named(const compiled *c, const char *name) {
    int exact = 0;
    uint32_t node = ta_event_name_lookup(c->view, name, (uint32_t)strlen(name), &exact);
    return exact ? node : TA_NIL;
}

/* How many findings of `rule` there are at `state` of `chart` (TA_NIL: any). */
typedef struct finding_query {
    uint32_t rule, chart, state;
} finding_query;

static uint32_t count(const ta_analysis_findings *findings, finding_query query) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < findings->count; i++) {
        const ta_analysis_finding *finding = &findings->list[i];
        if (finding->rule == query.rule && (query.chart == TA_NIL || finding->chart == query.chart) &&
            (query.state == TA_NIL || finding->state == query.state))
            found++;
    }
    return found;
}

static const ta_analysis_finding *first(const ta_analysis_findings *findings, uint32_t rule) {
    for (uint32_t i = 0; i < findings->count; i++)
        if (findings->list[i].rule == rule) return &findings->list[i];
    return NULL;
}

static ta_status lint(test_arena *a, const compiled *c, uint32_t flags, ta_analysis_findings *out) {
    ta_analysis_request request = {c->view, NULL, flags, 0, &a->arena};
    return ta_analysis_lint(&request, out);
}

/* An order: `item` before `item.added` (shadowed), a failed payment no
 * transition leaves (a dead end), an archive nothing enters, a raise
 * nothing handles and a send to a target that is no address. */
static const char order[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' name='order' datamodel='ecmascript' initial='open'>\n"
    "<datamodel><data id='total' expr='0'/></datamodel>\n"
    "<state id='open'>\n"
    "  <transition event='item' target='open'/>\n"
    "  <transition event='item.added' cond='total &gt; 0' target='paying'/>\n"
    "  <transition event='checkout' target='paying'/>\n"
    "</state>\n"
    "<state id='paying'>\n"
    "  <onentry><raise event='payment.requested'/><send event='audit' target='ledger'/></onentry>\n"
    "  <transition event='payment.done' target='paid'/>\n"
    "  <transition event='payment.failed' target='failed'/>\n"
    "</state>\n"
    "<state id='failed'/>\n"
    "<state id='archived'/>\n"
    "<final id='paid'/>\n"
    "</scxml>\n";

static void test_order_findings(void) {
    compiled c;
    if (!compile("order.scxml", order, NULL, &c)) {
        CHECK(0);
        return;
    }
    test_arena a;
    arena_begin(&a, (size_t)-1);
    ta_analysis_findings findings;
    CHECK_EQUAL(lint(&a, &c, 0, &findings), TA_OK);
    uint32_t chart = 0;
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNREACHABLE, chart, state_named(&c, chart, "archived")}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNREACHABLE, TA_NIL, TA_NIL}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_DEAD_END, chart, state_named(&c, chart, "failed")}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_DEAD_END, TA_NIL, TA_NIL}), 1); /* not archived */
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_NEVER_ENDS, TA_NIL, TA_NIL}), 0);
    const ta_analysis_finding *shadowed = first(&findings, TA_ANALYSIS_SHADOWED);
    CHECK(shadowed != NULL);
    if (shadowed) {
        CHECK_EQUAL(shadowed->transition, 1);
        CHECK_EQUAL(shadowed->other, 0);
        CHECK_EQUAL(shadowed->line, 5); /* its condition's */
    }
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_SHADOWED, TA_NIL, TA_NIL}), 1);
    const ta_analysis_finding *unhandled = first(&findings, TA_ANALYSIS_UNHANDLED_EVENT);
    CHECK(unhandled && unhandled->event == event_named(&c, "payment.requested") &&
          unhandled->state == state_named(&c, chart, "paying") && unhandled->send == TA_NIL);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNHANDLED_EVENT, TA_NIL, TA_NIL}), 1);
    const ta_analysis_finding *target = first(&findings, TA_ANALYSIS_SEND_TARGET);
    CHECK(target && target->send == 0 && target->state == state_named(&c, chart, "paying"));
    /* one chart: no system rules */
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNSENT_EVENT, TA_NIL, TA_NIL}), 0);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_EVENTLESS_LOOP, TA_NIL, TA_NIL}), 0);
    for (uint32_t i = 0; i < findings.count; i++)
        CHECK_EQUAL(findings.list[i].severity, ta_analysis_rule_severity(findings.list[i].rule));
    ta_growable_arena_free(&a.arena);
    free(c.bytes);
}

/* A support ticket: compound, history, a parallel escalation whose
 * regions end in states only the parallel's transition leaves, a hold
 * loop that never reaches a final state, a state that loops on an
 * eventless transition, and a merged ticket nothing happens to any more. */
static const char ticket[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' name='ticket' initial='new'>\n"
    "<state id='new'>\n"
    "  <transition event='assign' target='work'/>\n"
    "  <transition event='close' target='closed'/>\n"
    "  <transition event='hold' target='on_hold'/>\n"
    "  <transition event='merge' target='merged'/>\n"
    "</state>\n"
    "<state id='work' initial='triage'>\n"
    "  <history id='resume'><transition target='answering'/></history>\n"
    "  <state id='triage'><transition event='answer' target='answering'/></state>\n"
    "  <state id='answering'><transition event='wait' target='waiting'/>"
    "<transition event='solved' target='closed'/></state>\n"
    "  <transition event='escalate' target='escalated'/>\n"
    "</state>\n"
    "<state id='waiting'><transition event='reply' target='resume'/></state>\n"
    "<parallel id='escalated'>\n"
    "  <state id='engineering' initial='investigating'>\n"
    "    <state id='investigating'><transition event='fixed' target='fixed'/></state><state id='fixed'/>\n"
    "  </state>\n"
    "  <state id='support' initial='informing'>\n"
    "    <state id='informing'><transition event='informed' target='informed'/></state><state id='informed'/>\n"
    "  </state>\n"
    "  <transition event='resolve' target='closed'/>\n"
    "  <transition event='stall' target='spinning'/>\n"
    "</parallel>\n"
    "<state id='on_hold'><transition event='pause' target='paused'/></state>\n"
    "<state id='paused'><transition event='unpause' target='on_hold'/></state>\n"
    "<state id='spinning'><transition target='spinning'/></state>\n"
    "<state id='merged'/>\n"
    "<final id='closed'/>\n"
    "</scxml>\n";

static void test_ticket_findings(void) {
    compiled c;
    if (!compile("ticket.scxml", ticket, NULL, &c)) {
        CHECK(0);
        return;
    }
    test_arena a;
    arena_begin(&a, (size_t)-1);
    ta_analysis_findings findings;
    CHECK_EQUAL(lint(&a, &c, 0, &findings), TA_OK);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNREACHABLE, TA_NIL, TA_NIL}), 0); /* history included */
    /* fixed and informed: the parallel's own transitions leave them; spinning
     * and on_hold take events, they are no dead ends */
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_DEAD_END, 0, state_named(&c, 0, "merged")}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_DEAD_END, TA_NIL, TA_NIL}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_NEVER_ENDS, 0, state_named(&c, 0, "on_hold")}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_NEVER_ENDS, 0, state_named(&c, 0, "paused")}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_NEVER_ENDS, 0, state_named(&c, 0, "spinning")}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_NEVER_ENDS, TA_NIL, TA_NIL}), 3); /* merged: a dead end */
    const ta_analysis_finding *loop = first(&findings, TA_ANALYSIS_EVENTLESS_LOOP);
    CHECK(loop && loop->state == state_named(&c, 0, "spinning"));
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_EVENTLESS_LOOP, TA_NIL, TA_NIL}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_SHADOWED, TA_NIL, TA_NIL}), 0);
    /* a limit of one configuration: never-ends is not checked */
    ta_analysis_request limited = {c.view, NULL, 0, 1, &a.arena};
    CHECK_EQUAL(ta_analysis_lint(&limited, &findings), TA_OK);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_SEARCH_LIMIT, TA_NIL, TA_NIL}), 1);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_NEVER_ENDS, TA_NIL, TA_NIL}), 0);
    ta_growable_arena_free(&a.arena);
    free(c.bytes);
}

/* A payment that is well: no rule finds anything, with conditions,
 * a guarded eventless transition and a target that is an address. */
static const char payment[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' name='payment' datamodel='ecmascript' initial='authorizing'>\n"
    "<datamodel><data id='attempts' expr='0'/></datamodel>\n"
    "<state id='authorizing'>\n"
    "  <onentry><send event='authorize' target='https://bank.example/authorize'/></onentry>\n"
    "  <transition event='authorized' target='capturing'/>\n"
    "  <transition event='declined' cond='attempts &lt; 3' target='retrying'/>\n"
    "  <transition event='declined' target='failed'/>\n"
    "</state>\n"
    "<state id='retrying'><onentry><raise event='retry'/></onentry>"
    "<transition event='retry' target='authorizing'/></state>\n"
    "<state id='capturing'><transition cond='attempts &gt; 5' target='failed'/>"
    "<transition event='captured' target='settled'/></state>\n"
    "<final id='settled'/><final id='failed'/>\n"
    "</scxml>\n";

static void test_quiet_on_a_well_chart(void) {
    compiled c;
    if (!compile("payment.scxml", payment, NULL, &c)) {
        CHECK(0);
        return;
    }
    test_arena a;
    arena_begin(&a, (size_t)-1);
    ta_analysis_findings findings;
    CHECK_EQUAL(lint(&a, &c, 0, &findings), TA_OK);
    CHECK_EQUAL(findings.count, 0);
    for (uint32_t i = 0; i < findings.count; i++)
        fprintf(stderr, "  unexpected: %.*s\n", (int)ta_analysis_rule_name(findings.list[i].rule).length,
                ta_analysis_rule_name(findings.list[i].rule).pointer);
    ta_growable_arena_free(&a.arena);
    free(c.bytes);
}

/* Two charts: a checkout invokes a payment. It sends `charge`, which the
 * payment handles, and `refund`, which it does not; the payment handles
 * `cancel`, which nothing sends, and answers its parent. */
static const char checkout[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' name='checkout' initial='paying'>\n"
    "<state id='paying'>\n"
    "  <invoke id='pay' src='charge.scxml'/>\n"
    "  <onentry><send event='charge' target='#_pay'/><send event='refund' target='#_pay'/>"
    "<send event='receipt' target='#_receipts'/></onentry>\n"
    "  <transition event='charge.done' target='paid'/>\n"
    "  <transition event='done.invoke.pay' target='paid'/>\n"
    "</state>\n"
    "<final id='paid'/>\n"
    "</scxml>\n";
static const char charge[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' name='charge' initial='waiting'>\n"
    "<state id='waiting'>\n"
    "  <transition event='charge' target='charged'><send event='charge.done' target='#_parent'/></transition>\n"
    "  <transition event='cancel' target='cancelled'/>\n"
    "</state>\n"
    "<final id='charged'/><final id='cancelled'/>\n"
    "</scxml>\n";
static const test_file shop_files[] = {{"charge.scxml", charge}, {NULL, NULL}};

static void test_events_between_charts(void) {
    compiled c;
    if (!compile("checkout.scxml", checkout, shop_files, &c)) {
        CHECK(0);
        return;
    }
    test_arena a;
    arena_begin(&a, (size_t)-1);
    uint32_t parent = chart_named(&c, "checkout"), child = chart_named(&c, "charge");
    CHECK(parent != TA_NIL && child != TA_NIL);
    ta_analysis_findings findings;
    CHECK_EQUAL(lint(&a, &c, TA_ANALYSIS_SYSTEM, &findings), TA_OK);
    const ta_analysis_finding *unhandled = first(&findings, TA_ANALYSIS_UNHANDLED_EVENT);
    CHECK(unhandled && unhandled->chart == parent && unhandled->event == event_named(&c, "refund"));
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNHANDLED_EVENT, TA_NIL, TA_NIL}), 1);
    /* cancel, in the payment: only the application could send it */
    const ta_analysis_finding *unsent = first(&findings, TA_ANALYSIS_UNSENT_EVENT);
    CHECK(unsent && unsent->chart == child && unsent->event == event_named(&c, "cancel") &&
          unsent->severity == TA_ANALYSIS_NOTE);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNSENT_EVENT, TA_NIL, TA_NIL}), 1);
    /* #_receipts: no invocation of that id */
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_SEND_TARGET, parent, TA_NIL}), 1);
    /* without the system flag, the unsent events are not looked for */
    CHECK_EQUAL(lint(&a, &c, 0, &findings), TA_OK);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNSENT_EVENT, TA_NIL, TA_NIL}), 0);
    /* only the selected charts */
    uint8_t selected[2] = {0, 0};
    selected[child] = 1;
    ta_analysis_request request = {c.view, selected, TA_ANALYSIS_SYSTEM, 0, &a.arena};
    CHECK_EQUAL(ta_analysis_lint(&request, &findings), TA_OK);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNHANDLED_EVENT, TA_NIL, TA_NIL}), 0);
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNSENT_EVENT, child, TA_NIL}), 1);
    ta_growable_arena_free(&a.arena);
    free(c.bytes);
}

/* An event computed at run time may reach the invoked chart: its handled
 * events are unknown, never reported as unsent. */
static const char dispatcher[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' name='dispatcher' datamodel='ecmascript' initial='routing'>\n"
    "<state id='routing'>\n"
    "  <invoke id='worker' src='charge.scxml'/>\n"
    "  <transition event='route' target='routing'><send eventexpr='_event.data.kind' target='#_worker'/></transition>\n"
    "</state>\n"
    "</scxml>\n";

static void test_computed_events_are_unknown(void) {
    compiled c;
    if (!compile("dispatcher.scxml", dispatcher, shop_files, &c)) {
        CHECK(0);
        return;
    }
    test_arena a;
    arena_begin(&a, (size_t)-1);
    uint32_t child = chart_named(&c, "charge");
    ta_analysis_findings findings;
    CHECK_EQUAL(lint(&a, &c, TA_ANALYSIS_SYSTEM, &findings), TA_OK);
    const ta_analysis_finding *unknown = first(&findings, TA_ANALYSIS_UNKNOWN_EVENTS);
    CHECK(unknown && unknown->chart == child && unknown->other == chart_named(&c, "dispatcher"));
    CHECK_EQUAL(count(&findings, (finding_query){TA_ANALYSIS_UNSENT_EVENT, child, TA_NIL}), 0);
    ta_growable_arena_free(&a.arena);
    free(c.bytes);
}

/* ---- paths ------------------------------------------------------------------------------------ */

typedef struct path_query {
    const char *from, *to;
    uint32_t limit;
} path_query;

static ta_status find(test_arena *a, const compiled *c, path_query query, ta_analysis_path *out) {
    ta_analysis_path_request request = {c->view, 0, query.from ? state_named(c, 0, query.from) : TA_NIL,
                                        state_named(c, 0, query.to), query.limit, &a->arena};
    return ta_analysis_path_find(&request, out);
}

static int active(const compiled *c, const uint32_t *configuration, const char *id) {
    uint32_t s = state_named(c, 0, id);
    return s != TA_NIL && ((configuration[s >> 5] >> (s & 31u)) & 1u);
}

static void test_paths(void) {
    compiled c;
    if (!compile("ticket.scxml", ticket, NULL, &c)) {
        CHECK(0);
        return;
    }
    test_arena a;
    arena_begin(&a, (size_t)-1);
    ta_analysis_path path;
    /* the start already holds it */
    CHECK_EQUAL(find(&a, &c, (path_query){NULL, "new", 0}, &path), TA_OK);
    CHECK_EQUAL(path.result, TA_ANALYSIS_PATH_FOUND);
    CHECK_EQUAL(path.step_count, 1);
    /* compound: work enters triage by its initial state */
    CHECK_EQUAL(find(&a, &c, (path_query){NULL, "waiting", 0}, &path), TA_OK);
    CHECK_EQUAL(path.result, TA_ANALYSIS_PATH_FOUND);
    CHECK_EQUAL(path.step_count, 4); /* assign, answer, wait */
    if (path.step_count == 4) {
        CHECK(active(&c, path.steps[1].configuration, "triage"));
        CHECK(active(&c, path.steps[1].configuration, "work"));
        CHECK(active(&c, path.steps[3].configuration, "waiting"));
        CHECK(!active(&c, path.steps[3].configuration, "work"));
    }
    /* parallel: entering escalated enters a state in each region */
    CHECK_EQUAL(find(&a, &c, (path_query){NULL, "informed", 0}, &path), TA_OK);
    CHECK_EQUAL(path.step_count, 4); /* assign, escalate, informed */
    if (path.step_count == 4) {
        CHECK(active(&c, path.steps[2].configuration, "investigating"));
        CHECK(active(&c, path.steps[2].configuration, "informing"));
        CHECK(active(&c, path.steps[3].configuration, "investigating"));
        CHECK(active(&c, path.steps[3].configuration, "informed"));
    }
    /* history: its default, answering, from waiting */
    CHECK_EQUAL(find(&a, &c, (path_query){"waiting", "answering", 0}, &path), TA_OK);
    CHECK_EQUAL(path.result, TA_ANALYSIS_PATH_FOUND);
    CHECK_EQUAL(path.step_count, 2);
    if (path.step_count == 2) {
        CHECK(active(&c, path.steps[0].configuration, "waiting"));
        CHECK(active(&c, path.steps[1].configuration, "answering"));
        CHECK(!active(&c, path.steps[1].configuration, "triage"));
    }
    /* entered at a state of a region: the other region by its initial state */
    CHECK_EQUAL(find(&a, &c, (path_query){"fixed", "fixed", 0}, &path), TA_OK);
    CHECK(path.step_count == 1 && active(&c, path.steps[0].configuration, "informing") &&
          active(&c, path.steps[0].configuration, "escalated"));
    /* nothing leaves spinning */
    CHECK_EQUAL(find(&a, &c, (path_query){"spinning", "closed", 0}, &path), TA_OK);
    CHECK_EQUAL(path.result, TA_ANALYSIS_PATH_UNREACHABLE);
    CHECK_EQUAL(path.explored, 1);
    /* the actor ends at closed: no step from there */
    CHECK_EQUAL(find(&a, &c, (path_query){"closed", "new", 0}, &path), TA_OK);
    CHECK_EQUAL(path.result, TA_ANALYSIS_PATH_UNREACHABLE);
    /* a limit too small to get there */
    CHECK_EQUAL(find(&a, &c, (path_query){NULL, "informed", 3}, &path), TA_OK);
    CHECK_EQUAL(path.result, TA_ANALYSIS_PATH_LIMIT);
    /* refusals: a history state, a state out of range */
    CHECK_EQUAL(find(&a, &c, (path_query){NULL, "resume", 0}, &path), TA_ERROR_INVALID);
    ta_analysis_path_request outside = {c.view, 0, TA_NIL, 1000, 0, &a.arena};
    CHECK_EQUAL(ta_analysis_path_find(&outside, &path), TA_ERROR_INVALID);
    ta_growable_arena_free(&a.arena);
    free(c.bytes);
}

/* ---- names and running out of memory ------------------------------------------------------------ */

static void test_names(void) {
    for (uint32_t rule = 0; rule < TA_ANALYSIS_RULE_COUNT; rule++) {
        CHECK(ta_analysis_rule_name(rule).length > 0);
        CHECK_EQUAL(ta_analysis_rule_named(ta_analysis_rule_name(rule)), rule);
    }
    CHECK_EQUAL(ta_analysis_rule_named((ta_string_view){"door", 4}), TA_NIL);
    CHECK(ta_analysis_descriptor_covers((ta_string_view){"order", 5}, (ta_string_view){"order.paid", 10}));
    CHECK(ta_analysis_descriptor_covers((ta_string_view){"*", 1}, (ta_string_view){"order.paid", 10}));
    CHECK(!ta_analysis_descriptor_covers((ta_string_view){"order", 5}, (ta_string_view){"orders", 6}));
    CHECK(!ta_analysis_descriptor_covers((ta_string_view){"order.paid", 10}, (ta_string_view){"order", 5}));
}

/* Every budget below what lint and path need is refused with
 * TA_ERROR_NO_MEMORY, never more; then they succeed. */
static void test_out_of_memory(void) {
    compiled c;
    if (!compile("ticket.scxml", ticket, NULL, &c)) {
        CHECK(0);
        return;
    }
    int linted = 0, found = 0;
    for (size_t budget = 0; budget < (1u << 22) && !(linted && found); budget += 512) {
        test_arena a;
        arena_begin(&a, budget);
        ta_analysis_findings findings;
        ta_status status = lint(&a, &c, TA_ANALYSIS_SYSTEM, &findings);
        CHECK(status == TA_OK || status == TA_ERROR_NO_MEMORY);
        if (status == TA_OK) linted = 1;
        ta_growable_arena_free(&a.arena);
        arena_begin(&a, budget);
        ta_analysis_path path;
        status = find(&a, &c, (path_query){NULL, "informed", 0}, &path);
        CHECK(status == TA_OK || status == TA_ERROR_NO_MEMORY);
        if (status == TA_OK) {
            CHECK_EQUAL(path.result, TA_ANALYSIS_PATH_FOUND);
            found = 1;
        }
        ta_growable_arena_free(&a.arena);
    }
    CHECK(linted && found);
    free(c.bytes);
}

int main(void) {
    RUN(test_order_findings);
    RUN(test_ticket_findings);
    RUN(test_quiet_on_a_well_chart);
    RUN(test_events_between_charts);
    RUN(test_computed_events_are_unknown);
    RUN(test_paths);
    RUN(test_names);
    RUN(test_out_of_memory);
    TEST_MAIN_END();
}

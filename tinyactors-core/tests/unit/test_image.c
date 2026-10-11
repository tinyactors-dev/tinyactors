/* ta_image: verification of valid and hostile images, event-name lookup. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include "image/records.h"

static const char *CHART =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='a' datamodel='null'>"
    "<state id='a'><onentry><raise event='x.y'/><send event='z' delay='1s'/></onentry>"
    "<transition event='x' target='b'><log expr='1'/></transition></state>"
    "<state id='b'><transition event='z.*' target='a'/></state></scxml>";

static void rehash(unsigned char *b, size_t length) {
    ta_header_record *header = (ta_header_record *)(void *)b;
    header->hash = ta_image_hash(b, (uint32_t)length);
}

static const ta_section_record *section(const unsigned char *b, uint32_t kind) {
    const ta_header_record *header = (const ta_header_record *)(const void *)b;
    const ta_section_record *s = (const ta_section_record *)(const void *)(b + sizeof *header);
    for (uint32_t i = 0; i < header->section_count; i++)
        if (s[i].kind == kind) return &s[i];
    return NULL;
}

static void test_valid_and_lookup(void) {
    size_t length;
    unsigned char *b = (unsigned char *)compile_chart("t.scxml", CHART, NULL, &length, 0);
    CHECK(b != NULL);
    ta_image image;
    CHECK_EQUAL(ta_image_open(b, length, &image), TA_OK);
    const ta_image_view *view = ta_image_view_of(&image);
    int exact = 0;
    uint32_t xy = ta_event_name_lookup(view, "x.y", 3, &exact);
    CHECK(exact);
    ta_string_view n = ta_image_event_name(&image, xy);
    CHECK(n.length == 3 && memcmp(n.pointer, "x.y", 3) == 0);
    uint32_t x = ta_event_name_lookup(view, "x", 1, &exact);
    CHECK(exact && view->event_nodes[x].end > xy && x < xy);
    CHECK_EQUAL(ta_event_name_lookup(view, "x.y.deeper", 10, &exact), xy);
    CHECK(!exact);
    CHECK_EQUAL(ta_event_name_lookup(view, "unknown", 7, &exact), 0); /* root: matches only '*' */
    CHECK(!exact);
    CHECK_EQUAL(ta_event_name_lookup(view, "", 0, &exact), 0);
    CHECK_EQUAL(ta_event_name_lookup(view, "x.", 2, &exact), x);
    CHECK(!exact);
    ta_image_info info;
    ta_image_get_info(&image, &info);
    CHECK(info.content_hash != 0);
    free(b);
}

/* the state tree: parents (chart-relative), kinds, depths, ids in document order */
static void test_state_info(void) {
    size_t length;
    unsigned char *b = (unsigned char *)compile_chart(
        "t.scxml",
        "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='p' datamodel='null'>"
        "<parallel id='p'><state id='r1'><state id='x'/><history id='h' type='deep'><transition "
        "target='x'/></history></state>"
        "<state id='r2'/></parallel><final id='f'/></scxml>",
        NULL, &length, 0);
    CHECK(b != NULL);
    ta_image image;
    CHECK_EQUAL(ta_image_open(b, length, &image), TA_OK);
    ta_state_info state_info;
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){0, 0}, &state_info), TA_OK);
    CHECK(state_info.size == sizeof state_info && state_info.parent == TA_NIL &&
          state_info.depth == 0);
    uint32_t p = ta_image_state_by_name(&image, 0, (ta_string_view){"p", 1});
    uint32_t r1 = ta_image_state_by_name(&image, 0, (ta_string_view){"r1", 2});
    uint32_t x = ta_image_state_by_name(&image, 0, (ta_string_view){"x", 1});
    uint32_t h = ta_image_state_by_name(&image, 0, (ta_string_view){"h", 1});
    uint32_t f = ta_image_state_by_name(&image, 0, (ta_string_view){"f", 1});
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){0, p}, &state_info), TA_OK);
    CHECK(state_info.parent == 0 && state_info.kind == TA_STATE_PARALLEL && state_info.depth == 1);
    CHECK(state_info.id.length == 1 && state_info.id.pointer[0] == 'p');
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){0, r1}, &state_info), TA_OK);
    CHECK(state_info.parent == p && state_info.kind == TA_STATE_COMPOUND && state_info.depth == 2 &&
          r1 > p);
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){0, x}, &state_info), TA_OK);
    CHECK(state_info.parent == r1 && state_info.kind == TA_STATE_ATOMIC && state_info.depth == 3);
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){0, h}, &state_info), TA_OK);
    CHECK(state_info.parent == r1 && state_info.kind == TA_STATE_HISTORY_DEEP);
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){0, f}, &state_info), TA_OK);
    CHECK(state_info.parent == 0 && state_info.kind == TA_STATE_FINAL);
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){0, ta_image_state_count(&image, 0)},
                                    &state_info),
                TA_ERROR_INVALID);
    CHECK_EQUAL(ta_image_state_info(&image, (ta_chart_item){1, 0}, &state_info), TA_ERROR_INVALID);
    free(b);
}

static void test_rejects(void) {
    size_t length;
    unsigned char *b = (unsigned char *)compile_chart("t.scxml", CHART, NULL, &length, 0);
    unsigned char *w = (unsigned char *)aligned_alloc(8, length + 8);
    ta_image image;

    memcpy(w, b, length);
    CHECK_EQUAL(ta_image_open(w, length - 8, &image), TA_ERROR_INVALID); /* truncated */
    memcpy(w + 1, b, length);
    CHECK_EQUAL(ta_image_open(w + 1, length, &image), TA_ERROR_INVALID); /* misaligned */
    memcpy(w, b, length);
    w[length / 2] ^= 0x40;
    CHECK_EQUAL(ta_image_open(w, length, &image), TA_ERROR_INVALID); /* hash mismatch */

    /* structurally invalid but correctly hashed images */
    const ta_section_record *ss = section(b, TA_SECTION_STATES);
    const ta_section_record *cs = section(b, TA_SECTION_CODE);
    const ta_section_record *ts = section(b, TA_SECTION_STRINGS);
    memcpy(w, b, length);
    ((ta_state_record *)(void *)(w + ss->offset))[1].parent = 99;
    rehash(w, length);
    CHECK_EQUAL(ta_image_open(w, length, &image), TA_ERROR_INVALID);

    memcpy(w, b, length);
    ((uint32_t *)(void *)(w + cs->offset))[0] = 200; /* bad opcode */
    rehash(w, length);
    CHECK_EQUAL(ta_image_open(w, length, &image), TA_ERROR_INVALID);

    memcpy(w, b, length);
    *(uint32_t *)(void *)(w + ts->offset) = 0x7FFFFFF0u; /* string length past section */
    rehash(w, length);
    CHECK_EQUAL(ta_image_open(w, length, &image), TA_ERROR_INVALID);

    memcpy(w, b, length);
    ((ta_header_record *)(void *)w)->section_count = 1000;
    rehash(w, length);
    CHECK_EQUAL(ta_image_open(w, length, &image), TA_ERROR_INVALID);

    /* the view is cleared on failure */
    const ta_image_view *view = ta_image_view_of(&image);
    CHECK(view->base == NULL);
    free(w);
    free(b);
}

static ta_status open_copy(const unsigned char *w, size_t length) {
    unsigned char *c = (unsigned char *)aligned_alloc(8, length);
    memcpy(c, w, length);
    rehash(c, length);
    ta_image image;
    ta_status status = ta_image_open(c, length, &image);
    free(c);
    return status;
}

/* Jump targets and entry points must be instruction starts. The verifier
 * settles compiler output in one pass (pending forward targets, a stack of
 * <foreach> bodies, ascending entry points) and falls back to decoding from
 * the chart's start for anything else, so it accepts exactly what it did. */
static void test_jump_targets(void) {
    static const char *chart =
        "<scxml initial='a'><state id='a'><onentry><if cond='c'><raise event='r'/>"
        "<elseif cond='d'/><raise event='s'/><else/><foreach array='x' item='i'>"
        "<raise event='t'/></foreach></if></onentry><transition event='e' target='b'>"
        "<raise event='u'/></transition></state><state id='b'><onentry><raise event='v'/>"
        "</onentry><onexit><raise event='w'/></onexit></state></scxml>";
    size_t length;
    unsigned char *b = (unsigned char *)compile_chart("j.scxml", chart, NULL, &length, 0);
    CHECK(b != NULL);
    if (!b) return;
    unsigned char *w = (unsigned char *)aligned_alloc(8, length);
    const ta_section_record *cs = section(b, TA_SECTION_CODE), *ss = section(b, TA_SECTION_STATES);
    const uint32_t *code = (const uint32_t *)(const void *)(b + cs->offset);
    uint32_t n = cs->count, jf = TA_NIL, fenext = TA_NIL;
    for (uint32_t p = 0; p < n; p += ta_opcode_length[code[p]]) {
        if (code[p] == TA_OPCODE_JUMP_IF_FALSE && jf == TA_NIL) jf = p;
        if (code[p] == TA_OPCODE_FOREACH_NEXT) fenext = p;
    }
    CHECK(jf != TA_NIL && fenext != TA_NIL && code[jf + 3] == TA_OPCODE_RAISE);
    CHECK_EQUAL(open_copy(b, length), TA_OK);

    memcpy(w, b, length); /* a forward jump into the operand of the next instruction */
    ((uint32_t *)(void *)(w + cs->offset))[jf + 2] = jf + 4;
    CHECK_EQUAL(open_copy(w, length), TA_ERROR_INVALID);

    memcpy(w, b, length); /* a back jump into the operand of the body's first instruction */
    ((uint32_t *)(void *)(w + cs->offset))[fenext + 1] += 1;
    CHECK_EQUAL(open_copy(w, length), TA_ERROR_INVALID);

    memcpy(w, b, length); /* a back jump to another instruction start: still allowed */
    ((uint32_t *)(void *)(w + cs->offset))[fenext + 1] = jf;
    CHECK_EQUAL(open_copy(w, length), TA_OK);

    ta_state_record *state = (ta_state_record *)(void *)(w + ss->offset);
    memcpy(w, b, length); /* entry points out of order: still allowed */
    uint32_t t = state[1].onentry;
    state[1].onentry = state[2].onentry;
    state[2].onentry = t;
    CHECK_EQUAL(open_copy(w, length), TA_OK);
    memcpy(w, b, length);
    state[2].onexit = state[2].onentry + 1; /* the middle of an instruction */
    CHECK_EQUAL(open_copy(w, length), TA_ERROR_INVALID);
    free(w);
    free(b);
}

/* A chart with a top-level <script> whose code holds a bad opcode: rejected
 * (its entry point used to be decoded before the opcodes were checked: an
 * out-of-bounds read, and a hang when the length read was 0). */
static void test_script_bad_opcode(void) {
    size_t length;
    unsigned char *b =
        (unsigned char *)compile_chart("s.scxml",
                                       "<scxml><script>1</script><state id='a'><onentry><raise "
                                       "event='e'/></onentry></state></scxml>",
                                       NULL, &length, 0);
    CHECK(b != NULL);
    if (!b) return;
    const ta_section_record *cs = section(b, TA_SECTION_CODE);
    CHECK_EQUAL(((const uint32_t *)(const void *)(b + cs->offset))[0], TA_OPCODE_BLOCK);
    const ta_section_record *chs = section(b, TA_SECTION_CHARTS);
    CHECK(((const ta_chart_record *)(const void *)(b + chs->offset))->script > 0); /* decoded to */
    for (uint32_t opcode = TA_OPCODE_MAX_; opcode < 256; opcode += 13) {
        ((uint32_t *)(void *)(b + cs->offset))[0] = opcode;
        CHECK_EQUAL(open_copy(b, length), TA_ERROR_INVALID);
    }
    free(b);
}

/* Code the compiler never emits: more nesting than the verifier's bounded
 * stacks hold. It verifies like before, through the fallback. */
static void test_deep_code(void) {
    char document[8192];
    size_t k = (size_t)snprintf(document, sizeof document, "<scxml><state id='a'><onentry>");
    for (int i = 0; i < 300; i++)
        k += (size_t)snprintf(document + k, sizeof document - k, "<raise event='e'/>");
    snprintf(document + k, sizeof document - k, "</onentry><transition cond='c' target='a'/></state></scxml>");
    size_t length;
    unsigned char *b = (unsigned char *)compile_chart("d.scxml", document, NULL, &length, 0);
    CHECK(b != NULL);
    if (!b) return;
    const ta_section_record *cs = section(b, TA_SECTION_CODE);
    CHECK_EQUAL(cs->count, 603); /* BLOCK, 300 x RAISE, END */
    unsigned char *w = (unsigned char *)aligned_alloc(8, length);
    uint32_t *c = (uint32_t *)(void *)(w + cs->offset);

    /* 70 nested <foreach> (the body stack holds TA_IMAGE_MAX_NESTING) */
    memcpy(w, b, length);
    c[0] = TA_OPCODE_BLOCK;
    c[1] = 602;
    for (uint32_t i = 0; i < 70; i++) {
        uint32_t at = 2 + 5 * i, next = 352 + 2 * (69 - i);
        c[at] = TA_OPCODE_FOREACH_BEGIN;
        c[at + 1] = 0;
        c[at + 2] = 0;
        c[at + 3] = TA_NIL;
        c[at + 4] = next + 2;
        c[next] = TA_OPCODE_FOREACH_NEXT;
        c[next + 1] = at + 5;
    }
    for (uint32_t p = 492; p < 603; p++)
        c[p] = TA_OPCODE_END;
    CHECK_EQUAL(open_copy(w, length), TA_OK);
    c[352 + 2 * 1 + 1] += 1; /* an inner one (beyond the stack): into an operand */
    CHECK_EQUAL(open_copy(w, length), TA_ERROR_INVALID);
    c[352 + 2 * 1 + 1] -= 1;
    c[352 + 2 * 69 + 1] += 1; /* the outermost one */
    CHECK_EQUAL(open_copy(w, length), TA_ERROR_INVALID);

    /* 150 pending forward jumps (more than the pending stack holds) */
    memcpy(w, b, length);
    for (uint32_t i = 0; i < 150; i++) {
        c[2 + 2 * i] = TA_OPCODE_JUMP;
        c[3 + 2 * i] = 2 + 2 * (299 - i);
    }
    CHECK_EQUAL(open_copy(w, length), TA_OK);
    c[3 + 2 * 140] += 1; /* checked by the fallback */
    CHECK_EQUAL(open_copy(w, length), TA_ERROR_INVALID);
    c[3 + 2 * 140] -= 1;
    c[3 + 2 * 5] += 1; /* checked in the pass */
    CHECK_EQUAL(open_copy(w, length), TA_ERROR_INVALID);
    free(w);
    free(b);
}

/* Random single-word mutations (re-hashed): open must never crash and must
 * either reject or produce a self-consistent view. Run under ASan/UBSan. */
static void test_fuzz_mutations(void) {
    size_t length;
    unsigned char *b = (unsigned char *)compile_chart("t.scxml", CHART, NULL, &length, 0);
    unsigned char *w = (unsigned char *)aligned_alloc(8, length);
    uint32_t seed = 0x12345678u, accepted = 0, rejected = 0;
    for (int iter = 0; iter < 20000; iter++) {
        memcpy(w, b, length);
        int muts = 1 + (int)(seed % 3);
        for (int m = 0; m < muts; m++) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t at = (64 + seed % (uint32_t)(length - 64)) & ~3u;
            seed = seed * 1664525u + 1013904223u;
            uint32_t value = (seed & 1) ? (seed >> 8) % 64 : (seed >> 4);
            memcpy(w + at, &value, 4);
        }
        rehash(w, length);
        ta_image image;
        if (ta_image_open(w, length, &image) == TA_OK) {
            accepted++;
            const ta_image_view *view = ta_image_view_of(&image);
            /* touch every string and event node through public accessors */
            for (uint32_t i = 0; i < view->event_node_count; i++)
                (void)ta_image_event_name(&image, i).length;
            for (uint32_t c = 0; c < view->chart_count; c++)
                for (uint32_t s = 0; s < view->charts[c].state_count; s++)
                    (void)ta_image_state_name(&image, c, s).length;
        } else {
            rejected++;
        }
    }
    CHECK(rejected > 0);
    fprintf(stderr, "  fuzz: %u accepted, %u rejected\n", accepted, rejected);
    free(w);
    free(b);
}

int main(void) {
    RUN(test_valid_and_lookup);
    RUN(test_state_info);
    RUN(test_rejects);
    RUN(test_jump_targets);
    RUN(test_deep_code);
    RUN(test_script_bad_opcode);
    RUN(test_fuzz_mutations);
    TEST_MAIN_END();
}

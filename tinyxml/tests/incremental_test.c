/*
 * Incremental consumption: reading a document while it is still being parsed.
 *
 * ADR 0003 serves streaming consumers from the document itself rather than
 * from a token API: `txml_parser_doc` is valid from `txml_parser_init` onward,
 * an element whose end tag has not arrived carries `TXML_NODE_OPEN` and a
 * `txml_subtree_end` of `TXML_INDEX_NONE`, and a move that cannot yet be
 * answered returns `TXML_PENDING` rather than `TXML_NONE`. Those three answers
 * are the whole reason `txml_move` has three values, and nothing else in the
 * suite ever observes them: every other test walks a finished document.
 *
 * ADR 0018 withdrew mark and rewind, so this is consumption without
 * reclamation. The tree only grows, and this file asserts that too — a node
 * count that ever decreased would mean something reclaimed memory that no
 * public call can reclaim.
 *
 * Test programs may use the C library. The library itself may not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/txml.h"

static int failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            failures += 1;                                                   \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------------
 * Fixtures.
 */

static void *heap_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    size_t actual = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    size_t rounded = (size + actual - 1u) / actual * actual;
    return aligned_alloc(actual, rounded == 0u ? actual : rounded);
}

static void heap_free(void *self, const txml_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const txml_allocator_vtable heap_vtable = { .alloc = heap_alloc, .free = heap_free };
static const txml_allocator heap = { .self = NULL, .v = &heap_vtable };

static bool str_is(txml_str text, const char *literal) {
    const size_t length = strlen(literal);
    if (text.length != length) {
        return false;
    }
    return length == 0u || memcmp(text.ptr, literal, length) == 0;
}

/*
 * A log of independent records: <log><e i="0">r0</e>...</log>, written with no
 * whitespace between tags so the node table is exactly three nodes per record
 * and the document node and <log> above them. Under ADR 0017 every whitespace
 * run would otherwise be a node of its own.
 */
#define RECORDS 200u
#define DOCUMENT_BYTES (1u << 20)

enum {
    LOG_DOCUMENT = 0u, /* the document node */
    LOG_ELEMENT = 1u,  /* <log>, open until the very end */
    LOG_FIRST = 2u     /* the first record */
};

static char *build_log(size_t *length_out) {
    const size_t capacity = 64u * (size_t)RECORDS + 32u;
    char *text = (char *)malloc(capacity);
    if (text == NULL) {
        fprintf(stderr, "incremental_test: out of memory\n");
        exit(2);
    }

    int written = snprintf(text, capacity, "<log>");
    size_t length = (size_t)written;
    for (uint32_t i = 0; i < RECORDS; i++) {
        written = snprintf(text + length, capacity - length, "<e i=\"%u\">r%u</e>", i, i);
        length += (size_t)written;
    }
    written = snprintf(text + length, capacity - length, "</log>");
    length += (size_t)written;

    *length_out = length;
    return text;
}

static txml_parser_config log_config(void) {
    txml_parser_config config = {
        .allocator = heap,
        .limits = TXML_LIMITS_DEFAULT,
        .flags = 0u,
        .staging_length = 0u,
        .chunk_size = 4096u,
        /* One node per step, so the test inspects the document between every
         * pair of appends. Without it the parser could swallow the whole
         * input in one step and there would be nothing to interleave with. */
        .step_budget = 1u,
    };
    config.limits.max_document_bytes = DOCUMENT_BYTES;
    return config;
}

/* The two name IDs a consumer resolves once and then keeps. */
typedef struct {
    uint32_t element; /* {}e */
    uint32_t index;   /* {}i */
} record_names;

static record_names resolve_names(const txml_doc *doc) {
    const txml_qname e_q = { TXML_STR(""), TXML_STR("e") };
    const txml_qname i_q = { TXML_STR(""), TXML_STR("i") };
    record_names names;
    names.element = txml_doc_find_name(doc, &e_q);
    names.index = txml_doc_find_name(doc, &i_q);
    return names;
}

/* ------------------------------------------------------------------------
 * A record, read through the cursor API while the parse is still running.
 */

static void check_record(txml_cursor record, uint32_t expected, const record_names *names) {
    char number[16];
    char body[24];
    snprintf(number, sizeof(number), "%u", expected);
    snprintf(body, sizeof(body), "r%u", expected);

    CHECK(txml_kind(record) == TXML_NODE_ELEMENT);
    CHECK((txml_flags(record) & TXML_NODE_OPEN) == 0u);
    CHECK(txml_subtree_end(record) != TXML_INDEX_NONE);
    CHECK(txml_depth(record) == 2u);
    CHECK(txml_node_name_id(record) == names->element);

    txml_str value;
    CHECK(txml_find_attribute(record, names->index, &value));
    CHECK(str_is(value, number));

    txml_cursor text = record;
    CHECK(txml_first_child(&text) == TXML_MOVED);
    CHECK(txml_kind(text) == TXML_NODE_TEXT);
    CHECK(str_is(txml_node_text(text), body));

    /* A closed record is a closed range, and it is exactly the element and
     * its text. */
    CHECK(txml_contains(record, text));
    CHECK(txml_subtree_end(record) == txml_index(record) + 2u);

    /* Positions are always recorded now (ADR 0017), including mid-parse. */
    txml_position at;
    CHECK(txml_node_position(record, &at) == TXML_OK);
    CHECK(at.line == 1u); /* the log is one line */
}

/* ------------------------------------------------------------------------
 * The parse loop: consume each record as it closes.
 */

typedef struct {
    uint32_t consumed;
    uint32_t final_count;
    int pendings;   /* next_sibling answers that needed unparsed input */
    int open_seen;  /* passes where a record was still being built */
} stats;

static void test_incremental_consumption(const char *xml, size_t length, stats *out) {
    txml_parser_config config = log_config();
    txml_parser *parser = NULL;
    if (txml_parser_init(&parser, &config) != TXML_OK) {
        fprintf(stderr, "incremental_test: parser init failed\n");
        failures += 1;
        return;
    }

    memset(out, 0, sizeof(*out));
    record_names names = { TXML_NAME_NONE, TXML_NAME_NONE };
    bool names_resolved = false;

    uint32_t fed = 0u;
    uint32_t next_record = LOG_FIRST;
    uint32_t previous_count = 0u;
    bool done = false;

    for (unsigned long iterations = 0u; !done; iterations++) {
        if (iterations > 10000000u) {
            fprintf(stderr, "incremental_test: the parse made no progress\n");
            failures += 1;
            break;
        }

        const txml_step step = txml_parse_step(parser);
        if (step == TXML_STEP_NEED_INPUT) {
            const uint32_t remaining = (uint32_t)length - fed;
            const uint32_t want = remaining < 8u ? remaining : 8u;
            const txml_bytes piece = { (const uint8_t *)xml + fed, want };
            const uint32_t taken = txml_parse_feed(parser, piece, want == remaining);
            fed += taken;
            if (taken == 0u && want > 0u) {
                fprintf(stderr, "incremental_test: the parser accepted no input\n");
                failures += 1;
                break;
            }
            continue;
        }
        if (step == TXML_STEP_DONE) {
            /* Do not leave yet: the last records are still unconsumed. */
            done = true;
        } else if (step != TXML_STEP_PROGRESS) {
            fprintf(stderr, "incremental_test: the parse did not finish (step %d)\n", (int)step);
            failures += 1;
            break;
        }

        /* The document is readable from init onward, not merely at the end. */
        const txml_doc *view = txml_parser_doc(parser);
        if (view == NULL) {
            fprintf(stderr, "incremental_test: the parser has no document\n");
            failures += 1;
            break;
        }

        const uint32_t count = txml_doc_node_count(view);
        /* Columns are append-only and nothing reclaims them. */
        CHECK(count >= previous_count);
        previous_count = count;

        if (count <= LOG_ELEMENT) {
            continue;
        }

        /* While <log> is open it has no end, and it contains nothing: the
         * header defines containment as false for an open ancestor. */
        const txml_cursor log = txml_cursor_at(view, LOG_ELEMENT);
        if (!done) {
            CHECK((txml_flags(log) & TXML_NODE_OPEN) != 0u);
            CHECK(txml_subtree_end(log) == TXML_INDEX_NONE);
            CHECK(!txml_contains(log, txml_cursor_at(view, LOG_FIRST)));
        }

        /* Consume every record that closed since the last pass. Advancing by
         * subtree_end is the idiom: the next sibling of a closed node is
         * exactly where its range ends. */
        uint32_t last_consumed = TXML_INDEX_NONE;
        while (next_record < count) {
            const txml_cursor record = txml_cursor_at(view, next_record);
            if (txml_kind(record) != TXML_NODE_ELEMENT) {
                break;
            }
            if ((txml_flags(record) & TXML_NODE_OPEN) != 0u) {
                out->open_seen += 1; /* the parser is still building it */
                break;
            }

            if (!names_resolved) {
                names = resolve_names(view);
                CHECK(names.element != TXML_NAME_NONE);
                CHECK(names.index != TXML_NAME_NONE);
                names_resolved = true;
            }

            check_record(record, out->consumed, &names);
            out->consumed += 1u;
            last_consumed = next_record;
            next_record = txml_subtree_end(record);
        }

        /* With the parent still open and nothing parsed past the last record,
         * a sibling move cannot be answered yet. TXML_PENDING is the answer
         * that distinguishes "not yet" from "no"; TXML_NONE here would be a
         * lie a consumer would act on. */
        if (!done && last_consumed != TXML_INDEX_NONE && next_record >= count) {
            txml_cursor last = txml_cursor_at(view, last_consumed);
            CHECK(txml_next_sibling(&last) == TXML_PENDING);
            CHECK(txml_index(last) == last_consumed); /* a pending move does not move */
            out->pendings += 1;
        }
    }

    /* Everything the parser accepted, and nothing more. */
    CHECK(txml_parser_offset(parser) == (uint64_t)fed);
    CHECK((size_t)fed == length);

    txml_doc *doc = NULL;
    if (txml_parser_finish(parser, &doc) != TXML_OK || doc == NULL) {
        fprintf(stderr, "incremental_test: finishing the parse failed\n");
        failures += 1;
        txml_parser_destroy(parser);
        return;
    }

    out->final_count = txml_doc_node_count(doc);

    /* Nothing was reclaimed, so the finished document holds every record:
     * the document node, <log>, and three nodes per record. */
    CHECK(out->final_count == 2u + 3u * RECORDS);

    /* <log> is closed now, and containment answers for real. */
    const txml_cursor log = txml_cursor_at(doc, LOG_ELEMENT);
    CHECK((txml_flags(log) & TXML_NODE_OPEN) == 0u);
    CHECK(txml_subtree_end(log) == out->final_count);
    CHECK(txml_contains(log, txml_cursor_at(doc, LOG_FIRST)));

    /* Two arenas, both the document's, both non-empty, and together inside
     * the budget (ADR 0016). */
    const txml_arena *values = txml_doc_arena(doc, TXML_ARENA_VALUES);
    const txml_arena *tree = txml_doc_arena(doc, TXML_ARENA_TREE);
    CHECK(values != NULL);
    CHECK(tree != NULL);
    CHECK(values != tree); /* two arenas, not one reported twice */
    if (values != NULL && tree != NULL) {
        CHECK(values->used > 0u);
        CHECK(tree->used > 0u);
        CHECK(values->reserved >= values->used);
        CHECK(tree->reserved >= tree->used);
        CHECK((uint64_t)values->used + (uint64_t)tree->used <= (uint64_t)DOCUMENT_BYTES);
    }

    /* Name IDs resolved during the parse still resolve the same afterwards. */
    if (names_resolved) {
        const record_names again = resolve_names(doc);
        CHECK(again.element == names.element);
        CHECK(again.index == names.index);
    }

    txml_doc_destroy(doc);
}

/* ------------------------------------------------------------------------ */

int main(void) {
    size_t length = 0u;
    char *xml = build_log(&length);

    stats result;
    test_incremental_consumption(xml, length, &result);

    /* Every record was seen exactly once, in order. */
    CHECK(result.consumed == RECORDS);
    /* Both mid-parse answers were observed rather than assumed: a record
     * caught while still open, and a sibling move that could not be
     * answered yet. */
    CHECK(result.open_seen > 0);
    CHECK(result.pendings > 0);

    free(xml);

    if (failures != 0) {
        fprintf(stderr, "incremental_test: %d check(s) failed\n", failures);
        return 1;
    }

    printf("incremental_test: ok (%u records)\n", (unsigned)RECORDS);
    return 0;
}

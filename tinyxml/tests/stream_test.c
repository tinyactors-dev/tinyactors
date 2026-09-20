/*
 * Streaming: the parser must not care how its input is cut, and no construct
 * may need to fit in the staging window.
 *
 * Three properties are pinned here.
 *
 *  1. Differential. Every XML input in tests/spec and a corpus of awkward ones
 *     is parsed with feeds of 1, 2, 3 and 7 bytes and all at once, through a
 *     64-byte staging window, and with step budgets of 0, 1 and 3; the
 *     documents (or the errors, with their positions) must be identical.
 *  2. Every cut. Each corpus input is split into two feeds at every byte
 *     position, which puts a boundary inside every delimiter, reference,
 *     multi-byte character and line end.
 *  3. Size. Text, attribute values, comments, CDATA, PIs and DOCTYPEs of
 *     megabytes parse through a 64-byte window, and the content is right.
 *
 * Test programs may use the C library. The library itself may not.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/txml.h"

static int failures;
static long refusals_exercised;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            failures += 1;                                                   \
        }                                                                    \
    } while (0)

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

/* An allocator that refuses exactly one request, the `fail_at`th after it is
 * armed, and serves every other. */
typedef struct {
    long count;
    long fail_at;
    long refused;
    bool armed;
} flaky;

static void *flaky_alloc(void *self, size_t size, size_t alignment) {
    flaky *state = (flaky *)self;
    if (state->armed) {
        state->count += 1;
        if (state->count == state->fail_at) {
            state->refused += 1;
            return NULL;
        }
    }
    return heap_alloc(NULL, size, alignment);
}

static const txml_allocator_vtable flaky_vtable = { .alloc = flaky_alloc, .free = heap_free };

/* ------------------------------------------------------------------------
 * A growable byte buffer.
 */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} buf;

static void buf_put(buf *b, const char *bytes, size_t length) {
    if (b->length + length + 1u > b->capacity) {
        size_t capacity = b->capacity == 0u ? 1024u : b->capacity;
        while (capacity < b->length + length + 1u) {
            capacity *= 2u;
        }
        b->data = (char *)realloc(b->data, capacity);
        if (b->data == NULL) {
            fprintf(stderr, "stream_test: out of memory\n");
            exit(2);
        }
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, bytes, length);
    b->length += length;
    b->data[b->length] = '\0';
}

static void buf_printf(buf *b, const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    buf_put(b, line, n < 0 ? 0u : (size_t)n < sizeof line ? (size_t)n : sizeof line - 1u);
}

static void buf_str(buf *b, txml_str text) {
    for (uint32_t i = 0; i < text.length; i++) {
        const unsigned char c = (unsigned char)text.ptr[i];
        if (c >= 0x20u && c < 0x7fu && c != '\\') {
            buf_put(b, (const char *)&c, 1u);
        } else {
            buf_printf(b, "\\x%02x", c);
        }
    }
}

/* ------------------------------------------------------------------------
 * Dumping a document: everything a reader can see, so two parses that agree
 * here agree.
 */

static void render_xml(buf *out, const txml_doc *doc) {
    unsigned char bytes[256];
    txml_writer_config config = {
        .allocator = heap,
        .output = { .ptr = bytes, .capacity = (uint32_t)sizeof(bytes) },
        .limits = TXML_LIMITS_DEFAULT,
        .flags = TXML_WRITE_EMPTY_TAGS,
    };
    txml_writer *writer = NULL;
    if (txml_writer_init(&writer, &config) != TXML_OK) {
        buf_printf(out, "<writer init failed>");
        return;
    }
    txml_result result = txml_write_subtree(writer, txml_doc_root(doc));
    while (result == TXML_NEED_SPACE) {
        const txml_bytes produced = txml_writer_output(writer);
        buf_put(out, (const char *)produced.ptr, produced.length);
        result = txml_writer_drain(writer);
    }
    if (result == TXML_OK) {
        result = txml_writer_finish(writer);
        while (result == TXML_NEED_SPACE) {
            const txml_bytes produced = txml_writer_output(writer);
            buf_put(out, (const char *)produced.ptr, produced.length);
            result = txml_writer_drain(writer);
        }
    }
    if (result != TXML_OK) {
        buf_printf(out, "<writer failed %d>", (int)result);
    } else {
        const txml_bytes rest = txml_writer_output(writer);
        buf_put(out, (const char *)rest.ptr, rest.length);
        txml_writer_drain(writer);
    }
    txml_writer_destroy(writer);
}

static void dump_doc(buf *out, const txml_doc *doc) {
    const uint32_t count = txml_doc_node_count(doc);

    buf_printf(out, "nodes=%u names=%u\n", count, txml_doc_name_count(doc));
    for (uint32_t i = 0; i < count; i++) {
        const txml_cursor c = txml_cursor_at(doc, i);
        txml_position at = { 0, 0, 0 };
        txml_node_position(c, &at);
        buf_printf(out, "%u k=%d d=%u f=%u end=%u name=%u @%u:%u+%llu prefix=", i, (int)txml_kind(c), txml_depth(c),
                   txml_flags(c), txml_subtree_end(c), txml_node_name_id(c), at.line, at.column,
                   (unsigned long long)at.offset);
        buf_str(out, txml_node_prefix(c));
        buf_printf(out, " text=");
        buf_str(out, txml_node_text(c));
        buf_printf(out, "\n");

        txml_attr_cursor attribute;
        txml_move move = txml_first_attribute(c, &attribute);
        while (move == TXML_MOVED) {
            txml_attribute_view view;
            if (txml_attribute_read(attribute, &view) == TXML_OK) {
                buf_printf(out, "  a name=%u f=%u value=", view.name, view.flags);
                buf_str(out, view.value);
                buf_printf(out, "\n");
            }
            move = txml_next_attribute(&attribute);
        }
    }
    for (uint32_t id = 0; id < txml_doc_name_count(doc); id++) {
        txml_qname name;
        if (txml_doc_name_at(doc, id, &name) == TXML_OK) {
            buf_printf(out, "name %u {", id);
            buf_str(out, name.ns);
            buf_printf(out, "}");
            buf_str(out, name.local);
            buf_printf(out, "\n");
        }
    }
    buf_printf(out, "xml=");
    render_xml(out, doc);
    buf_printf(out, "\n");
}

/* ------------------------------------------------------------------------
 * Driving a parser with a chosen feed plan.
 */

typedef struct {
    uint32_t chunk;   /* bytes per feed; 0 means all that remain */
    uint32_t cut;     /* when nonzero, the first feed ends here */
    uint32_t staging; /* staging_length; 0 selects the default */
    uint32_t budget;  /* step_budget */
    txml_limits limits;
    size_t chunk_size;  /* arena chunk size; 0 selects the default */
    flaky *refuser;     /* when set, the allocator that refuses one request */
} plan;

static plan default_plan(void) {
    plan p = { 0u, 0u, 0u, 0u, TXML_LIMITS_DEFAULT, 0u, NULL };
    return p;
}

/* Returns the finished document, or NULL with `error` filled in when the parse
 * failed, or NULL with `stuck` set when the driver itself gave up. */
static txml_doc *drive(const uint8_t *xml, size_t length, const plan *p, txml_document_error *error, bool *failed) {
    txml_parser_config config = {
        .allocator = p->refuser != NULL ? (txml_allocator){ p->refuser, &flaky_vtable } : heap,
        .limits = p->limits,
        .staging_length = p->staging,
        .chunk_size = p->chunk_size,
        .step_budget = p->budget,
    };
    txml_parser *parser = NULL;
    size_t fed = 0u;
    bool final_sent = false;

    *failed = false;
    CHECK(txml_parser_init(&parser, &config) == TXML_OK);
    if (p->refuser != NULL) {
        p->refuser->armed = true;
    }
    for (unsigned long iterations = 0u;; iterations++) {
        if (iterations > 50000000ul) {
            fprintf(stderr, "stream_test: the parser made no progress\n");
            failures += 1;
            txml_parser_destroy(parser);
            return NULL;
        }
        const txml_step step = txml_parse_step(parser);
        if (step == TXML_STEP_PROGRESS) {
            continue;
        }
        if (step == TXML_STEP_NEED_INPUT) {
            if (final_sent) {
                fprintf(stderr, "stream_test: NEED_INPUT after the final chunk\n");
                failures += 1;
                txml_parser_destroy(parser);
                return NULL;
            }
            size_t want = length - fed;
            if (p->cut != 0u && fed < p->cut) {
                want = p->cut - fed;
            } else if (p->chunk != 0u && want > p->chunk) {
                want = p->chunk;
            }
            const uint32_t taken = txml_parse_feed(parser, (txml_bytes){ xml + fed, (uint32_t)want }, fed + want == length);
            fed += taken;
            final_sent = fed == length && taken == want;
            if (taken == 0u && want != 0u) {
                fprintf(stderr, "stream_test: the parser accepted no input\n");
                failures += 1;
                txml_parser_destroy(parser);
                return NULL;
            }
            continue;
        }
        if (step == TXML_STEP_FAILED) {
            CHECK(txml_parser_error(parser, error) == TXML_OK);
            *failed = true;
            txml_parser_destroy(parser);
            return NULL;
        }
        if (step == TXML_STEP_NEED_MEMORY && p->refuser != NULL) {
            continue; /* the allocator only refuses once; retry */
        }
        if (step != TXML_STEP_DONE) {
            fprintf(stderr, "stream_test: unexpected step %d\n", (int)step);
            failures += 1;
            txml_parser_destroy(parser);
            return NULL;
        }
        break;
    }
    txml_doc *doc = NULL;
    CHECK(txml_parser_finish(parser, &doc) == TXML_OK);
    return doc;
}

/* The outcome of a parse as text: the dump, or the error and where. */
static void outcome_text(buf *out, const uint8_t *xml, size_t length, const plan *p) {
    txml_document_error error;
    bool failed = false;
    txml_doc *doc = drive(xml, length, p, &error, &failed);

    if (doc != NULL) {
        dump_doc(out, doc);
        txml_doc_destroy(doc);
    } else if (failed) {
        buf_printf(out, "error %d limit=%d at %u:%u+%llu\n", (int)error.code, (int)error.limit, error.at.line,
                   error.at.column, (unsigned long long)error.at.offset);
    } else {
        buf_printf(out, "<driver gave up>\n");
    }
}

/* ------------------------------------------------------------------------
 * The corpus.
 */

static const char *const corpus[] = {
    "<a/>",
    "<a>text</a>",
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<a b='1'  c=\"2\"/>\n",
    "<a>one &amp; two &lt;three&gt; &#65;&#x42;&#x1F600; &apos;q&quot;</a>",
    "<a>\r\nline\rend\r\n\r\nx</a>",
    "<a k=\"a\r\nb\rc\nd\te\">v</a>",
    "<a>x]y]]z]]]w&gt;</a>",
    "<a><![CDATA[x]y]]z]]]]><![CDATA[]]><![CDATA[a\r\nb]]></a>",
    "<a><!-- c --><!----><!-- a-b --><!--\r\n--></a>",
    "<a><?t?><?t d?><?pi a?b ?c ?\?>\r\n<?q  spaced ?></a>",
    "<!-- pro --><?pro log?>\n<a/>\n<!-- epi --><?epi log?>\n",
    "<a xmlns=\"u\" xmlns:p=\"v\"><p:b p:k=\"1\" k=\"2\"/><b xmlns=\"\"/></a>",
    "<p:a p:k=\"1\" xmlns:p=\"urn:x\"><q:b xmlns:q=\"urn:x\" q:k=\"2\"/></p:a>",
    "<a xml:lang=\"en\"><b xml:space=\"preserve\"/></a>",
    "<a>caf\xc3\xa9 \xe2\x98\x83 \xf0\x9f\x98\x80 done</a>",
    "<a k=\"caf\xc3\xa9 \xe2\x98\x83\">\xf0\x9f\x98\x80</a>",
    "<\xc3\xa9l\xc3\xa9ment \xe2\x82\xac=\"1\"/>",
    "<!DOCTYPE a SYSTEM \"a.dtd\"><a/>",
    "<!DOCTYPE a PUBLIC \"-//x//y\" 'a>b.dtd'>\n<a/>",
    "<!DOCTYPE a [<!ENTITY e \"v\"><!ENTITY f 'w&e;'>]><a k=\"&e;&f;\">&e;&f;</a>",
    "<!DOCTYPE a [<!ATTLIST a k CDATA \"d\" j (x|y) #IMPLIED m CDATA #FIXED \"f\">]><a/>",
    "<!DOCTYPE r [<!ATTLIST b k CDATA \"v\" xmlns:p CDATA \"urn:p\">]><r><b/><b k=\"w\"/></r>",
    "<!DOCTYPE a [\r\n<!-- a > ] \" ' comment -->\r\n<?pi > ] \" ' ?>\r\n<!ELEMENT a (#PCDATA|b)*>"
    "<!NOTATION n SYSTEM \"x>y\">%pe;<!ENTITY % p \"z\"><!ENTITY q \"&#38;#60;\">]><a>&q;</a>",
    "<!DOCTYPE a [<!ENTITY e \"a&#x26;#65;b\">]><a>&e;</a>",
    "<!DOCTYPE a [<!ENTITY e \"x\"><!ENTITY e \"y\">]><a>&e;</a>",
    "<a>&#0;</a>",
    "<a>&x;</a>",
    "<a></b>",
    "<a><b></a>",
    "<a x=\"1\" x=\"2\"/>",
    "<a x=1/>",
    "<a x=\"<\"/>",
    "<a><!-- a -- b --></a>",
    "<a>1 < 2</a>",
    "<a>]]></a>",
    "<a>\xff</a>",
    "<a>\xe2\x98</a>",
    "<a>\x01</a>",
    "<a/><b/>",
    "<a/>tail",
    "text<a/>",
    "<a",
    "<a>",
    "<a><![CDATA[x",
    "<a><!-- x",
    "<a><?p x",
    "<!DOCTYPE a [<!ENTITY e \"x\">",
    "<?xml version=\"1.0\" encoding=\"latin-1\"?><a/>",
    "<a>&amp</a>",
    "<a k=\"&amp\"/>",
    "<a>&#x110000;</a>",
    "<p:a/>",
};

#define CORPUS_COUNT (sizeof(corpus) / sizeof(corpus[0]))

/* ------------------------------------------------------------------------
 * The spec files' inputs.
 */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void unescape_into(buf *out, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] != '\\' || i + 1u >= length) {
            buf_put(out, text + i, 1u);
            continue;
        }
        const char next = text[++i];
        char c = next;
        if (next == 'n') c = '\n';
        else if (next == 'r') c = '\r';
        else if (next == 't') c = '\t';
        else if (next == 'x' && i + 2u < length && hex_value(text[i + 1u]) >= 0 && hex_value(text[i + 2u]) >= 0) {
            c = (char)((hex_value(text[i + 1u]) << 4) | hex_value(text[i + 2u]));
            i += 2u;
        }
        buf_put(out, &c, 1u);
    }
}

/* Every @xml section of one spec file, appended to `inputs`. */
static size_t collect_spec_inputs(const char *path, buf **inputs, size_t count) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return count;
    }
    char line[4096];
    bool in_xml = false;
    buf current = { NULL, 0, 0 };

    while (fgets(line, sizeof line, file) != NULL) {
        size_t length = strlen(line);
        while (length > 0u && (line[length - 1u] == '\n' || line[length - 1u] == '\r')) {
            length -= 1u;
        }
        if (length > 0u && line[0] == '@') {
            if (in_xml) {
                *inputs = (buf *)realloc(*inputs, (count + 1u) * sizeof(buf));
                (*inputs)[count++] = current;
                current = (buf){ NULL, 0, 0 };
            }
            in_xml = strncmp(line, "@xml", 4) == 0;
            continue;
        }
        if (in_xml) {
            if (current.length > 0u) {
                buf_put(&current, "\n", 1u);
            }
            unescape_into(&current, line, length);
        }
    }
    if (in_xml) {
        *inputs = (buf *)realloc(*inputs, (count + 1u) * sizeof(buf));
        (*inputs)[count++] = current;
    }
    fclose(file);
    return count;
}

/* ------------------------------------------------------------------------
 * Properties 1 and 2.
 */

static void compare(const char *label, const buf *expected, const buf *actual, const uint8_t *xml, size_t length) {
    if (expected->length == actual->length && memcmp(expected->data, actual->data, expected->length) == 0) {
        return;
    }
    failures += 1;
    fprintf(stderr, "stream_test: %s differs for input (%zu bytes): ", label, length);
    for (size_t i = 0; i < length && i < 120u; i++) {
        const unsigned char c = xml[i];
        fputc(c >= 0x20u && c < 0x7fu ? (int)c : '?', stderr);
    }
    fprintf(stderr, "\n--- expected ---\n%.*s\n--- actual ---\n%.*s\n", (int)(expected->length < 1500u ? expected->length : 1500u),
            expected->data, (int)(actual->length < 1500u ? actual->length : 1500u), actual->data);
}

static void check_differential(const uint8_t *xml, size_t length) {
    buf baseline = { NULL, 0, 0 };
    plan whole = default_plan();
    outcome_text(&baseline, xml, length, &whole);

    static const uint32_t chunks[] = { 1u, 2u, 3u, 7u, 0u };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        plan p = default_plan();
        p.chunk = chunks[i];
        p.staging = 64u;
        buf got = { NULL, 0, 0 };
        outcome_text(&got, xml, length, &p);
        compare("chunked parse", &baseline, &got, xml, length);
        free(got.data);
    }
    static const uint32_t budgets[] = { 1u, 3u };
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
        plan p = default_plan();
        p.chunk = 5u;
        p.staging = 64u;
        p.budget = budgets[i];
        buf got = { NULL, 0, 0 };
        outcome_text(&got, xml, length, &p);
        compare("budgeted parse", &baseline, &got, xml, length);
        free(got.data);
    }
    free(baseline.data);
}

static void check_every_cut(const uint8_t *xml, size_t length) {
    buf baseline = { NULL, 0, 0 };
    plan whole = default_plan();
    outcome_text(&baseline, xml, length, &whole);

    for (size_t cut = 1u; cut < length; cut++) {
        plan p = default_plan();
        p.cut = (uint32_t)cut;
        p.staging = 64u;
        buf got = { NULL, 0, 0 };
        outcome_text(&got, xml, length, &p);
        compare("cut parse", &baseline, &got, xml, length);
        free(got.data);
        if (failures > 20) {
            break;
        }
    }
    free(baseline.data);
}

/* Refuse each allocation in turn and retry: the parse must come out exactly as
 * it does when nothing is refused, at every point where memory can run out. */
static void check_memory_retries(const uint8_t *xml, size_t length) {
    plan reference = default_plan();
    reference.chunk_size = 96u;
    reference.staging = 64u;
    reference.chunk = 5u;
    buf expected = { NULL, 0, 0 };
    outcome_text(&expected, xml, length, &reference);

    for (long n = 1;; n++) {
        flaky state = { 0, n, 0, false };
        plan p = reference;
        p.refuser = &state;
        buf got = { NULL, 0, 0 };
        outcome_text(&got, xml, length, &p);
        if (state.refused == 0) {
            free(got.data);
            break; /* n is past the last allocation */
        }
        refusals_exercised += 1;
        compare("parse after a refused allocation", &expected, &got, xml, length);
        free(got.data);
        if (failures > 20 || n > 2000) {
            break;
        }
    }
    free(expected.data);
}

static void test_corpus(void) {
    for (size_t i = 0; i < CORPUS_COUNT; i++) {
        check_differential((const uint8_t *)corpus[i], strlen(corpus[i]));
        check_every_cut((const uint8_t *)corpus[i], strlen(corpus[i]));
        check_memory_retries((const uint8_t *)corpus[i], strlen(corpus[i]));
    }
}

static void test_spec_inputs(const char *directory) {
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        fprintf(stderr, "stream_test: cannot open %s\n", directory);
        failures += 1;
        return;
    }
    buf *inputs = NULL;
    size_t count = 0u;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const size_t name = strlen(entry->d_name);
        if (name > 5u && strcmp(entry->d_name + name - 5u, ".spec") == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
            count = collect_spec_inputs(path, &inputs, count);
        }
    }
    closedir(dir);
    CHECK(count > 50u);
    for (size_t i = 0; i < count; i++) {
        check_differential((const uint8_t *)inputs[i].data, inputs[i].length);
        free(inputs[i].data);
    }
    free(inputs);
    printf("stream_test: %zu spec inputs, %zu corpus inputs, %ld refused allocations retried\n", count, (size_t)CORPUS_COUNT,
           refusals_exercised);
}

/* ------------------------------------------------------------------------
 * Property 3: size.
 */

/* `unit` repeated until the buffer holds `total` bytes, between a prefix and a suffix. */
static buf make_document(const char *prefix, const char *unit, size_t total, const char *suffix) {
    buf b = { NULL, 0, 0 };
    const size_t unit_length = strlen(unit);

    buf_put(&b, prefix, strlen(prefix));
    for (size_t written = 0; written < total; written += unit_length) {
        buf_put(&b, unit, unit_length);
    }
    buf_put(&b, suffix, strlen(suffix));
    return b;
}

/* Does `got` hold `unit` repeated exactly `repeats` times? */
static bool str_equals_literal(txml_str text, const char *literal) {
    return text.length == strlen(literal) && memcmp(text.ptr, literal, text.length) == 0;
}

static bool is_repetition(txml_str got, const char *unit, size_t repeats) {
    const size_t unit_length = strlen(unit);
    if (got.length != unit_length * repeats) {
        return false;
    }
    for (size_t i = 0; i < repeats; i++) {
        if (memcmp(got.ptr + i * unit_length, unit, unit_length) != 0) {
            return false;
        }
    }
    return true;
}

/* `wide` runs with the default 64 KiB window and everything offered at once,
 * which is where work that grows with the window (a position recomputed per
 * reference, say) shows; otherwise a 64-byte window and 4 KiB feeds. */
static txml_doc *parse_big_with(const buf *xml, bool wide, bool *failed, txml_document_error *error) {
    plan p = default_plan();
    p.chunk = wide ? 0u : 4096u;
    p.staging = wide ? 0u : 64u;
    p.limits.max_document_bytes = 512u * 1024u * 1024u;
    return drive((const uint8_t *)xml->data, xml->length, &p, error, failed);
}

static txml_doc *parse_big(const buf *xml, bool *failed, txml_document_error *error) {
    return parse_big_with(xml, false, failed, error);
}

typedef struct {
    const char *name;
    const char *prefix;
    const char *raw_unit;     /* what is written, repeated */
    const char *decoded_unit; /* what a reader sees, repeated */
    size_t total;
    const char *suffix;
    uint32_t node;            /* which node holds the content */
    bool attribute;           /* the content is the first attribute's value */
} big_case;

static void test_big(const big_case *item, bool wide) {
    buf xml = make_document(item->prefix, item->raw_unit, item->total, item->suffix);
    const size_t repeats = (item->total + strlen(item->raw_unit) - 1u) / strlen(item->raw_unit);
    bool failed = false;
    txml_document_error error;
    const clock_t started = clock();
    txml_doc *doc = parse_big_with(&xml, wide, &failed, &error);
    const double seconds = (double)(clock() - started) / (double)CLOCKS_PER_SEC;

    if (doc == NULL) {
        fprintf(stderr, "stream_test: %s: the parse %s (code %d)\n", item->name, failed ? "failed" : "gave up", (int)error.code);
        failures += 1;
        free(xml.data);
        return;
    }
    /* Generous: this is about growth, not speed. */
    if (seconds > 10.0) {
        fprintf(stderr, "stream_test: %s took %.1f s%s\n", item->name, seconds, wide ? " with the default window" : "");
        failures += 1;
    }
    const txml_cursor node = txml_cursor_at(doc, item->node);
    txml_str got;
    if (item->attribute) {
        txml_attr_cursor attribute;
        txml_attribute_view view;
        got = (txml_str){ "", 0u };
        if (txml_first_attribute(node, &attribute) == TXML_MOVED && txml_attribute_read(attribute, &view) == TXML_OK) {
            got = view.value;
        }
    } else {
        got = txml_node_text(node);
    }
    if (!is_repetition(got, item->decoded_unit, repeats)) {
        fprintf(stderr, "stream_test: %s: wrong content (%u bytes, wanted %zu)\n", item->name, got.length,
                repeats * strlen(item->decoded_unit));
        failures += 1;
    }
    txml_doc_destroy(doc);
    free(xml.data);
}

static void test_sizes(void) {
    static const big_case cases[] = {
        { "5 MiB of text", "<a>", "abcdefghij&amp;klm\r\nnopqrs\n", "abcdefghij&klm\nnopqrs\n", 5u << 20, "</a>", 2u, false },
        { "5 MiB attribute value", "<a k=\"", "abcdefghij&amp;klm\r\nnopqrs\t", "abcdefghij&klm nopqrs ", 5u << 20, "\"/>", 1u, true },
        { "2 MiB comment", "<a><!--", "comment - with a-dash\r\n", "comment - with a-dash\n", 2u << 20, "--></a>", 2u, false },
        { "2 MiB CDATA", "<a><![CDATA[", "x]y]]z&amp;<b>\r\n", "x]y]]z&amp;<b>\n", 2u << 20, "]]></a>", 2u, false },
        { "2 MiB PI", "<a><?t ", "data ? with ?questions\r\n", "data ? with ?questions\n", 2u << 20, "?></a>", 2u, false },
        { "2 MiB external id", "<!DOCTYPE a SYSTEM \"", "some/long/path.dtd/", " a SYSTEM \"some/long/path.dtd/", 0u, "\"><a/>", 1u, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].total == 0u) {
            continue;
        }
        test_big(&cases[i], false);
        test_big(&cases[i], true);
    }

    /* A DOCTYPE keeps its text as written, and a subset comment as long as a
     * megabyte streams past without being held. */
    {
        buf xml = make_document("<!DOCTYPE a [<!-- ", "subset comment > ] \" ' text\n", 2u << 20, " -->]><a/>");
        bool failed = false;
        txml_document_error error;
        txml_doc *doc = parse_big(&xml, &failed, &error);
        CHECK(doc != NULL);
        if (doc != NULL) {
            const txml_str text = txml_node_text(txml_cursor_at(doc, 1u));
            CHECK(txml_kind(txml_cursor_at(doc, 1u)) == TXML_NODE_DOCTYPE);
            CHECK(text.length == xml.length - strlen("<!DOCTYPE") - strlen(">") - strlen("<a/>"));
            CHECK(txml_kind(txml_cursor_at(doc, 2u)) == TXML_NODE_ELEMENT);
            txml_doc_destroy(doc);
        }
        free(xml.data);
    }

    /* A large entity through the declaration buffer, referenced many times. */
    {
        buf value = make_document("", "0123456789abcdef", 60u * 1024u, "");
        buf xml = { NULL, 0, 0 };
        buf_printf(&xml, "<!DOCTYPE a [<!ENTITY e \"");
        buf_put(&xml, value.data, value.length);
        buf_printf(&xml, "\">]><a>");
        for (int i = 0; i < 100; i++) {
            buf_printf(&xml, "&e;");
        }
        buf_printf(&xml, "</a>");
        bool failed = false;
        txml_document_error error;
        txml_doc *doc = parse_big(&xml, &failed, &error);
        CHECK(doc != NULL);
        if (doc != NULL) {
            /* doctype, element, one text node */
            CHECK(txml_node_text(txml_cursor_at(doc, 3u)).length == value.length * 100u);
            txml_doc_destroy(doc);
        }
        free(value.data);
        free(xml.data);
    }

    /* A DOCTYPE of many declarations, read through a wide window. */
    {
        buf xml = { NULL, 0, 0 };
        buf_printf(&xml, "<!DOCTYPE a [\n");
        for (int i = 0; i < 20000; i++) {
            buf_printf(&xml, "<!ENTITY e%d \"value %d\"><!ATTLIST b%d k CDATA \"d\">\n", i, i, i);
        }
        buf_printf(&xml, "]><a>&e19999;</a>");
        bool failed = false;
        txml_document_error error;
        const clock_t started = clock();
        txml_doc *doc = parse_big_with(&xml, true, &failed, &error);
        const double seconds = (double)(clock() - started) / (double)CLOCKS_PER_SEC;
        CHECK(doc != NULL);
        CHECK(seconds < 10.0);
        if (doc != NULL) {
            CHECK(str_equals_literal(txml_node_text(txml_cursor_at(doc, 3u)), "value 19999"));
            txml_doc_destroy(doc);
        }
        free(xml.data);
    }

    /* A document whose bytes are all in one huge start tag. */
    {
        buf xml = make_document("<a", " k=\"v\"", 64u * 1024u, "/>");
        plan p = default_plan();
        p.chunk = 100u;
        p.staging = 64u;
        p.limits.max_attributes = 100000u;
        p.limits.max_names = 4096u;
        txml_document_error error;
        bool failed = false;
        txml_doc *doc = drive((const uint8_t *)xml.data, xml.length, &p, &error, &failed);
        /* the attribute repeats, so this is a duplicate-attribute error, found
         * only when the tag ends, however long the tag was */
        CHECK(doc == NULL && failed && error.code == TXML_DOC_DUPLICATE_ATTRIBUTE);
        free(xml.data);
    }
}

/* Roughly linear: 1 MiB fed 64 bytes at a time must not take long. */
static void test_throughput(void) {
    buf xml = make_document("<a>", "0123456789abcdefghij&amp;klmnopqrstuvwxyz<b k=\"v\"/>", 1u << 20, "</a>");
    plan p = default_plan();
    p.chunk = 64u;
    p.staging = 64u;
    txml_document_error error;
    bool failed = false;

    const clock_t started = clock();
    txml_doc *doc = drive((const uint8_t *)xml.data, xml.length, &p, &error, &failed);
    const double seconds = (double)(clock() - started) / (double)CLOCKS_PER_SEC;
    CHECK(doc != NULL);
    if (doc != NULL) {
        txml_doc_destroy(doc);
    }
    printf("stream_test: %.2f MiB in 64-byte feeds: %.3f s (%.1f MiB/s)\n", (double)xml.length / 1048576.0, seconds,
           seconds > 0.0 ? (double)xml.length / 1048576.0 / seconds : 0.0);
    CHECK(seconds < 5.0);
    free(xml.data);
}

int main(int argc, char **argv) {
    const char *spec = argc > 1 ? argv[1] : "tests/spec";

    test_corpus();
    test_spec_inputs(spec);
    test_sizes();
    test_throughput();

    if (failures != 0) {
        fprintf(stderr, "stream_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("stream_test: ok\n");
    return 0;
}

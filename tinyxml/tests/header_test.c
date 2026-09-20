/*
 * The header on its own: layout, constants, and macro ergonomics.
 *
 * This test calls no txml function, so it links and runs before any
 * implementation exists. Everything that can be a compile-time assertion is
 * one; the rest are runtime checks, because TXML_STR and TXML_LIMITS_DEFAULT
 * expand to compound literals, which C does not treat as constant
 * expressions.
 *
 * Test programs may use the C library. The library itself may not.
 */
#include <stddef.h>
#include <stdio.h>

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
 * Layout the design depends on.
 *
 * Cursors are passed and returned by value everywhere, so they must stay
 * cheap: a document pointer plus an index, never more than two words.
 */

_Static_assert(sizeof(txml_str) <= 2u * sizeof(void *), "txml_str must be a two-word view");
_Static_assert(offsetof(txml_str, ptr) == 0u, "txml_str.ptr must come first");
_Static_assert(_Alignof(txml_str) == _Alignof(char *), "txml_str aligns like a pointer");

_Static_assert(sizeof(txml_bytes) == sizeof(txml_str), "txml_bytes mirrors txml_str");
_Static_assert(offsetof(txml_bytes, ptr) == 0u, "txml_bytes.ptr must come first");

_Static_assert(sizeof(txml_qname) == 2u * sizeof(txml_str), "a qname is exactly two strings");
_Static_assert(offsetof(txml_qname, ns) == 0u, "qname namespace comes first");

_Static_assert(sizeof(txml_position) == 16u, "a position is 8 + 4 + 4 bytes");
_Static_assert(offsetof(txml_position, offset) == 0u, "position offset comes first");

_Static_assert(sizeof(txml_cursor) <= 2u * sizeof(void *), "a cursor must stay two words");
_Static_assert(sizeof(txml_attr_cursor) == sizeof(txml_cursor), "both cursors have one shape");
_Static_assert(offsetof(txml_cursor, doc) == 0u, "a cursor starts with its document");

_Static_assert(sizeof(txml_find) == 2u * sizeof(uint32_t), "a find query is two indices");
_Static_assert(sizeof(txml_arena_mark) <= 2u * sizeof(void *), "an arena mark stays small");

/* The allocator and its vtable are fat pointers, like tinyactors-core's. */
_Static_assert(sizeof(txml_allocator) == 2u * sizeof(void *), "an allocator is {self, v}");
_Static_assert(offsetof(txml_allocator, self) == 0u, "allocator self comes first");

/* ------------------------------------------------------------------------
 * Results: TXML_OK is zero, TXML_NEED_SPACE is the only other non-error, and
 * every error is negative and distinct.
 */

_Static_assert(TXML_OK == 0, "TXML_OK must be zero");
_Static_assert(TXML_NEED_SPACE == 1, "TXML_NEED_SPACE must be one");
_Static_assert(TXML_ERR_INVALID < 0, "errors are negative");
_Static_assert(TXML_ERR_NOMEM < 0, "errors are negative");
_Static_assert(TXML_ERR_LIMIT < 0, "errors are negative");
_Static_assert(TXML_ERR_NOT_FOUND < 0, "errors are negative");
_Static_assert(TXML_ERR_PENDING < 0, "errors are negative");
_Static_assert(TXML_ERR_DOCUMENT < 0, "errors are negative");

_Static_assert(TXML_ERR_INVALID != TXML_ERR_NOMEM, "error codes are distinct");
_Static_assert(TXML_ERR_NOMEM != TXML_ERR_LIMIT, "error codes are distinct");
_Static_assert(TXML_ERR_LIMIT != TXML_ERR_NOT_FOUND, "error codes are distinct");
_Static_assert(TXML_ERR_NOT_FOUND != TXML_ERR_PENDING, "error codes are distinct");
_Static_assert(TXML_ERR_PENDING != TXML_ERR_DOCUMENT, "error codes are distinct");
_Static_assert(sizeof(txml_result) == 4u, "a result is an int32_t");

/* ------------------------------------------------------------------------
 * Sentinels.
 */

_Static_assert(TXML_NAME_NONE == UINT32_MAX, "TXML_NAME_NONE is UINT32_MAX");
_Static_assert(TXML_INDEX_NONE == UINT32_MAX, "TXML_INDEX_NONE is UINT32_MAX");
_Static_assert(TXML_ARENA_CHUNK_DEFAULT == 65536u, "the default chunk is 64 KiB");
_Static_assert(TXML_NODE_DOCUMENT == 0, "the document node kind is the first enumerator");
/* ADR 0017: the declaration and the doctype are nodes, appended after the
 * kinds that existed before them so no stored kind value shifts. */
_Static_assert(TXML_NODE_DECLARATION == TXML_NODE_PI + 1, "declaration follows pi");
_Static_assert(TXML_NODE_DOCTYPE == TXML_NODE_DECLARATION + 1, "doctype follows declaration");
_Static_assert(TXML_ARENA_VALUES == 0 && TXML_ARENA_TREE == 1, "two arenas, values first");
_Static_assert(TXML_DOC_OK == 0, "no document error is zero");
_Static_assert(TXML_LIMIT_NONE == 0, "no limit is zero");

/* ------------------------------------------------------------------------
 * Flag sets: every flag is a single bit, and no two flags in a set overlap.
 * Asserting the union pins both properties at once.
 */

#define SINGLE_BIT(flag) \
    _Static_assert(((flag) & ((flag) - 1u)) == 0u && (flag) != 0u, #flag " must be one bit")

/* ADR 0017 removed every retention flag; what is left is a promise about the
 * caller's buffer, not a choice about what the tree contains. */
SINGLE_BIT(TXML_INPUT_PADDED);
_Static_assert(TXML_INPUT_PADDED == 0x1u, "the one parser flag is the low bit");

SINGLE_BIT(TXML_NODE_EMPTY);
SINGLE_BIT(TXML_NODE_CDATA);
SINGLE_BIT(TXML_NODE_OPEN);
_Static_assert((TXML_NODE_EMPTY | TXML_NODE_CDATA | TXML_NODE_OPEN) == 0x7u,
               "the three node flags must be three distinct bits");

SINGLE_BIT(TXML_ATTR_NS_DECL);
SINGLE_BIT(TXML_ATTR_DEFAULTED);
_Static_assert((TXML_ATTR_NS_DECL | TXML_ATTR_DEFAULTED) == 0x3u,
               "the two attribute flags must be two distinct bits");

SINGLE_BIT(TXML_WRITE_DECLARATION);
SINGLE_BIT(TXML_WRITE_INDENT);
SINGLE_BIT(TXML_WRITE_EMPTY_TAGS);
_Static_assert((TXML_WRITE_DECLARATION | TXML_WRITE_INDENT | TXML_WRITE_EMPTY_TAGS) == 0x7u,
               "the three writer flags must be three distinct bits");

/* Node flags and attribute flags live in different fields, but a reader that
 * confuses them should not silently get a plausible answer: TXML_NODE_CDATA
 * and TXML_ATTR_DEFAULTED share a bit on purpose only if the header says so.
 * It does not, so simply record the two sets' widths. */
_Static_assert(TXML_NODE_OPEN > TXML_NODE_CDATA, "node flags are ordered");

/* ------------------------------------------------------------------------
 * Version.
 */

_Static_assert(TXML_VERSION_MAJOR >= 0, "a major version exists");
_Static_assert(TXML_VERSION_MINOR >= 0, "a minor version exists");
_Static_assert(TXML_VERSION_PATCH >= 0, "a patch version exists");

/* ------------------------------------------------------------------------
 * Configurations must be writable with designated initializers, and
 * TXML_LIMITS_DEFAULT must work as a member initializer inside one. This is
 * the ergonomics an embedder sees first; it is a compile-time check that the
 * macro is an expression of the right type.
 */

static void *unused_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)size;
    (void)alignment;
    return NULL;
}

static void unused_free(void *self, const txml_allocator_free_params *params) {
    (void)self;
    (void)params;
}

static const txml_allocator_vtable unused_vtable = { .alloc = unused_alloc, .free = unused_free };

int main(void) {
    const txml_allocator allocator = { .self = NULL, .v = &unused_vtable };

    /* TXML_STR: length excludes the terminator, and the pointer is the
     * literal itself. */
    const txml_str abc = TXML_STR("abc");
    CHECK(abc.length == 3u);
    CHECK(abc.ptr[0] == 'a');
    CHECK(abc.ptr[2] == 'c');

    const txml_str empty = TXML_STR("");
    CHECK(empty.length == 0u);

    const txml_str escaped = TXML_STR("a\"b");
    CHECK(escaped.length == 3u);

    /* TXML_LIMITS_DEFAULT as a plain initializer. */
    const txml_limits limits = TXML_LIMITS_DEFAULT;
    CHECK(limits.max_depth == 256u);
    CHECK(limits.max_attributes == 256u);
    CHECK(limits.max_name_length == 1024u);
    CHECK(limits.max_names == 4096u);
    CHECK(limits.max_document_bytes == 64u * 1024u * 1024u);
    CHECK(limits.entity_expansion_budget == 8u * 1024u * 1024u);

    /* Every limit must be usable: a zero default would mean "nothing is
     * allowed" for an embedder that copies the defaults. */
    CHECK(limits.max_depth > 0u);
    CHECK(limits.max_attributes > 0u);
    CHECK(limits.max_name_length > 0u);
    CHECK(limits.max_names > 0u);
    CHECK(limits.max_document_bytes > 0u);
    CHECK(limits.entity_expansion_budget > 0u);

    /* TXML_LIMITS_DEFAULT inside a configuration, next to designated
     * initializers and a flag set. */
    const txml_parser_config parser_config = {
        .allocator = allocator,
        .limits = TXML_LIMITS_DEFAULT,
        .flags = TXML_INPUT_PADDED,
        .staging_length = 0u,
        .chunk_size = 0u,
        .step_budget = 0u,
    };
    CHECK(parser_config.limits.max_depth == 256u);
    CHECK((parser_config.flags & TXML_INPUT_PADDED) != 0u);
    CHECK(parser_config.allocator.v == &unused_vtable);

    /* A zero-initialized configuration must mean "defaults everywhere the
     * header documents a zero default". */
    const txml_parser_config zeroed = { 0 };
    CHECK(zeroed.staging_length == 0u);
    CHECK(zeroed.chunk_size == 0u);
    CHECK(zeroed.step_budget == 0u);
    CHECK(zeroed.flags == 0u);

    unsigned char output_bytes[8];
    const txml_writer_config writer_config = {
        .allocator = allocator,
        .output = { .ptr = output_bytes, .capacity = (uint32_t)sizeof(output_bytes) },
        .limits = TXML_LIMITS_DEFAULT,
        .flags = TXML_WRITE_EMPTY_TAGS,
    };
    CHECK(writer_config.output.capacity == 8u);
    CHECK(writer_config.output.ptr == output_bytes);
    CHECK(writer_config.flags == TXML_WRITE_EMPTY_TAGS);

    /* Cursors are values: copying one must not alias anything. */
    txml_cursor cursor = { .doc = NULL, .node = 7u };
    const txml_cursor copy = cursor;
    cursor.node = 9u;
    CHECK(copy.node == 7u);
    CHECK(cursor.node == 9u);

    /* A find query over the whole document. */
    const txml_find query = { .name = TXML_NAME_NONE, .bound = TXML_INDEX_NONE };
    CHECK(query.name == UINT32_MAX);
    CHECK(query.bound == UINT32_MAX);

    /* A document error is plain data the caller can copy and keep. */
    const txml_document_error error = {
        .code = TXML_DOC_TAG_MISMATCH,
        .limit = TXML_LIMIT_NONE,
        .at = { .offset = 12u, .line = 2u, .column = 4u },
        .message = TXML_STR("end tag does not match"),
    };
    const txml_document_error error_copy = error;
    CHECK(error_copy.code == TXML_DOC_TAG_MISMATCH);
    CHECK(error_copy.at.line == 2u);
    CHECK(error_copy.message.length == 22u);

    if (failures != 0) {
        fprintf(stderr, "header_test: %d check(s) failed\n", failures);
        return 1;
    }

    printf("header_test: ok\n");
    return 0;
}

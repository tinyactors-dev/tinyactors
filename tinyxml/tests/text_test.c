/*
 * The text helpers: txml_str_equal, txml_qname_equal, txml_str_starts_with.
 *
 * They are three small functions, and they are the comparisons the rest of
 * the library is built out of — name interning, attribute lookup, and the
 * namespace checks all reduce to them. They are also the only public
 * functions an embedder can call without parsing anything.
 *
 * What is pinned here is that they are length-based, not NUL-based (ADR 0001:
 * no C library, so no strcmp semantics leak in), and byte-exact: no case
 * folding, no Unicode normalization, no trimming.
 *
 * Test programs may use the C library. The library itself may not.
 */
#include <stdio.h>
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

/* A view into the middle of a buffer: no terminator anywhere near it. */
static txml_str slice(const char *text, uint32_t offset, uint32_t length) {
    txml_str result = { text + offset, length };
    return result;
}

/* ------------------------------------------------------------------------
 * txml_str_equal.
 */

static void test_str_equal(void) {
    CHECK(txml_str_equal(TXML_STR("state"), TXML_STR("state")));
    CHECK(!txml_str_equal(TXML_STR("state"), TXML_STR("statement")));
    CHECK(!txml_str_equal(TXML_STR("statement"), TXML_STR("state")));
    CHECK(!txml_str_equal(TXML_STR("state"), TXML_STR("stata")));

    /* Byte-exact: no case folding. */
    CHECK(!txml_str_equal(TXML_STR("State"), TXML_STR("state")));
    CHECK(!txml_str_equal(TXML_STR("ID"), TXML_STR("id")));

    /* And no trimming. */
    CHECK(!txml_str_equal(TXML_STR(" id"), TXML_STR("id")));
    CHECK(!txml_str_equal(TXML_STR("id "), TXML_STR("id")));

    /* Empty is equal to empty, and to nothing else. */
    CHECK(txml_str_equal(TXML_STR(""), TXML_STR("")));
    CHECK(!txml_str_equal(TXML_STR(""), TXML_STR("x")));
    CHECK(!txml_str_equal(TXML_STR("x"), TXML_STR("")));

    /* An empty view with a null pointer is still empty: length decides, and
     * no byte is read. */
    const txml_str null_empty = { NULL, 0u };
    CHECK(txml_str_equal(null_empty, TXML_STR("")));
    CHECK(txml_str_equal(TXML_STR(""), null_empty));
    CHECK(!txml_str_equal(null_empty, TXML_STR("x")));

    /* Length decides, not a terminator: these views have no NUL after them. */
    const char haystack[] = "prefix:local=value";
    CHECK(txml_str_equal(slice(haystack, 0u, 6u), TXML_STR("prefix")));
    CHECK(txml_str_equal(slice(haystack, 7u, 5u), TXML_STR("local")));
    CHECK(!txml_str_equal(slice(haystack, 0u, 7u), TXML_STR("prefix")));

    /* A NUL inside the text is an ordinary byte, compared like any other.
     * TXML_STR() measures with sizeof, so it keeps the embedded one. */
    CHECK(TXML_STR("a\0b").length == 3u);
    CHECK(txml_str_equal(TXML_STR("a\0b"), TXML_STR("a\0b")));
    CHECK(!txml_str_equal(TXML_STR("a\0b"), TXML_STR("a\0c")));
    /* strcmp() would call these equal; this is the check that says it is not
     * strcmp(). */
    CHECK(!txml_str_equal(TXML_STR("a\0b"), TXML_STR("a")));

    /* Bytes above 0x7f are bytes: UTF-8 is compared, never decoded. */
    CHECK(txml_str_equal(TXML_STR("caf\xc3\xa9"), TXML_STR("caf\xc3\xa9")));
    CHECK(!txml_str_equal(TXML_STR("caf\xc3\xa9"), TXML_STR("cafe")));

    /* Reflexive, symmetric, and agreeing with memcmp over a corpus. */
    static const char *const corpus[] = { "",    "a",     "b",      "ab",
                                          "ba",  "abc",   "state",  "State",
                                          "id",  "xmlns", "urn:cat", "urn:cats" };
    const size_t count = sizeof(corpus) / sizeof(corpus[0]);

    for (size_t i = 0; i < count; i++) {
        const txml_str left = { corpus[i], (uint32_t)strlen(corpus[i]) };
        for (size_t j = 0; j < count; j++) {
            const txml_str right = { corpus[j], (uint32_t)strlen(corpus[j]) };
            const bool expected = strcmp(corpus[i], corpus[j]) == 0;
            CHECK(txml_str_equal(left, right) == expected);
            CHECK(txml_str_equal(left, right) == txml_str_equal(right, left));
        }
        CHECK(txml_str_equal(left, left));
    }
}

/* ------------------------------------------------------------------------
 * txml_str_starts_with.
 */

static void test_str_starts_with(void) {
    CHECK(txml_str_starts_with(TXML_STR("error.send.failed"), TXML_STR("error.")));
    CHECK(txml_str_starts_with(TXML_STR("error.send.failed"), TXML_STR("error.send")));
    CHECK(txml_str_starts_with(TXML_STR("error.send.failed"), TXML_STR("e")));
    CHECK(!txml_str_starts_with(TXML_STR("error.send.failed"), TXML_STR("send")));
    CHECK(!txml_str_starts_with(TXML_STR("error.send.failed"), TXML_STR("Error.")));

    /* The whole text is a prefix of itself. */
    CHECK(txml_str_starts_with(TXML_STR("done.state"), TXML_STR("done.state")));
    /* A longer prefix never matches, however much it agrees. */
    CHECK(!txml_str_starts_with(TXML_STR("done"), TXML_STR("done.state")));
    CHECK(!txml_str_starts_with(TXML_STR(""), TXML_STR("x")));

    /* The empty prefix matches everything, including the empty text. */
    CHECK(txml_str_starts_with(TXML_STR("anything"), TXML_STR("")));
    CHECK(txml_str_starts_with(TXML_STR(""), TXML_STR("")));

    const txml_str null_empty = { NULL, 0u };
    CHECK(txml_str_starts_with(TXML_STR("anything"), null_empty));
    CHECK(txml_str_starts_with(null_empty, null_empty));

    /* Length-based, as with equality: no terminator is consulted. */
    const char events[] = "error.communication";
    CHECK(txml_str_starts_with(slice(events, 0u, 19u), TXML_STR("error.")));
    CHECK(!txml_str_starts_with(slice(events, 0u, 3u), TXML_STR("error.")));

    /* Agrees with equality when the prefix is the whole text, and with
     * memcmp over a corpus otherwise. */
    static const char *const corpus[] = { "", "e", "er", "error", "error.", "error.send", "Error." };
    const size_t count = sizeof(corpus) / sizeof(corpus[0]);

    for (size_t i = 0; i < count; i++) {
        const txml_str text = { corpus[i], (uint32_t)strlen(corpus[i]) };
        for (size_t j = 0; j < count; j++) {
            const txml_str prefix = { corpus[j], (uint32_t)strlen(corpus[j]) };
            const bool expected = prefix.length <= text.length &&
                                  (prefix.length == 0u ||
                                   memcmp(text.ptr, prefix.ptr, prefix.length) == 0);
            CHECK(txml_str_starts_with(text, prefix) == expected);
        }
        /* Equality implies the prefix relation, in both directions. */
        CHECK(txml_str_starts_with(text, text));
    }
}

/* ------------------------------------------------------------------------
 * txml_qname_equal: both halves, and the namespace is not optional.
 */

static void test_qname_equal(void) {
    const txml_qname item = { TXML_STR("urn:cat"), TXML_STR("item") };
    const txml_qname same = { TXML_STR("urn:cat"), TXML_STR("item") };
    const txml_qname other_ns = { TXML_STR("urn:meta"), TXML_STR("item") };
    const txml_qname other_local = { TXML_STR("urn:cat"), TXML_STR("name") };
    const txml_qname unqualified = { TXML_STR(""), TXML_STR("item") };

    CHECK(txml_qname_equal(&item, &same));
    CHECK(txml_qname_equal(&item, &item));
    CHECK(txml_qname_equal(&same, &item));

    CHECK(!txml_qname_equal(&item, &other_ns));
    CHECK(!txml_qname_equal(&item, &other_local));

    /* No namespace is a namespace of its own: an unqualified name never
     * equals a qualified one. This is what makes an unprefixed attribute
     * distinct from the same local name in the default namespace. */
    CHECK(!txml_qname_equal(&item, &unqualified));
    CHECK(!txml_qname_equal(&unqualified, &item));

    const txml_qname also_unqualified = { TXML_STR(""), TXML_STR("item") };
    CHECK(txml_qname_equal(&unqualified, &also_unqualified));

    /* Both halves are compared byte-exactly, like txml_str_equal. */
    const txml_qname cased = { TXML_STR("urn:cat"), TXML_STR("Item") };
    const txml_qname cased_ns = { TXML_STR("URN:CAT"), TXML_STR("item") };
    CHECK(!txml_qname_equal(&item, &cased));
    CHECK(!txml_qname_equal(&item, &cased_ns));

    /* A namespace URI is compared, not resolved: no trailing-slash or case
     * equivalence, no relative-reference handling. */
    const txml_qname trailing = { TXML_STR("urn:cat/"), TXML_STR("item") };
    CHECK(!txml_qname_equal(&item, &trailing));

    /* Empty everywhere is still a well-formed comparison. */
    const txml_qname nothing = { TXML_STR(""), TXML_STR("") };
    const txml_qname nothing_too = { { NULL, 0u }, { NULL, 0u } };
    CHECK(txml_qname_equal(&nothing, &nothing_too));
    CHECK(!txml_qname_equal(&nothing, &unqualified));

    /* qname equality is exactly the two string equalities. */
    static const char *const namespaces[] = { "", "urn:cat", "urn:meta" };
    static const char *const locals[] = { "", "item", "name" };

    for (size_t a = 0; a < 3u; a++) {
        for (size_t b = 0; b < 3u; b++) {
            const txml_qname left = { { namespaces[a], (uint32_t)strlen(namespaces[a]) },
                                      { locals[b], (uint32_t)strlen(locals[b]) } };
            for (size_t c = 0; c < 3u; c++) {
                for (size_t d = 0; d < 3u; d++) {
                    const txml_qname right = { { namespaces[c], (uint32_t)strlen(namespaces[c]) },
                                               { locals[d], (uint32_t)strlen(locals[d]) } };
                    const bool expected = txml_str_equal(left.ns, right.ns) &&
                                          txml_str_equal(left.local, right.local);
                    CHECK(txml_qname_equal(&left, &right) == expected);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ */

int main(void) {
    test_str_equal();
    test_str_starts_with();
    test_qname_equal();

    if (failures != 0) {
        fprintf(stderr, "text_test: %d check(s) failed\n", failures);
        return 1;
    }

    printf("text_test: ok\n");
    return 0;
}

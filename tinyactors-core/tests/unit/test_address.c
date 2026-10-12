/* ta_address: the one address parser (docs/protocol.md, Addresses): the
 * address vectors (valid, invalid, formatted), then the edges. */
#include "../harness/harness.h"
#include <tinyactors/address.h>

#include <stdlib.h>
#include <string.h>

static ta_string_view S(const char *s) {
    return (ta_string_view){s, (uint32_t)strlen(s)};
}

static int string_equal(ta_string_view a, const char *b) {
    return a.length == strlen(b) && (!a.length || memcmp(a.pointer, b, a.length) == 0);
}

/* Addresses that parse, into their system and their session or name. */
typedef struct valid_address {
    const char *text, *system;
    uint64_t session; /* 0: a name */
    const char *name;
} valid_address;

static const valid_address valid_addresses[] = {
    {"tinyactors://a/#_scxml_1", "a", 1, NULL},
    {"tinyactors://orders/#_scxml_42", "orders", 42, NULL},
    {"tinyactors://eu.orders-2/#_scxml_9007199254740993", "eu.orders-2", 9007199254740993u, NULL},
    {"tinyactors://db/#_scxml_4611686018427387904", "db", 4611686018427387904u, NULL},
    {"tinyactors://db/#_scxml_18446744073709551615", "db", UINT64_MAX, NULL},
    {"tinyactors://chess/@lobby", "chess", 0, "lobby"},
    {"tinyactors://chess/@order-1", "chess", 0, "order-1"},
    {"tinyactors://x/@a.b_c~d", "x", 0, "a.b_c~d"},
    {"tinyactors://x/@link%3Aserver", "x", 0, "link:server"},
    {"tinyactors://x/@caf%C3%A9", "x", 0, "caf\xc3\xa9"},
    {"tinyactors://x/@a%2Fb", "x", 0, "a/b"},
    {"tinyactors://0/#_scxml_7", "0", 7, NULL},
};

/* Addresses that do not parse, and why. */
static const char *const invalid_addresses[][2] = {
    {"tinyactors://a/#_scxml_0", "session 0"},
    {"tinyactors://a/#_scxml_01", "leading zero"},
    {"tinyactors://a/#_scxml_18446744073709551616", "above 2^64-1"},
    {"tinyactors://a/#_scxml_", "no digits"},
    {"tinyactors://a/#_scxml_12x", "not a number"},
    {"tinyactors://a/#_scxml_-1", "negative"},
    {"tinyactors://A/#_scxml_1", "uppercase system id"},
    {"tinyactors://-a/#_scxml_1", "label starts with -"},
    {"tinyactors://a-/#_scxml_1", "label ends with -"},
    {"tinyactors://a..b/#_scxml_1", "empty label"},
    {"tinyactors://a_b/#_scxml_1", "underscore in system id"},
    {"tinyactors:///#_scxml_1", "empty system id"},
    {"tinyactors://a/", "empty local part"},
    {"tinyactors://a", "no local part"},
    {"tinyactors://a/@", "empty name"},
    {"tinyactors://a/@a b", "space in name"},
    {"tinyactors://a/@a%2", "truncated escape"},
    {"tinyactors://a/@a%zz", "bad escape"},
    {"tinyactors://a/#_parent", "SCXML-local target, not an address"},
    {"scxml://a/#_scxml_1", "wrong scheme"},
    {"#_scxml_1", "local part only"},
};

/* Addresses written from a system and a session or a name. */
typedef struct formatted_address {
    const char *system;
    uint64_t session; /* 0: the name */
    const char *name, *text;
} formatted_address;

static const formatted_address formatted_addresses[] = {
    {"a", 1, NULL, "tinyactors://a/#_scxml_1"},
    {"db", UINT64_MAX, NULL, "tinyactors://db/#_scxml_18446744073709551615"},
    {"x", 0, "link:server", "tinyactors://x/@link%3Aserver"},
    {"x", 0, "caf\xc3\xa9", "tinyactors://x/@caf%C3%A9"},
    {"x", 0, "a b/c", "tinyactors://x/@a%20b%2Fc"},
};

static void valid(const valid_address *vector) {
    ta_address address;
    ta_status status = ta_address_parse(S(vector->text), &address);
    CHECK_EQUAL(status, TA_OK);
    if (status != TA_OK) {
        fprintf(stderr, "  should parse: %s\n", vector->text);
        return;
    }
    CHECK(string_equal(address.system, vector->system));
    if (vector->session) {
        CHECK_EQUAL(address.kind, TA_ADDRESS_SESSION);
        CHECK_EQUAL(address.session, vector->session);
        char back[TA_ADDRESS_SESSION_MAX]; /* formats back to the same text */
        ta_buffer out = {back, sizeof back, 0};
        CHECK_EQUAL(ta_address_format(address.system, address.session, &out), TA_OK);
        size_t n = out.length;
        CHECK(n == strlen(vector->text) && !memcmp(back, vector->text, n));
    } else {
        CHECK_EQUAL(address.kind, TA_ADDRESS_NAME);
        char decoded[256];
        ta_buffer out = {decoded, sizeof decoded, 0};
        CHECK_EQUAL(ta_address_name_decode(address.name, &out), TA_OK);
        size_t n = out.length;
        CHECK(n == strlen(vector->name) && !memcmp(decoded, vector->name, n));
        if (!(n == strlen(vector->name) && !memcmp(decoded, vector->name, n)))
            fprintf(stderr, "  name of %s: %.*s, want %s\n", vector->text, (int)n, decoded, vector->name);
    }
    /* the local part alone parses the same */
    ta_address local;
    CHECK_EQUAL(ta_address_parse_local(address.local, &local), TA_OK);
    CHECK_EQUAL(local.kind, address.kind);
}

static void format(const formatted_address *vector) {
    char out[512];
    ta_buffer written = {out, sizeof out, 0};
    if (vector->session)
        CHECK_EQUAL(ta_address_format(S(vector->system), vector->session, &written), TA_OK);
    else
        CHECK_EQUAL(ta_address_format_name(S(vector->system), S(vector->name), &written), TA_OK);
    size_t n = written.length;
    CHECK(n == strlen(vector->text) && !memcmp(out, vector->text, n));
    if (!(n == strlen(vector->text) && !memcmp(out, vector->text, n)))
        fprintf(stderr, "  format: %.*s, want %s\n", (int)n, out, vector->text);
}

static void test_vectors(void) {
    for (size_t i = 0; i < sizeof valid_addresses / sizeof valid_addresses[0]; i++)
        valid(&valid_addresses[i]);
    for (size_t i = 0; i < sizeof invalid_addresses / sizeof invalid_addresses[0]; i++) {
        ta_address address;
        ta_status status = ta_address_parse(S(invalid_addresses[i][0]), &address);
        CHECK_EQUAL(status, TA_ERROR_INVALID);
        if (status != TA_ERROR_INVALID)
            fprintf(stderr, "  should not parse (%s): %s\n", invalid_addresses[i][1], invalid_addresses[i][0]);
    }
    for (size_t i = 0; i < sizeof formatted_addresses / sizeof formatted_addresses[0]; i++)
        format(&formatted_addresses[i]);
}

static void test_edges(void) {
    ta_address address;
    char buffer[TA_ADDRESS_SESSION_MAX];
    /* the longest system id and session id fit TA_ADDRESS_SESSION_MAX */
    char system[TA_ADDRESS_SYSTEM_MAX + 1];
    for (uint32_t i = 0; i < TA_ADDRESS_SYSTEM_MAX; i++)
        system[i] = (i % 64 == 63) ? '.' : 'a';
    system[TA_ADDRESS_SYSTEM_MAX] = 0;
    CHECK(ta_address_system_valid(S(system)));
    ta_buffer out = {buffer, sizeof buffer, 0};
    CHECK_EQUAL(ta_address_format(S(system), UINT64_MAX, &out), TA_OK);
    CHECK_EQUAL(out.length, sizeof buffer);
    CHECK_EQUAL(ta_address_parse((ta_string_view){buffer, (uint32_t)out.length}, &address), TA_OK);
    CHECK_EQUAL(address.session, UINT64_MAX);
    /* too long a system id; too small a buffer; session 0 */
    char longer[TA_ADDRESS_SYSTEM_MAX + 2];
    memset(longer, 'a', sizeof longer - 1);
    longer[sizeof longer - 1] = 0;
    CHECK(!ta_address_system_valid(S(longer)));
    ta_buffer small = {buffer, 10, 0};
    CHECK_EQUAL(ta_address_format(S("a"), 1, &small), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(small.length, 24u); /* what it needs: tinyactors://a/#_scxml_1 */
    CHECK_EQUAL(ta_address_format(S("a"), 0, &out), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_address_format_name(S("a"), S(""), &out), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_address_parse((ta_string_view){NULL, 0}, &address), TA_ERROR_INVALID);
    /* parse_local */
    CHECK_EQUAL(ta_address_parse_local(S("#_scxml_5"), &address), TA_OK);
    CHECK_EQUAL(address.session, 5u);
    CHECK_EQUAL(address.system.length, 0u);
    CHECK_EQUAL(ta_address_parse_local(S("#_parent"), &address), TA_ERROR_INVALID);
}

int main(void) {
    RUN(test_vectors);
    RUN(test_edges);
    TEST_MAIN_END();
}

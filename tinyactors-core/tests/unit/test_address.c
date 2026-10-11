/* ta_address: the one address parser (docs/protocol.md, Addresses), against
 * the shared vectors tests/conformance/addresses.json. The vectors are read
 * with quickjs (JSON only, no evaluation of code). */
#include "../harness/harness.h"
#include <tinyactors/address.h>
#include "quickjs.h"

#include <stdlib.h>
#include <string.h>

#ifndef TA_VECTORS
#define TA_VECTORS "tests/conformance/addresses.json"
#endif

static JSContext *J;

static char *slurp(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) b[n] = 0;
    *length = (size_t)n;
    return b;
}

static const char *str_of(JSValueConst o, const char *k, char *buffer, size_t capacity) {
    JSValue v = JS_GetPropertyStr(J, o, k);
    buffer[0] = 0;
    if (JS_IsString(v)) {
        size_t n;
        const char *s = JS_ToCStringLen(J, &n, v);
        if (n < capacity) {
            memcpy(buffer, s, n);
            buffer[n] = 0;
        }
        JS_FreeCString(J, s);
    }
    int has = !JS_IsUndefined(v);
    JS_FreeValue(J, v);
    return has ? buffer : NULL;
}

static ta_string_view S(const char *s) {
    return (ta_string_view){s, (uint32_t)strlen(s)};
}

static int string_equal(ta_string_view a, const char *b) {
    return a.length == strlen(b) && (!a.length || memcmp(a.pointer, b, a.length) == 0);
}

static void each(JSValueConst root, const char *list, void (*function)(JSValueConst)) {
    JSValue arr = JS_GetPropertyStr(J, root, list);
    int64_t n = 0;
    JS_GetLength(J, arr, &n);
    CHECK(n > 0);
    for (int64_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(J, arr, (uint32_t)i);
        function(e);
        JS_FreeValue(J, e);
    }
    JS_FreeValue(J, arr);
}

static void valid(JSValueConst e) {
    char text[512], system_text[300], kind[16], session[32], name[256];
    str_of(e, "text", text, sizeof text);
    str_of(e, "system", system_text, sizeof system_text);
    str_of(e, "kind", kind, sizeof kind);
    ta_address address;
    ta_status status = ta_address_parse(S(text), &address);
    CHECK_EQUAL(status, TA_OK);
    if (status != TA_OK) {
        fprintf(stderr, "  should parse: %s\n", text);
        return;
    }
    CHECK(string_equal(address.system, system_text));
    if (!strcmp(kind, "session")) {
        str_of(e, "session", session, sizeof session);
        CHECK_EQUAL(address.kind, TA_ADDRESS_SESSION);
        CHECK_EQUAL(address.session, strtoull(session, NULL, 10));
        char back[TA_ADDRESS_SESSION_MAX]; /* formats back to the same text */
        ta_buffer out = {back, sizeof back, 0};
        CHECK_EQUAL(ta_address_format(address.system, address.session, &out), TA_OK);
        size_t n = out.length;
        CHECK(n == strlen(text) && !memcmp(back, text, n));
    } else {
        str_of(e, "name", name, sizeof name);
        CHECK_EQUAL(address.kind, TA_ADDRESS_NAME);
        char dec[256];
        ta_buffer out = {dec, sizeof dec, 0};
        CHECK_EQUAL(ta_address_name_decode(address.name, &out), TA_OK);
        size_t n = out.length;
        CHECK(n == strlen(name) && !memcmp(dec, name, n));
        if (!(n == strlen(name) && !memcmp(dec, name, n)))
            fprintf(stderr, "  name of %s: %.*s, want %s\n", text, (int)n, dec, name);
    }
    /* the local part alone parses the same */
    ta_address l;
    CHECK_EQUAL(ta_address_parse_local(address.local, &l), TA_OK);
    CHECK_EQUAL(l.kind, address.kind);
}

static void invalid(JSValueConst e) {
    char text[512], why[128];
    str_of(e, "text", text, sizeof text);
    str_of(e, "why", why, sizeof why);
    ta_address address;
    ta_status status = ta_address_parse(S(text), &address);
    CHECK_EQUAL(status, TA_ERROR_INVALID);
    if (status != TA_ERROR_INVALID) fprintf(stderr, "  should not parse (%s): %s\n", why, text);
}

static void format(JSValueConst e) {
    char system[300], session[32], name[256], want[512];
    str_of(e, "system", system, sizeof system);
    str_of(e, "text", want, sizeof want);
    char out[512];
    ta_buffer written = {out, sizeof out, 0};
    if (str_of(e, "session", session, sizeof session))
        CHECK_EQUAL(ta_address_format(S(system), strtoull(session, NULL, 10), &written), TA_OK);
    else
        CHECK_EQUAL(
            ta_address_format_name(S(system), S(str_of(e, "name", name, sizeof name)), &written),
            TA_OK);
    size_t n = written.length;
    CHECK(n == strlen(want) && !memcmp(out, want, n));
    if (!(n == strlen(want) && !memcmp(out, want, n)))
        fprintf(stderr, "  format: %.*s, want %s\n", (int)n, out, want);
}

static void test_vectors(void) {
    size_t length;
    char *text = slurp(TA_VECTORS, &length);
    CHECK(text != NULL);
    if (!text) return;
    JSRuntime *rt = JS_NewRuntime();
    J = JS_NewContext(rt);
    JSValue root = JS_ParseJSON(J, text, length, "addresses.json");
    CHECK(JS_IsObject(root));
    each(root, "valid", valid);
    each(root, "invalid", invalid);
    each(root, "format", format);
    JS_FreeValue(J, root);
    JS_FreeContext(J);
    JS_FreeRuntime(rt);
    free(text);
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

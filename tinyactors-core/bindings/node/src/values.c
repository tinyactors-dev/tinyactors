/* values.c — neutral values (docs/protocol.md, "Values"): wire JSON and base64 (binding.h). */
#include "binding.h"

/* ---- base64 (RFC 4648, standard alphabet, padded): $bytes on the wire --------------- */

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_put(text *t, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) |
                     (i + 2 < n ? p[i + 2] : 0);
        char out[4] = {base64_alphabet[v >> 18 & 63], base64_alphabet[v >> 12 & 63],
                       i + 1 < n ? base64_alphabet[v >> 6 & 63] : '=',
                       i + 2 < n ? base64_alphabet[v & 63] : '='};
        text_put(t, out, 4);
    }
}

static int base64_digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes into out (n / 4 * 3 bytes at most); returns the length, or -1. */
static long base64_read(const char *s, size_t n, unsigned char *out) {
    if (n % 4) return -1;
    long k = 0;
    for (size_t i = 0; i < n; i += 4) {
        int d[4];
        for (int j = 0; j < 4; j++) {
            d[j] = s[i + j] == '=' && i + 4 == n && j >= 2 ? 0 : base64_digit(s[i + j]);
            if (d[j] < 0) return -1;
        }
        uint32_t v =
            (uint32_t)d[0] << 18 | (uint32_t)d[1] << 12 | (uint32_t)d[2] << 6 | (uint32_t)d[3];
        out[k++] = (unsigned char)(v >> 16);
        if (s[i + 2] != '=') out[k++] = (unsigned char)(v >> 8);
        if (s[i + 3] != '=') out[k++] = (unsigned char)v;
    }
    return k;
}

static void wire_number(text *t, double d) {
    char buffer[40];
    if (d != d || d > 1.7976931348623157e308 || d < -1.7976931348623157e308) {
        text_put(t, "null", 4);
        return;
    }
    if (d == 0) {
        text_char(t, '0');
        return;
    }
    for (int precision = 15; precision <= 17; precision++) { /* the shortest that reads back */
        snprintf(buffer, sizeof buffer, "%.*g", precision, d);
        if (strtod(buffer, NULL) == d) break;
    }
    text_put(t, buffer, strlen(buffer));
}

/* 1: a property worth writing (not undefined, a function or a symbol) */
static int wire_present(napi_env environment, napi_value v) {
    napi_valuetype k = type_of(environment, v);
    return k != napi_undefined && k != napi_function && k != napi_symbol;
}

static ta_status wire_object(napi_env environment, napi_value v, text *t, int depth) {
    napi_value keys;
    uint32_t n = 0;
    if (napi_get_property_names(environment, v, &keys) != napi_ok) return TA_ERROR_INVALID;
    napi_get_array_length(environment, keys, &n);
    int escape = 0;
    for (uint32_t i = 0; i < n && !escape; i++) {
        napi_value key;
        char first[2] = {0, 0};
        size_t length = 0;
        napi_get_element(environment, keys, i, &key);
        if (napi_get_value_string_utf8(environment, key, first, sizeof first, &length) == napi_ok &&
            first[0] == '$')
            escape = 1;
    }
    if (escape) text_put(t, "{\"$\":", 5);
    text_char(t, '{');
    int written = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, value;
        napi_get_element(environment, keys, i, &key);
        if (napi_get_property(environment, v, key, &value) != napi_ok) return TA_ERROR_INVALID;
        if (!wire_present(environment, value)) continue;
        size_t length = 0;
        char *k = get_string(environment, key, &length);
        if (!k) return TA_ERROR_INVALID;
        if (written++) text_char(t, ',');
        text_string(t, k, length);
        free(k);
        text_char(t, ':');
        ta_status status = wire_value(environment, value, t, depth + 1);
        if (status != TA_OK) return status;
    }
    text_char(t, '}');
    if (escape) text_char(t, '}');
    return TA_OK;
}

ta_status wire_value(napi_env environment, napi_value v, text *t, int depth) {
    if (depth > 128) return TA_ERROR_INVALID;
    switch (type_of(environment, v)) {
    case napi_null:
    case napi_undefined:
    case napi_function:
    case napi_symbol:
        text_put(t, "null", 4);
        return TA_OK;
    case napi_boolean: {
        bool b = false;
        napi_get_value_bool(environment, v, &b);
        text_put(t, b ? "true" : "false", b ? 4 : 5);
        return TA_OK;
    }
    case napi_number:
        wire_number(t, get_double(environment, v));
        return TA_OK;
    case napi_string:
    case napi_bigint: {
        napi_value s = v;
        size_t n = 0;
        if (type_of(environment, v) == napi_bigint &&
            napi_coerce_to_string(environment, v, &s) != napi_ok)
            return TA_ERROR_INVALID;
        char *p = get_string(environment, s, &n);
        if (!p) return TA_ERROR_INVALID;
        text_string(t, p, n);
        free(p);
        return TA_OK;
    }
    case napi_object:
        break;
    default:
        return TA_ERROR_INVALID;
    }
    void *data = NULL;
    size_t n = 0;
    if (bytes_of(environment, v, &data, &n)) {
        text_put(t, "{\"$bytes\":\"", 11);
        base64_put(t, (const unsigned char *)data, n);
        text_put(t, "\"}", 2);
        return TA_OK;
    }
    napi_value to_json = property(environment, v, "toJSON");
    if (type_of(environment, to_json) == napi_function) {
        napi_value r = NULL;
        if (napi_call_function(environment, v, to_json, 0, NULL, &r) != napi_ok) {
            clear_exception(environment);
            return TA_ERROR_INVALID;
        }
        napi_value address = property(environment, r, "$ref");
        if (type_of(environment, address) == napi_string) {
            size_t length = 0;
            char *p = get_string(environment, address, &length);
            text_put(t, "{\"$ref\":", 8);
            text_string(t, p, length);
            text_char(t, '}');
            free(p);
            return TA_OK;
        }
        return wire_value(environment, r, t, depth + 1);
    }
    bool is_array = false;
    napi_is_array(environment, v, &is_array);
    if (!is_array) return wire_object(environment, v, t, depth);
    uint32_t count = 0;
    napi_get_array_length(environment, v, &count);
    text_char(t, '[');
    for (uint32_t i = 0; i < count; i++) {
        napi_value element;
        if (i) text_char(t, ',');
        napi_get_element(environment, v, i, &element);
        ta_status status = wire_value(environment, element, t, depth + 1);
        if (status != TA_OK) return status;
    }
    text_char(t, ']');
    return TA_OK;
}

/* A JS value as neutral JSON text in *t (t->failed: no memory). */
ta_status wire_of(napi_env environment, napi_value v, text *t) {
    t->length = 0;
    ta_status status = wire_value(environment, v, t, 0);
    if (status == TA_OK && t->failed) status = TA_ERROR_NO_MEMORY;
    return status;
}

/* JSON.parse's result with the neutral forms read: {"$bytes"}, {"$ref"}
 * (refs: callbacks.ref, or the address itself), {"$": ...} (its object's keys
 * literally, its values read). */
static napi_value unwire_children(napi_env environment, napi_value v, napi_value refs, int depth);

static napi_value unwire(napi_env environment, napi_value v, napi_value refs, int depth) {
    if (depth > 512 || type_of(environment, v) != napi_object) return v;
    bool is_array = false;
    napi_is_array(environment, v, &is_array);
    if (is_array) return unwire_children(environment, v, refs, depth);
    napi_value keys, key, inner;
    uint32_t n = 0;
    napi_get_property_names(environment, v, &keys);
    napi_get_array_length(environment, keys, &n);
    if (n == 1) {
        char name[8] = {0};
        size_t length = 0;
        napi_get_element(environment, keys, 0, &key);
        napi_get_value_string_utf8(environment, key, name, sizeof name, &length);
        napi_get_property(environment, v, key, &inner);
        if (length == 6 && !strcmp(name, "$bytes") && type_of(environment, inner) == napi_string) {
            size_t sl = 0;
            char *s = get_string(environment, inner, &sl);
            unsigned char *out = (unsigned char *)malloc(sl / 4 * 3 + 1);
            long k = s && out ? base64_read(s, sl, out) : -1;
            napi_value r = k >= 0 ? bytes_value(environment, out, (size_t)k) : v;
            free(s);
            free(out);
            return r;
        }
        if (length == 4 && !strcmp(name, "$ref") && type_of(environment, inner) == napi_string) {
            napi_value r = type_of(environment, refs) == napi_function
                               ? call(environment, refs, 1, &inner)
                               : NULL;
            if (!r) clear_exception(environment);
            return r ? r : inner;
        }
        if (length == 1 && name[0] == '$' && type_of(environment, inner) == napi_object)
            return unwire_children(environment, inner, refs, depth);
    }
    return unwire_children(environment, v, refs, depth);
}

static napi_value unwire_children(napi_env environment, napi_value v, napi_value refs, int depth) {
    bool is_array = false;
    napi_is_array(environment, v, &is_array);
    if (is_array) {
        uint32_t n = 0;
        napi_get_array_length(environment, v, &n);
        for (uint32_t i = 0; i < n; i++) {
            napi_value element;
            napi_get_element(environment, v, i, &element);
            if (type_of(environment, element) == napi_object)
                napi_set_element(environment, v, i, unwire(environment, element, refs, depth + 1));
        }
        return v;
    }
    napi_value keys;
    uint32_t n = 0;
    napi_get_property_names(environment, v, &keys);
    napi_get_array_length(environment, keys, &n);
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, element;
        napi_get_element(environment, keys, i, &key);
        napi_get_property(environment, v, key, &element);
        if (type_of(environment, element) == napi_object)
            napi_set_property(environment, v, key, unwire(environment, element, refs, depth + 1));
    }
    return v;
}

/* Neutral JSON text -> a JS value; NULL if it does not parse (no exception left). */
napi_value value_of_wire(napi_env environment, ta_string_view json, napi_value refs) {
    napi_value s = str_value(environment, json),
               parsed = call_global(environment, "JSON", "parse", 1, &s);
    if (!parsed) {
        clear_exception(environment);
        return NULL;
    }
    return unwire(environment, parsed, refs, 0);
}

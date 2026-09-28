#include "napi_codecs.h"

#include <string.h>

#include "gen/ta_napi.h"

/* ChartErrorCode, by ta_chart_error_code. */
static const char *const tna_chart_error_names[] = {
    NULL,
    "syntax",
    "not-chart",
    "unknown-construct",
    "misplaced-construct",
    "unknown-attribute",
    "missing-attribute",
    "conflicting-attribute",
    "bad-value",
    "foreign-content",
    "src-unresolved",
    "limit",
    "unrepresentable",
};

#define TNA_CHART_ERROR_COUNT (sizeof tna_chart_error_names / sizeof tna_chart_error_names[0])

const char *tna_chart_error_name(ta_chart_error_code code) {
    return (uint32_t)code < TNA_CHART_ERROR_COUNT ? tna_chart_error_names[code] : NULL;
}

/* The code named `text`; SYNTAX for a name that is none (lib/ checks names). */
static ta_chart_error_code tna_chart_error_parse(ta_str text) {
    for (uint32_t code = 1; code < TNA_CHART_ERROR_COUNT; code += 1u) {
        const char *name = tna_chart_error_names[code];
        if (strlen(name) == text.len && memcmp(name, text.ptr, text.len) == 0) {
            return (ta_chart_error_code)code;
        }
    }
    return TA_CHART_ERROR_SYNTAX;
}

static tna_js_codec *tna_js_codec_of(void *self) {
    return self;
}

/* A property of `object`, or NULL when it is absent or undefined. */
static napi_value tna_js_property(napi_env env, napi_value object, const char *name) {
    napi_value value = NULL;
    napi_valuetype type = napi_undefined;
    if (napi_get_named_property(env, object, name, &value) != napi_ok || napi_typeof(env, value, &type) != napi_ok ||
        type == napi_undefined || type == napi_null) {
        return NULL;
    }
    return value;
}

/* A string property copied into `arena`; empty when absent. */
static ta_str tna_js_text(napi_env env, napi_value object, const char *name, ta_arena *arena) {
    napi_value value = tna_js_property(env, object, name);
    size_t length = 0;
    if (value == NULL || napi_get_value_string_utf8(env, value, NULL, 0, &length) != napi_ok || length > UINT32_MAX) {
        return (ta_str){ 0 };
    }
    char *text = ta_arena_alloc(arena, length + 1u, 1);
    if (text == NULL || napi_get_value_string_utf8(env, value, text, length + 1u, &length) != napi_ok) {
        return (ta_str){ 0 };
    }
    return (ta_str){ text, (uint32_t)length };
}

/* A non-negative number property; 0 when absent. */
static uint64_t tna_js_count(napi_env env, napi_value object, const char *name) {
    napi_value value = tna_js_property(env, object, name);
    double number = 0;
    if (value == NULL || napi_get_value_double(env, value, &number) != napi_ok || !(number >= 0) || number > 9007199254740991.0) {
        return 0;
    }
    return (uint64_t)number;
}

/* A refusal { code, message, line?, column?, offset?, construct?, attribute? }. */
static void tna_js_refusal(napi_env env, napi_value refused, ta_arena *arena, ta_chart_error *error) {
    if (error == NULL) {
        return;
    }
    *error = (ta_chart_error){
        .code = tna_chart_error_parse(tna_js_text(env, refused, "code", arena)),
        .at = { .offset = tna_js_count(env, refused, "offset"),
                .line = (uint32_t)tna_js_count(env, refused, "line"),
                .column = (uint32_t)tna_js_count(env, refused, "column") },
        .construct = tna_js_text(env, refused, "construct", arena),
        .attribute = tna_js_text(env, refused, "attribute", arena),
        .message = tna_js_text(env, refused, "message", arena),
    };
}

/* What core is told when the codec threw: the exception stays pending. */
static ta_result tna_js_threw(ta_chart_error *error) {
    if (error != NULL) {
        *error = (ta_chart_error){ .code = TA_CHART_ERROR_SYNTAX, .message = { "the codec threw", 15 } };
    }
    return TA_ERR_INVALID;
}

static ta_result tna_js_decode(void *self, const ta_chart_decode_params *params) {
    tna_js_codec *codec = tna_js_codec_of(self);
    napi_env env = codec->call->env;
    napi_value buffer = NULL;
    napi_value bytes = NULL;
    napi_value undefined = NULL;
    napi_value answer = NULL;
    void *data = NULL;
    if (napi_create_arraybuffer(env, params->input.len, &data, &buffer) != napi_ok ||
        napi_create_typedarray(env, napi_uint8_array, params->input.len, buffer, 0, &bytes) != napi_ok) {
        return TA_ERR_NOMEM;
    }
    if (params->input.len != 0) {
        memcpy(data, params->input.ptr, params->input.len);
    }
    napi_get_undefined(env, &undefined);
    if (napi_call_function(env, undefined, codec->decode, 1, &bytes, &answer) != napi_ok) {
        return tna_js_threw(params->error);
    }
    napi_value refused = tna_js_property(env, answer, "refused");
    if (refused != NULL) {
        tna_js_refusal(env, refused, params->arena, params->error);
        return TA_ERR_INVALID;
    }
    napi_value spec = tna_js_property(env, answer, "spec");
    ta_scxml_spec *out = tna_call_alloc(codec->call, sizeof *out, _Alignof(ta_scxml_spec));
    if (spec == NULL || out == NULL || !tna_gen_decode_spec(codec->call, spec, out)) {
        /* The generated codec threw, with the path of what it refused. */
        return tna_js_threw(params->error);
    }
    *params->out = out;
    return TA_OK;
}

/* Tells the span sink where the codec wrote each spec element. */
static void tna_js_spans(napi_env env, napi_value spans, ta_chart_span_sink sink) {
    uint32_t count = 0;
    bool array = false;
    if (sink.v == NULL || spans == NULL || napi_is_array(env, spans, &array) != napi_ok || !array ||
        napi_get_array_length(env, spans, &count) != napi_ok) {
        return;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        napi_value entry = NULL;
        napi_valuetype type = napi_undefined;
        if (napi_get_element(env, spans, index, &entry) != napi_ok || napi_typeof(env, entry, &type) != napi_ok ||
            type != napi_object) {
            continue;
        }
        ta_source_span where = {
            .offset = tna_js_count(env, entry, "offset"),
            .length = (uint32_t)tna_js_count(env, entry, "length"),
            .line = (uint32_t)tna_js_count(env, entry, "line"),
            .column = (uint32_t)tna_js_count(env, entry, "column"),
            .end_line = (uint32_t)tna_js_count(env, entry, "end_line"),
            .end_column = (uint32_t)tna_js_count(env, entry, "end_column"),
        };
        if (where.line != 0) {
            sink.v->written(sink.self, index + 1u, &where);
        }
    }
}

static ta_result tna_js_encode(void *self, const ta_chart_encode_params *params) {
    tna_js_codec *codec = tna_js_codec_of(self);
    napi_env env = codec->call->env;
    napi_value spec = tna_gen_encode_spec(codec->call, params->spec);
    if (spec == NULL) {
        return tna_js_threw(params->error);
    }
    napi_value undefined = NULL;
    napi_value answer = NULL;
    napi_get_undefined(env, &undefined);
    if (napi_call_function(env, undefined, codec->encode, 1, &spec, &answer) != napi_ok) {
        return tna_js_threw(params->error);
    }
    napi_value refused = tna_js_property(env, answer, "refused");
    if (refused != NULL) {
        tna_js_refusal(env, refused, params->arena, params->error);
        return TA_ERR_INVALID;
    }
    napi_value bytes = tna_js_property(env, answer, "bytes");
    bool typed = false;
    napi_typedarray_type kind = napi_uint8_array;
    size_t length = 0;
    void *data = NULL;
    if (bytes == NULL || napi_is_typedarray(env, bytes, &typed) != napi_ok || !typed ||
        napi_get_typedarray_info(env, bytes, &kind, &length, &data, NULL, NULL) != napi_ok ||
        kind != napi_uint8_array || length > UINT32_MAX) {
        tna_throw(env, "invalid", "encode: a codec's encode answers bytes");
        return tna_js_threw(params->error);
    }
    uint8_t *copy = ta_arena_alloc(params->arena, length == 0 ? 1 : length, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    if (length != 0) {
        memcpy(copy, data, length);
    }
    *params->out = (ta_bytes){ copy, (uint32_t)length };
    tna_js_spans(env, tna_js_property(env, answer, "spans"), params->spans);
    return TA_OK;
}

ta_result tna_js_codec_init(tna_js_codec *codec, tna_call *call, ta_str name, napi_value decode, napi_value encode,
                            ta_chart_codec *out) {
    if (name.len >= sizeof codec->name) {
        return TA_ERR_INVALID;
    }
    *codec = (tna_js_codec){ .call = call, .decode = decode, .encode = encode };
    memcpy(codec->name, name.ptr, name.len);
    codec->vtable = (ta_chart_codec_vtable){
        .info = { .name = { codec->name, name.len },
                  .flags = (decode != NULL ? TA_CHART_CODEC_DECODES : 0u) | (encode != NULL ? TA_CHART_CODEC_ENCODES : 0u) },
        .decode = decode != NULL ? tna_js_decode : NULL,
        .encode = encode != NULL ? tna_js_encode : NULL,
    };
    *out = (ta_chart_codec){ .self = codec, .v = &codec->vtable };
    return TA_OK;
}

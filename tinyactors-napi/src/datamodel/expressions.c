/* expressions.c — a chart's expressions, compiled once at chart_attach
 * (bindings.md, section 8), as quickjs's shared realm compiles them
 * (tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:220-260):
 * a value as a strict function inside `with (scope)`, so assigning an
 * undeclared name throws; a script as a strict function body; a location
 * also as a setter. They run in the application's realm: chart code is
 * trusted (README.md). */
#include "datamodel.h"

#include <stdlib.h>
#include <string.h>

/* `pre` + source + `post`, run as a script: the function it gives, or NULL
 * when it did not compile (the exception is cleared). */
typedef struct ta_napi_wrapping {
    const char *pre;
    ta_string_view source;
    const char *post;
} ta_napi_wrapping;

static napi_ref ta_napi_compile_wrapped(napi_env env, const ta_napi_wrapping *wrapping) {
    size_t pre = strlen(wrapping->pre), post = strlen(wrapping->post), length = pre + wrapping->source.length + post;
    char *text = (char *)malloc(length + 1);
    if (!text) return NULL;
    memcpy(text, wrapping->pre, pre);
    if (wrapping->source.length) memcpy(text + pre, wrapping->source.pointer, wrapping->source.length);
    memcpy(text + pre + wrapping->source.length, wrapping->post, post);
    napi_value script, function;
    napi_ref reference = NULL;
    if (napi_create_string_utf8(env, text, length, &script) == napi_ok &&
        napi_run_script(env, script, &function) == napi_ok)
        napi_create_reference(env, function, 1, &reference);
    free(text);
    if (!reference) {
        napi_value thrown;
        napi_get_and_clear_last_exception(env, &thrown);
    }
    return reference;
}

/* A trailing ';' is tolerated in a value, as in expr="new Foo();" (quickjs's
 * trim_expression). */
static ta_string_view ta_napi_trim(ta_string_view source) {
    while (source.length) {
        char c = source.pointer[source.length - 1];
        if (c != ';' && c != ' ' && c != '\n' && c != '\t' && c != '\r') break;
        source.length--;
    }
    return source;
}

ta_string_view ta_napi_chart_source(const ta_napi_chart *chart, ta_expression expression) {
    ta_expression_info info;
    ta_string_view none = {"", 0};
    if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, expression}, &info) != TA_OK) return none;
    return info.source;
}

ta_status ta_napi_chart_compile(ta_napi_datamodel *datamodel, const ta_datamodel_chart *attach, ta_napi_chart **out) {
    napi_env env = datamodel->env;
    ta_napi_chart *chart = (ta_napi_chart *)calloc(1, sizeof *chart);
    if (!chart) return TA_ERROR_NO_MEMORY;
    chart->image = attach->image;
    chart->chart = attach->chart;
    chart->count = ta_image_expression_count(attach->image, attach->chart);
    chart->kinds = (uint32_t *)calloc(chart->count + 1, sizeof *chart->kinds);
    chart->getters = (napi_ref *)calloc(chart->count + 1, sizeof *chart->getters);
    chart->setters = (napi_ref *)calloc(chart->count + 1, sizeof *chart->setters);
    if (!chart->kinds || !chart->getters || !chart->setters) {
        ta_napi_chart_free(datamodel, chart);
        return TA_ERROR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < chart->count; i++) {
        ta_expression_info info;
        if (ta_image_expression(attach->image, (ta_chart_item){attach->chart, i}, &info) != TA_OK) continue;
        chart->kinds[i] = info.kind;
        napi_handle_scope scope;
        if (napi_open_handle_scope(env, &scope) != napi_ok) continue;
        if (info.kind == TA_EXPRESSION_SCRIPT) {
            ta_napi_wrapping script = {"(function(__s){with(__s){(function(){'use strict';\n", info.source, "\n})();}})"};
            chart->getters[i] = ta_napi_compile_wrapped(env, &script);
        } else if (info.kind != TA_EXPRESSION_CONTENT) {
            ta_napi_wrapping value = {"(function(__s){with(__s){return (function(){'use strict';return (\n",
                                      ta_napi_trim(info.source), "\n);})();}})"};
            chart->getters[i] = ta_napi_compile_wrapped(env, &value);
        }
        if (info.kind == TA_EXPRESSION_LOCATION) {
            ta_napi_wrapping setter = {"(function(__s,__ta_v){with(__s){(\n", info.source, "\n) = __ta_v;}})"};
            chart->setters[i] = ta_napi_compile_wrapped(env, &setter);
        }
        napi_close_handle_scope(env, scope);
    }
    *out = chart;
    return TA_OK;
}

void ta_napi_chart_free(ta_napi_datamodel *datamodel, ta_napi_chart *chart) {
    for (uint32_t i = 0; chart->getters && i < chart->count; i++)
        if (chart->getters[i]) ta_napi_defer_drop(datamodel, chart->getters[i]);
    for (uint32_t i = 0; chart->setters && i < chart->count; i++)
        if (chart->setters[i]) ta_napi_defer_drop(datamodel, chart->setters[i]);
    free(chart->kinds);
    free(chart->getters);
    free(chart->setters);
    free(chart);
}

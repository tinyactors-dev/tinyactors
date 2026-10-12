/* expressions.c — a chart's expressions, compiled once at chart_attach
 * (bindings.md, section 8), as quickjs's shared realm compiles them
 * (tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:220-260):
 * a value as a strict function inside `with (scope)`, so assigning an
 * undeclared name throws; a script as a strict function body; a location
 * also as a setter. They run in the application's realm: chart code is
 * trusted (README.md). The functions are a chart row's two JavaScript
 * arrays; nothing here allocates. */
#include "datamodel.h"

#include <string.h>

/* `pre` + source + `post`, run as a script: the function it gives, or
 * undefined when it did not compile (the exception is cleared). The text
 * is put together in a room of the call's. */
typedef struct ta_napi_wrapping {
    const char *pre;
    ta_string_view source;
    const char *post;
} ta_napi_wrapping;

static napi_value ta_napi_compile_wrapped(ta_napi_datamodel *datamodel, const ta_napi_wrapping *wrapping) {
    napi_env env = datamodel->env;
    size_t pre = strlen(wrapping->pre), post = strlen(wrapping->post), length = pre + wrapping->source.length + post;
    napi_value script, function = NULL, undefined;
    napi_get_undefined(env, &undefined);
    char stack[512];
    ta_napi_room text;
    ta_napi_room_init(&text, env, (ta_buffer){stack, sizeof stack, 0});
    if (!ta_napi_room_reserve(&text, length)) return undefined;
    memcpy(text.pointer, wrapping->pre, pre);
    if (wrapping->source.length) memcpy(text.pointer + pre, wrapping->source.pointer, wrapping->source.length);
    memcpy(text.pointer + pre + wrapping->source.length, wrapping->post, post);
    napi_status status = napi_create_string_utf8(env, text.pointer, length, &script);
    ta_napi_room_free(&text);
    if (status != napi_ok || napi_run_script(env, script, &function) != napi_ok) {
        napi_value thrown;
        napi_get_and_clear_last_exception(env, &thrown);
        return undefined;
    }
    return function;
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

ta_napi_expression ta_napi_chart_expression(const ta_napi_chart_row *chart, ta_expression expression) {
    ta_expression_info info;
    ta_napi_expression none = {{"", 0}, 0};
    if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, expression}, &info) != TA_OK) return none;
    ta_napi_expression found = {info.source, info.kind};
    return found;
}

ta_status ta_napi_chart_compile(ta_napi_datamodel *datamodel, const ta_datamodel_chart *attach, uint64_t *out) {
    napi_env env = datamodel->env;
    uint32_t count = ta_image_expression_count(attach->image, attach->chart);
    napi_value getters, setters;
    napi_ref getters_ref = NULL, setters_ref = NULL;
    if (napi_create_array_with_length(env, count, &getters) != napi_ok ||
        napi_create_array_with_length(env, count, &setters) != napi_ok)
        return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        ta_expression_info info;
        if (ta_image_expression(attach->image, (ta_chart_item){attach->chart, i}, &info) != TA_OK) continue;
        napi_handle_scope scope;
        if (napi_open_handle_scope(env, &scope) != napi_ok) continue;
        if (info.kind == TA_EXPRESSION_SCRIPT) {
            ta_napi_wrapping script = {"(function(__s){with(__s){(function(){'use strict';\n", info.source, "\n})();}})"};
            napi_set_element(env, getters, i, ta_napi_compile_wrapped(datamodel, &script));
        } else if (info.kind != TA_EXPRESSION_CONTENT) {
            ta_napi_wrapping value = {"(function(__s){with(__s){return (function(){'use strict';return (\n",
                                      ta_napi_trim(info.source), "\n);})();}})"};
            napi_set_element(env, getters, i, ta_napi_compile_wrapped(datamodel, &value));
        }
        if (info.kind == TA_EXPRESSION_LOCATION) {
            ta_napi_wrapping setter = {"(function(__s,__ta_v){with(__s){(\n", info.source, "\n) = __ta_v;}})"};
            napi_set_element(env, setters, i, ta_napi_compile_wrapped(datamodel, &setter));
        }
        napi_close_handle_scope(env, scope);
    }
    if (napi_create_reference(env, getters, 1, &getters_ref) != napi_ok ||
        napi_create_reference(env, setters, 1, &setters_ref) != napi_ok) {
        if (getters_ref) napi_delete_reference(env, getters_ref);
        return TA_ERROR_NO_MEMORY;
    }
    uint64_t handle = ta_napi_table_add(&datamodel->charts);
    ta_napi_chart_row *chart = (ta_napi_chart_row *)ta_napi_table_get(&datamodel->charts, handle);
    if (!chart) {
        napi_delete_reference(env, getters_ref);
        napi_delete_reference(env, setters_ref);
        return TA_ERROR_NO_MEMORY;
    }
    chart->image = attach->image;
    chart->chart = attach->chart;
    chart->count = count;
    chart->getters = getters_ref;
    chart->setters = setters_ref;
    *out = handle;
    return TA_OK;
}

void ta_napi_chart_unuse(ta_napi_datamodel *datamodel, uint64_t handle) {
    ta_napi_chart_row *chart = (ta_napi_chart_row *)ta_napi_table_get(&datamodel->charts, handle);
    if (!chart || --chart->row.references) return;
    chart->row.references = 1; /* live for the remove */
    ta_napi_defer_drop(&datamodel->drops, chart->getters);
    ta_napi_defer_drop(&datamodel->drops, chart->setters);
    ta_napi_table_remove(&datamodel->charts, handle);
}

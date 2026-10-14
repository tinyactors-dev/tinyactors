/* compile.c — public compiler API: queue sources, compile charts in index
 * order (linked/inline children are appended to the queue), link event
 * names, emit and self-verify the image. */
#include "compile/compile_internal.h"

#include <string.h>

ta_status ta_compiler_resolve(ta_compiler *compiler, const ta_resolve_request *request,
                              ta_source *source) {
    ta_resolve_result resolved = {0};
    const ta_compile_options *options = compiler->options;
    ta_status status = options->resolve(options->resolve_context, request, &resolved);
    if (status == TA_OK) {
        *source = resolved.source;
        return status;
    }
    ta_string_view name = resolved.source.name.length && resolved.source.name.pointer
                             ? resolved.source.name : request->source;
    char *file = ta_compile_copy(&compiler->arena, name, &compiler->out_of_memory);
    ta_string_view message = resolved.error.message;
    if (!message.pointer || !message.length)
        message = (ta_string_view){"cannot resolve src", 18};
    /* Diagnostics have a 511-byte bound, independent of resolver storage. */
    uint32_t length = message.length > 511 ? 511 : message.length;
    ta_diagnostics_add(&compiler->diagnostics, &(ta_diagnostic_input){
        TA_DIAGNOSTIC_ERROR, file, resolved.error.line, resolved.error.column, "%.*s"},
        (int)length, message.pointer);
    return status;
}

struct ta_compile_result {
    ta_allocator allocator;
    void *image;
    size_t length;
    ta_vector diagnostics;
    ta_growable_arena arena;
};

static ta_compile_result *result_new(const ta_compile_options *options) {
    ta_allocator allocator = ta_compile_default_allocator();
    if (options && options->alloc.alloc && options->alloc.free) allocator = options->alloc;
    ta_compile_result *result =
        (ta_compile_result *)allocator.alloc(allocator.context, sizeof *result, 8);
    if (!result) return NULL;
    memset(result, 0, sizeof *result);
    result->allocator = allocator;
    ta_growable_arena_init(&result->arena, &(ta_growable_arena_config){
        sizeof(ta_growable_arena_config), 0, 32768, &result->allocator});
    ta_vector_init(&result->diagnostics, &result->allocator, sizeof(ta_diagnostic));
    return result;
}

static void compiler_init(ta_compiler *compiler, ta_compile_result *result,
                          const ta_compile_options *options) {
    memset(compiler, 0, sizeof *compiler);
    compiler->allocator = &result->allocator;
    compiler->options = options;
    if (options &&
        options->size >= offsetof(ta_compile_options, abort_flag) + sizeof options->abort_flag)
        compiler->abort_flag = options->abort_flag;
    ta_growable_arena_init(&compiler->arena, &(ta_growable_arena_config){
        sizeof(ta_growable_arena_config), 0, 32768, compiler->allocator});
    compiler->diagnostics.list = result->diagnostics;
    compiler->diagnostics.arena = &result->arena;
    ta_vector_init(&compiler->strings, compiler->allocator, 1);
    ta_vector_init(&compiler->charts, compiler->allocator, sizeof(ta_chart_record));
    ta_vector_init(&compiler->states, compiler->allocator, sizeof(ta_state_record));
    ta_vector_init(&compiler->transitions, compiler->allocator, sizeof(ta_transition_record));
    ta_vector_init(&compiler->u32, compiler->allocator, sizeof(uint32_t));
    ta_vector_init(&compiler->code, compiler->allocator, sizeof(uint32_t));
    ta_vector_init(&compiler->expressions, compiler->allocator, sizeof(ta_expression_record));
    ta_vector_init(&compiler->captures, compiler->allocator, sizeof(ta_capture_record));
    ta_vector_init(&compiler->sends, compiler->allocator, sizeof(ta_send_record));
    ta_vector_init(&compiler->invokes, compiler->allocator, sizeof(ta_invoke_record));
    ta_vector_init(&compiler->data, compiler->allocator, sizeof(ta_data_record));
    ta_vector_init(&compiler->actors, compiler->allocator, sizeof(ta_actor_record));
    ta_vector_init(&compiler->event_names, compiler->allocator, sizeof(ta_string_view));
    ta_vector_init(&compiler->event_nodes, compiler->allocator, sizeof(ta_event_node_record));
    ta_vector_init(&compiler->event_name_node, compiler->allocator, sizeof(uint32_t));
    ta_vector_init(&compiler->pending, compiler->allocator, sizeof(ta_pending_document));
    ta_string_map_init(&compiler->string_map, compiler->allocator);
    ta_string_map_init(&compiler->event_name_map, compiler->allocator);
    ta_string_map_init(&compiler->source_map, compiler->allocator);
}

static int compiler_out_of_memory(const ta_compiler *compiler) {
    const ta_vector *vs[] = {&compiler->strings,        &compiler->charts,      &compiler->states,
                             &compiler->transitions,    &compiler->u32,         &compiler->code,
                             &compiler->expressions,    &compiler->captures,    &compiler->sends,
                             &compiler->invokes,        &compiler->data,        &compiler->actors,
                             &compiler->event_names,    &compiler->event_nodes, &compiler->pending,
                             &compiler->event_name_node};
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++)
        if (vs[i]->out_of_memory) return 1;
    return compiler->out_of_memory || compiler->diagnostics.out_of_memory ||
           compiler->diagnostics.list.out_of_memory || compiler->string_map.out_of_memory ||
           compiler->event_name_map.out_of_memory || compiler->source_map.out_of_memory;
}

static void compiler_free(ta_compiler *compiler, ta_compile_result *result) {
    result->diagnostics = compiler->diagnostics.list; /* diagnostics outlive the builder */
    ta_vector *vs[] = {&compiler->strings,        &compiler->charts,      &compiler->states,
                       &compiler->transitions,    &compiler->u32,         &compiler->code,
                       &compiler->expressions,    &compiler->captures,    &compiler->sends,
                       &compiler->invokes,        &compiler->data,        &compiler->actors,
                       &compiler->event_names,    &compiler->event_nodes, &compiler->pending,
                       &compiler->event_name_node};
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++)
        ta_vector_free(vs[i]);
    ta_string_map_free(&compiler->string_map);
    ta_string_map_free(&compiler->event_name_map);
    ta_string_map_free(&compiler->source_map);
    ta_growable_arena_free(&compiler->arena);
}

/* Link + emit + verify after every queued chart has been compiled. */
static ta_status finish(ta_compiler *compiler, ta_compile_result *result) {
    if (ta_compiler_stop(compiler)) {
        ta_diagnostics_add(&compiler->diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, NULL, 0, 0, "compilation aborted"});
        compiler_free(compiler, result);
        return TA_ERROR_ABORTED;
    }
    if (!compiler->diagnostics.errors && !compiler_out_of_memory(compiler)) {
        ta_compiler_link_names(compiler);
        if (ta_compiler_stop(compiler)) return finish(compiler, result);
        if (!compiler_out_of_memory(compiler) &&
            !ta_compiler_emit(compiler, &result->image, &result->length))
            ta_diagnostics_add(&compiler->diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, NULL, 0, 0, "image too large"});
    }
    if (compiler_out_of_memory(compiler))
        ta_diagnostics_add(&compiler->diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, NULL, 0, 0, "out of memory"});
    if (result->image && !compiler->diagnostics.errors) {
        ta_image check;
        if (ta_image_open((ta_bytes){result->image, result->length}, &check) != TA_OK)
            ta_diagnostics_add(&compiler->diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, NULL, 0, 0, "internal error: emitted image failed verification"});
    }
    int werror = compiler->options && (compiler->options->flags & TA_COMPILE_WERROR);
    int failed = compiler->diagnostics.errors || (werror && compiler->diagnostics.warnings);
    compiler_free(compiler, result);
    if (failed && result->image) {
        result->allocator.free(result->allocator.context, result->image, result->length);
        result->image = NULL;
        result->length = 0;
    }
    return failed ? TA_ERROR_INVALID : TA_OK;
}

ta_status ta_compile_charts(const ta_compile_options *options, ta_source_list list,
                            ta_compile_result **out) {
    const ta_source *charts = list.pointer;
    uint32_t n = list.count;
    *out = NULL;
    ta_compile_result *result = result_new(options);
    if (!result) return TA_ERROR_NO_MEMORY;
    *out = result;
    if (!n) return TA_ERROR_INVALID;
    ta_compiler compiler;
    compiler_init(&compiler, result, options);
    for (uint32_t i = 0; i < n && !ta_compiler_stop(&compiler); i++) {
        char *name = ta_compile_copy(&compiler.arena, (ta_string_view){charts[i].name.pointer ? charts[i].name.pointer : "chart", (uint32_t)(charts[i].name.pointer ? charts[i].name.length : 5)}, &compiler.out_of_memory);
        if (!name) break;
        if (ta_string_map_get(&compiler.source_map, name, (uint32_t)strlen(name)) != TA_NIL)
            ta_diagnostics_add(&compiler.diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, name, 0, 0, "duplicate source name"});
        ta_compiler_queue(&compiler, &(ta_pending_document){NULL, name, charts[i].text.pointer ? charts[i].text.pointer : "", charts[i].text.length});
    }
    ta_actor_record *actor = (ta_actor_record *)ta_vector_push(&compiler.actors);
    if (actor) {
        actor->chart = 0;
        actor->name = TA_NIL; /* patched after compile: chart 0's name */
    }
    for (uint32_t i = 0; i < compiler.pending.length && !compiler_out_of_memory(&compiler) &&
                         !ta_compiler_stop(&compiler);
         i++)
        ta_compiler_compile_chart(&compiler, i);
    if (actor && compiler.charts.length)
        TA_VECTOR_AT(&compiler.actors, ta_actor_record, 0).name =
            TA_VECTOR_AT(&compiler.charts, ta_chart_record, 0).name;
    return finish(&compiler, result);
}

ta_status ta_compile_system(const ta_compile_options *options, const ta_source *manifest,
                            ta_compile_result **out) {
    *out = NULL;
    ta_compile_result *result = result_new(options);
    if (!result) return TA_ERROR_NO_MEMORY;
    *out = result;
    ta_compiler compiler;
    compiler_init(&compiler, result, options);
    const char *mname = manifest->name.pointer ? manifest->name.pointer : "system";
    char *file = ta_compile_copy(&compiler.arena, (ta_string_view){mname, (uint32_t)(manifest->name.pointer ? manifest->name.length : 6)}, &compiler.out_of_memory);
    char *text =
        ta_compile_copy(&compiler.arena, (ta_string_view){manifest->text.pointer, (uint32_t)(manifest->text.length)}, &compiler.out_of_memory);
    ta_xml_node *root = file && text ? ta_xml_parse(&compiler.arena, &(ta_xml_parse_request){
        {text, manifest->text.length}, &compiler.diagnostics, compiler.allocator,
        file, compiler.abort_flag, &compiler.aborted, &compiler.out_of_memory})
                                     : NULL;
    ta_string_map alias;
    ta_string_map_init(&alias, compiler.allocator);
    if (root && strcmp(root->name, "system") != 0)
        ta_diagnostics_add(&compiler.diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, file, root->line, root->column, "manifest root must be <system>"});
    for (ta_xml_node *node = root ? root->first : NULL;
         node && !compiler.diagnostics.errors && !ta_compiler_stop(&compiler); node = node->next) {
        if (strcmp(node->name, "chart") == 0) {
            const char *source = ta_xml_attribute_value(node, "src");
            const char *name = ta_xml_attribute_value(node, "name");
            ta_source s;
            ta_resolve_request request = {{file, (uint32_t)strlen(file)},
                                          {source, source ? (uint32_t)strlen(source) : 0},
                                          &compiler.arena};
            if (!source || !options || !options->resolve) {
                ta_diagnostics_add(&compiler.diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, file, node->line, node->column, "cannot resolve chart src"});
                break;
            }
            if (ta_compiler_resolve(&compiler, &request, &s) != TA_OK) break;
            char *canon =
                s.name.pointer && s.name.length
                    ? ta_compile_copy(&compiler.arena, (ta_string_view){s.name.pointer, (uint32_t)(s.name.length)}, &compiler.out_of_memory)
                    : ta_compile_copy(&compiler.arena, (ta_string_view){source, (uint32_t)(strlen(source))}, &compiler.out_of_memory);
            uint32_t index = ta_compiler_queue(&compiler, &(ta_pending_document){NULL, canon ? canon : source, s.text.pointer, s.text.length});
            if (canon && strcmp(canon, source) != 0) /* nested src="" may use the short name */
                ta_string_map_put(&compiler.source_map, source, (uint32_t)strlen(source), index);
            if (name) ta_string_map_put(&alias, name, (uint32_t)strlen(name), index);
            ta_string_map_put(&alias, source, (uint32_t)strlen(source), index);
        } else if (strcmp(node->name, "actor") == 0) {
            const char *name = ta_xml_attribute_value(node, "name");
            const char *chart = ta_xml_attribute_value(node, "chart");
            uint32_t index =
                chart ? ta_string_map_get(&alias, chart, (uint32_t)strlen(chart)) : TA_NIL;
            if (!name || index == TA_NIL) {
                ta_diagnostics_add(&compiler.diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, file, node->line, node->column, "<actor> needs name and a known chart"});
                break;
            }
            ta_actor_record *actor = (ta_actor_record *)ta_vector_push(&compiler.actors);
            if (actor) {
                actor->name = ta_compiler_string(&compiler, name, (uint32_t)strlen(name));
                actor->chart = index;
            }
        } else {
            ta_diagnostics_add(&compiler.diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, file, node->line, node->column, "unknown manifest element"});
        }
    }
    if (!compiler.pending.length && !compiler.diagnostics.errors)
        ta_diagnostics_add(&compiler.diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, file, 0, 0, "manifest declares no charts"});
    /* chart display names from manifest aliases */
    for (uint32_t i = 0; i < compiler.pending.length && !compiler_out_of_memory(&compiler) &&
                         !ta_compiler_stop(&compiler);
         i++)
        ta_compiler_compile_chart(&compiler, i);
    for (uint32_t i = 0; root && i < alias.capacity; i++) {
        const ta_string_map_entry *entry = &alias.slots[i];
        if (!entry->key || entry->value >= compiler.charts.length) continue;
        ta_chart_record *chart_record =
            &TA_VECTOR_AT(&compiler.charts, ta_chart_record, entry->value);
        ta_pending_document *pending =
            &TA_VECTOR_AT(&compiler.pending, ta_pending_document, entry->value);
        /* a name="" alias (not one of the src spellings) becomes the chart name */
        int is_source = strlen(pending->name) >= entry->length &&
                        memcmp(pending->name + strlen(pending->name) - entry->length, entry->key,
                               entry->length) == 0;
        if (!is_source)
            chart_record->name = ta_compiler_string(&compiler, entry->key, entry->length);
    }
    ta_string_map_free(&alias);
    return finish(&compiler, result);
}

ta_string_view ta_compile_image(const ta_compile_result *result) {
    ta_string_view s = {result ? (const char *)result->image : NULL,
                        result ? (uint32_t)result->length : 0};
    return s;
}

uint32_t ta_compile_diagnostics(const ta_compile_result *result, const ta_diagnostic **out) {
    if (!result) {
        *out = NULL;
        return 0;
    }
    *out = (const ta_diagnostic *)(const void *)result->diagnostics.data;
    return result->diagnostics.length;
}

void ta_compile_free(ta_compile_result *result) {
    if (!result) return;
    ta_allocator allocator = result->allocator;
    if (result->image) allocator.free(allocator.context, result->image, result->length);
    ta_vector_free(&result->diagnostics);
    ta_growable_arena_free(&result->arena);
    allocator.free(allocator.context, result, sizeof *result);
}

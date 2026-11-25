/* input.c — what a command reads: files, told apart by their first bytes,
 * and the one image they make (tool.h). Freestanding: files come through the
 * platform, memory from the invocation's arena. */
#include "tool.h"

#include "base/memory.h"

/* ---- telling inputs apart ---------------------------------------------------------- */

static int starts_with(ta_bytes bytes, size_t at, const char *text) {
    for (size_t i = 0; text[i]; i++)
        if (at + i >= bytes.length || ((const char *)bytes.pointer)[at + i] != text[i]) return 0;
    return 1;
}

/* Past the next occurrence of `end` from `at` (bytes.length when there is none). */
static size_t past(ta_bytes bytes, size_t at, const char *end) {
    for (; at < bytes.length; at++)
        if (starts_with(bytes, at, end)) {
            while (*end++)
                at++;
            return at;
        }
    return bytes.length;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* An image by its magic; XML by the local name of its root element, past
 * the byte order mark, the declaration, comments and a doctype. TA_NIL: none
 * of them. */
static uint32_t classify(ta_bytes bytes) {
    const char *text = (const char *)bytes.pointer;
    if (starts_with(bytes, 0, "TAI1")) return TOOL_INPUT_IMAGE;
    size_t at = starts_with(bytes, 0, "\xEF\xBB\xBF") ? 3 : 0;
    for (;;) {
        while (at < bytes.length && is_space(text[at]))
            at++;
        if (starts_with(bytes, at, "<?")) at = past(bytes, at, "?>");
        else if (starts_with(bytes, at, "<!--")) at = past(bytes, at, "-->");
        else if (starts_with(bytes, at, "<!")) at = past(bytes, at, ">");
        else break;
    }
    if (at >= bytes.length || text[at] != '<') return TA_NIL;
    size_t start = ++at;
    while (at < bytes.length && !is_space(text[at]) && text[at] != '>' && text[at] != '/') {
        if (text[at] == ':') start = at + 1; /* a prefix: the local name follows */
        at++;
    }
    ta_string_view name = {text + start, (uint32_t)(at - start)};
    if (tool_equal(name, ta_string("scxml"))) return TOOL_INPUT_CHART;
    if (tool_equal(name, ta_string("system"))) return TOOL_INPUT_MANIFEST;
    return TA_NIL;
}

/* ---- the resolver: a src relative to the file that names it -------------------------- */

static ta_status resolve(void *context, const ta_resolve_request *request, ta_resolve_result *out) {
    tool *t = (tool *)context;
    tool_text path;
    tool_text_init(t, &path);
    int absolute = request->source.length && request->source.pointer[0] == '/';
    if (!absolute) {
        uint32_t directory = request->base.length;
        while (directory && request->base.pointer[directory - 1] != '/')
            directory--;
        tool_text_append(&path, (ta_string_view){request->base.pointer, directory});
    }
    tool_text_append(&path, request->source);
    ta_string_view name = tool_text_view(&path);
    out->source.name = name;
    tool_read read = {name, t->arena};
    ta_bytes bytes;
    if (!name.pointer || t->platform->read(t->platform->context, &read, &bytes) != TA_OK) {
        out->error.message = ta_string("cannot read the file");
        return TA_ERROR_INVALID;
    }
    out->source.text = (ta_string_view){(const char *)bytes.pointer, (uint32_t)bytes.length};
    return TA_OK;
}

/* ---- loading -------------------------------------------------------------------------- */

static ta_string_view copied(tool *t, ta_string_view text) {
    ta_string_view copy = {(const char *)0, 0};
    if (!text.length) return copy;
    char *room = (char *)ta_growable_arena_alloc(t->arena, text.length, 1);
    if (!room) return copy;
    ta_memcpy(room, text.pointer, text.length);
    copy.pointer = room;
    copy.length = text.length;
    return copy;
}

/* The compile's diagnostics and image, copied into the arena before the
 * result is freed. */
static ta_status keep(tool *t, const ta_compile_result *result, tool_image *out) {
    const ta_diagnostic *diagnostics;
    uint32_t count = ta_compile_diagnostics(result, &diagnostics);
    ta_diagnostic *kept =
        count ? (ta_diagnostic *)ta_growable_arena_alloc(t->arena, sizeof *kept * count, 8) : 0;
    if (count && !kept) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        kept[i] = diagnostics[i];
        kept[i].file = copied(t, diagnostics[i].file);
        kept[i].message = copied(t, diagnostics[i].message);
    }
    out->diagnostics = kept;
    out->diagnostic_count = count;
    ta_string_view image = ta_compile_image(result);
    if (!image.length) return TA_OK;
    unsigned char *bytes = (unsigned char *)ta_growable_arena_alloc(t->arena, image.length, 8);
    if (!bytes) return TA_ERROR_NO_MEMORY;
    ta_memcpy(bytes, image.pointer, image.length);
    out->bytes = (ta_bytes){bytes, image.length};
    return TA_OK;
}

static void say_cannot_read(tool *t, ta_string_view path) {
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string("cannot read "));
    tool_text_append(&text, path);
    tool_complain(t, tool_text_view(&text));
}

/* A diagnostic of the tool's own (an image that does not verify). */
static void own_diagnostic(tool *t, tool_image *out, ta_diagnostic diagnostic) {
    ta_diagnostic *kept = (ta_diagnostic *)ta_growable_arena_alloc(t->arena, sizeof *kept, 8);
    if (!kept) return;
    *kept = diagnostic;
    out->diagnostics = kept;
    out->diagnostic_count = 1;
}

ta_status tool_load(tool *t, tool_inputs inputs, tool_image *out) {
    ta_memset(out, 0, sizeof *out);
    ta_source *sources = (ta_source *)ta_growable_arena_alloc(t->arena, sizeof *sources * inputs.count, 8);
    if (!sources) return t->exit = TOOL_EXIT_FAILURE, TA_ERROR_NO_MEMORY;
    uint32_t kind = TA_NIL;
    for (uint32_t i = 0; i < inputs.count; i++) {
        ta_bytes bytes;
        tool_read read = {inputs.paths[i], t->arena};
        if (t->platform->read(t->platform->context, &read, &bytes) != TA_OK) {
            say_cannot_read(t, inputs.paths[i]);
            t->exit = TOOL_EXIT_FAILURE;
            return TA_ERROR_INVALID;
        }
        uint32_t this_kind = classify(bytes);
        if (this_kind == TA_NIL || (kind != TA_NIL && (this_kind != kind || kind != TOOL_INPUT_CHART))) {
            tool_text text;
            tool_text_init(t, &text);
            tool_text_append(&text, inputs.paths[i]);
            tool_text_append(&text, this_kind == TA_NIL
                                        ? ta_string(" is neither an image (.tai) nor an SCXML chart "
                                                    "nor a system manifest")
                                        : ta_string(": give one image, one manifest, or charts"));
            tool_complain(t, tool_text_view(&text));
            t->exit = this_kind == TA_NIL ? TOOL_EXIT_REFUSED : TOOL_EXIT_USAGE;
            return TA_ERROR_INVALID;
        }
        kind = this_kind;
        sources[i].name = inputs.paths[i];
        sources[i].text = (ta_string_view){(const char *)bytes.pointer, (uint32_t)bytes.length};
        if (kind == TOOL_INPUT_IMAGE) out->bytes = bytes;
    }
    out->kind = kind;
    if (kind != TOOL_INPUT_IMAGE) {
        ta_compile_options options;
        ta_memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.flags = inputs.flags;
        options.alloc = ta_growable_arena_allocator(t->arena);
        options.resolve = resolve;
        options.resolve_context = t;
        ta_compile_result *result = 0;
        out->status = kind == TOOL_INPUT_MANIFEST
                          ? ta_compile_system(&options, &sources[0], &result)
                          : ta_compile_charts(&options, (ta_source_list){sources, inputs.count}, &result);
        ta_status kept = result ? keep(t, result, out) : TA_ERROR_NO_MEMORY;
        ta_compile_free(result);
        if (kept != TA_OK) return t->exit = TOOL_EXIT_FAILURE, kept;
        if (out->status != TA_OK) {
            t->exit = TOOL_EXIT_REFUSED;
            return out->status;
        }
    }
    out->status = ta_image_open(out->bytes, &out->image);
    if (out->status != TA_OK) {
        own_diagnostic(t, out, (ta_diagnostic){TA_DIAGNOSTIC_ERROR, 0, 0, 0, inputs.paths[0],
                                               ta_string("the image does not verify: it is damaged, "
                                                         "or of another format version")});
        t->exit = TOOL_EXIT_REFUSED;
        return out->status;
    }
    out->opened = 1;
    return TA_OK;
}

static ta_string_view severity_name(uint32_t severity) {
    return severity == TA_DIAGNOSTIC_ERROR     ? ta_string("error")
           : severity == TA_DIAGNOSTIC_WARNING ? ta_string("warning")
                                               : ta_string("note");
}

void tool_report_diagnostics(tool *t, const tool_image *image) {
    ta_report *report = &t->report;
    ta_report_list(report, (ta_report_list_names){ta_string("diagnostics"), ta_string("diagnostic")});
    for (uint32_t i = 0; i < image->diagnostic_count; i++) {
        const ta_diagnostic *d = &image->diagnostics[i];
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("file"), d->file);
        ta_report_number(report, ta_string("line"), d->line);
        ta_report_number(report, ta_string("column"), d->column);
        ta_report_text(report, ta_string("severity"), severity_name(d->severity));
        ta_report_text(report, ta_string("message"), d->message);
        ta_report_end(report);
    }
    ta_report_end(report);
}

ta_status tool_select_charts(tool *t, tool_selection selection, tool_charts *out) {
    const tool_image *image = selection.image;
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&image->image, &info);
    uint8_t *selected = (uint8_t *)ta_growable_arena_alloc(t->arena, info.chart_count + 1u, 1);
    if (!selected) return t->exit = TOOL_EXIT_FAILURE, TA_ERROR_NO_MEMORY;
    int named = tool_count(t, TOOL_OPTION_CHART) > 0;
    for (uint32_t c = 0; c < info.chart_count; c++)
        selected[c] = (uint8_t)!named;
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope != TA_CLI_SCOPE_COMMAND || value->option.option != TOOL_OPTION_CHART)
            continue;
        uint32_t chart = ta_image_chart_by_name(&image->image, value->text);
        if (chart == TA_NIL && (selection.flags & TOOL_SELECT_NAMES_ELSEWHERE)) continue;
        if (chart == TA_NIL) {
            tool_text text;
            tool_text_init(t, &text);
            tool_text_append(&text, ta_string("no chart '"));
            tool_text_append(&text, value->text);
            tool_text_append(&text, ta_string("' in the image; it has "));
            for (uint32_t c = 0; c < info.chart_count; c++) {
                if (c) tool_text_append(&text, ta_string(", "));
                tool_text_append(&text, ta_image_chart_name(&image->image, c));
            }
            tool_complain(t, tool_text_view(&text));
            t->exit = TOOL_EXIT_USAGE;
            return TA_ERROR_INVALID;
        }
        selected[chart] = 1;
    }
    out->count = info.chart_count;
    out->selected = selected;
    return TA_OK;
}

ta_status tool_open(tool *t, tool_open_request request, tool_opened *out) {
    tool_inputs inputs = {request.count, request.paths,
                          tool_count(t, TOOL_OPTION_WARNINGS_AS_ERRORS) ? TA_COMPILE_WERROR : 0u};
    ta_status status = tool_load(t, inputs, &out->image);
    if (status != TA_OK) {
        if (t->exit == TOOL_EXIT_REFUSED) tool_report_diagnostics(t, &out->image);
        return status;
    }
    return tool_select_charts(t, (tool_selection){&out->image, request.flags}, &out->charts);
}

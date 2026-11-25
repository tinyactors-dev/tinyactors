/* input.c — what a command reads: files, told apart by their first bytes,
 * and the one image they make (tool.h). Freestanding: files come through the
 * platform, memory from the invocation's arena. */
#include "tool.h"

#include "base/memory.h"
#include <tinyactors/host.h>

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

/* An image, a snapshot or a blob by its magic (the four bytes a blob's
 * header starts with, as a little-endian machine writes them); XML by the
 * local name of its root element, past the byte order mark, the
 * declaration, comments and a doctype. TA_NIL: none of them. */
uint32_t tool_input_kind_of(ta_bytes bytes) {
    const char *text = (const char *)bytes.pointer;
    if (starts_with(bytes, 0, "TAI1")) return TOOL_INPUT_IMAGE;
    if (starts_with(bytes, 0, "tinyactors-snapshot ")) return TOOL_INPUT_SNAPSHOT;
    if (starts_with(bytes, 0, "TAHB") || starts_with(bytes, 0, "TAEX")) return TOOL_INPUT_BLOB;
    if (starts_with(bytes, 0, "TAHS")) return TOOL_INPUT_HOST_SNAPSHOT;
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

/* ---- the resolver: a src relative to the file that names it, then in each -I directory -- */

/* What the resolver reads with: the invocation (its -I directories and
 * platform), the arena it reads into, and, for sources given as text, the
 * documents a src names. */
typedef struct resolution {
    tool *t;
    ta_growable_arena *arena;
    const ta_source_list *documents;
} resolution;

/* `directory` and `source` joined into a path in the arena: "directory/source". */
static ta_string_view joined_path(const resolution *context, ta_string_view directory, ta_string_view source) {
    tool_text path;
    tool_text_over(&path, context->arena);
    tool_text_append(&path, directory);
    if (directory.length && directory.pointer[directory.length - 1] != '/')
        tool_text_append(&path, ta_string("/"));
    tool_text_append(&path, source);
    return tool_text_view(&path);
}

/* The candidate read into out->source when it can be: 1. */
static int found(const resolution *context, ta_string_view candidate, ta_resolve_result *out) {
    tool *t = context->t;
    tool_read read = {candidate, context->arena};
    ta_bytes bytes;
    if (!candidate.pointer || t->platform->read(t->platform->context, &read, &bytes) != TA_OK) return 0;
    out->source.name = candidate;
    out->source.text = (ta_string_view){(const char *)bytes.pointer, (uint32_t)bytes.length};
    return 1;
}

/* A src among the documents given as text: the one of that name. */
static ta_status resolve_document(const resolution *context, const ta_resolve_request *request,
                                  ta_resolve_result *out) {
    for (uint32_t i = 0; i < context->documents->count; i++)
        if (tool_equal(context->documents->pointer[i].name, request->source)) {
            out->source = context->documents->pointer[i];
            return TA_OK;
        }
    out->source.name = request->source;
    out->error.message = ta_string("no source of that name was given");
    return TA_ERROR_INVALID;
}

static ta_status resolve(void *opaque, const ta_resolve_request *request, ta_resolve_result *out) {
    const resolution *context = (const resolution *)opaque;
    if (context->documents) return resolve_document(context, request, out);
    tool *t = context->t;
    int absolute = request->source.length && request->source.pointer[0] == '/';
    uint32_t directory = request->base.length;
    while (!absolute && directory && request->base.pointer[directory - 1] != '/')
        directory--;
    tool_text first;
    tool_text_over(&first, context->arena);
    tool_text_append(&first, (ta_string_view){request->base.pointer, absolute ? 0 : directory});
    tool_text_append(&first, request->source);
    ta_string_view relative = tool_text_view(&first);
    if (found(context, relative, out)) return TA_OK;
    /* the include path, in the order given, for a relative src */
    uint32_t includes = absolute ? 0 : tool_count(t, TOOL_OPTION_INCLUDE);
    tool_text message;
    tool_text_over(&message, context->arena);
    tool_text_append(&message, ta_string("cannot read the file"));
    if (includes) {
        tool_text_append(&message, ta_string("; looked for it at "));
        tool_text_append(&message, relative);
    }
    for (uint32_t i = 0; i < t->parsed->value_count && includes; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope != TA_CLI_SCOPE_COMMAND || value->option.option != TOOL_OPTION_INCLUDE)
            continue;
        ta_string_view candidate = joined_path(context, value->text, request->source);
        if (found(context, candidate, out)) return TA_OK;
        tool_text_append(&message, ta_string(", "));
        tool_text_append(&message, candidate);
    }
    out->source.name = relative;
    out->error.message = tool_text_view(&message);
    return TA_ERROR_INVALID;
}

/* ---- loading -------------------------------------------------------------------------- */

static ta_string_view copied(ta_growable_arena *arena, ta_string_view text) {
    ta_string_view copy = {(const char *)0, 0};
    if (!text.length) return copy;
    char *room = (char *)ta_growable_arena_alloc(arena, text.length, 1);
    if (!room) return copy;
    ta_memcpy(room, text.pointer, text.length);
    copy.pointer = room;
    copy.length = text.length;
    return copy;
}

/* The compile's diagnostics and image, copied into the arena before the
 * result is freed. */
static ta_status keep(ta_growable_arena *arena, const ta_compile_result *result, tool_image *out) {
    const ta_diagnostic *diagnostics;
    uint32_t count = ta_compile_diagnostics(result, &diagnostics);
    ta_diagnostic *kept =
        count ? (ta_diagnostic *)ta_growable_arena_alloc(arena, sizeof *kept * count, 8) : 0;
    if (count && !kept) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        kept[i] = diagnostics[i];
        kept[i].file = copied(arena, diagnostics[i].file);
        kept[i].message = copied(arena, diagnostics[i].message);
    }
    out->diagnostics = kept;
    out->diagnostic_count = count;
    ta_string_view image = ta_compile_image(result);
    if (!image.length) return TA_OK;
    unsigned char *bytes = (unsigned char *)ta_growable_arena_alloc(arena, image.length, 8);
    if (!bytes) return TA_ERROR_NO_MEMORY;
    ta_memcpy(bytes, image.pointer, image.length);
    out->bytes = (ta_bytes){bytes, image.length};
    return TA_OK;
}

ta_status tool_compile_sources(tool *t, const tool_compilation *compilation, tool_image *out) {
    resolution context = {t, compilation->arena, compilation->documents};
    ta_compile_options options;
    ta_memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.flags = compilation->flags;
    options.alloc = ta_growable_arena_allocator(compilation->arena);
    options.resolve = resolve;
    options.resolve_context = &context;
    ta_compile_result *result = 0;
    out->status = compilation->kind == TOOL_INPUT_MANIFEST
                      ? ta_compile_system(&options, &compilation->sources.pointer[0], &result)
                      : ta_compile_charts(&options, compilation->sources, &result);
    ta_status kept = result ? keep(compilation->arena, result, out) : TA_ERROR_NO_MEMORY;
    ta_compile_free(result);
    return kept;
}

/* Why the inputs make no image, to be told (tool_load) or answered. */
static ta_status refuse(tool_image *out, const tool_text *text, uint32_t exit) {
    out->complaint = tool_text_view(text);
    out->exit = exit;
    return out->status = TA_ERROR_INVALID;
}

/* A diagnostic of the tool's own (an image that does not verify). */
static void own_diagnostic(ta_growable_arena *arena, tool_image *out, ta_diagnostic diagnostic) {
    ta_diagnostic *kept = (ta_diagnostic *)ta_growable_arena_alloc(arena, sizeof *kept, 8);
    if (!kept) return;
    *kept = diagnostic;
    out->diagnostics = kept;
    out->diagnostic_count = 1;
}

/* A snapshot's image: every live segment's verified, then the segment
 * asked for (inputs.segment), or the newest live one. A refusal is a
 * diagnostic, or a complaint for a segment the snapshot does not run. */
static ta_status snapshot_image(const tool_image_request *request, tool_image *out) {
    tool_inputs inputs = request->inputs;
    ta_bytes pack = out->bytes;
    ta_host_snapshot_contents contents;
    ta_memset(&contents, 0, sizeof contents);
    contents.size = sizeof contents;
    tool_text text;
    tool_text_over(&text, request->arena);
    uint32_t chosen = TA_NIL;
    out->status = ta_host_snapshot_unpack(pack, &contents);
    if (out->status != TA_OK)
        tool_text_append(&text, ta_string("the snapshot cannot be read: it is damaged, or of "
                                          "another format version"));
    for (uint32_t i = 0; out->status == TA_OK && i < contents.image_count; i++) {
        ta_bytes image = ta_host_snapshot_image(pack, i);
        if (!image.length) continue; /* a retired segment */
        ta_image opened;
        out->status = ta_image_open(image, &opened);
        if (out->status != TA_OK) {
            tool_text_append(&text, ta_string("the image of segment "));
            tool_text_number(&text, i);
            tool_text_append(&text, ta_string(" does not verify: it is damaged"));
        } else if (inputs.segment == TA_NIL || inputs.segment == i) {
            chosen = i;
        }
    }
    if (out->status != TA_OK) {
        own_diagnostic(request->arena, out,
                       (ta_diagnostic){TA_DIAGNOSTIC_ERROR, 0, 0, 0, inputs.paths[0], tool_text_view(&text)});
        out->exit = TOOL_EXIT_REFUSED;
        return out->status;
    }
    if (chosen == TA_NIL) {
        tool_text_append(&text, inputs.paths[0]);
        if (inputs.segment == TA_NIL) {
            tool_text_append(&text, ta_string(" has no live segment"));
        } else {
            tool_text_append(&text, ta_string(" has no live segment "));
            tool_text_number(&text, inputs.segment);
        }
        return refuse(out, &text, TOOL_EXIT_USAGE);
    }
    out->bytes = ta_host_snapshot_image(pack, chosen);
    out->segment = chosen;
    return TA_OK;
}

ta_status tool_image_make(tool *t, const tool_image_request *request, tool_image *out) {
    tool_inputs inputs = request->inputs;
    ta_growable_arena *arena = request->arena;
    ta_memset(out, 0, sizeof *out);
    ta_source *sources = (ta_source *)ta_growable_arena_alloc(arena, sizeof *sources * inputs.count, 8);
    if (!sources) {
        out->exit = TOOL_EXIT_FAILURE;
        return out->status = TA_ERROR_NO_MEMORY;
    }
    uint32_t kind = TA_NIL;
    for (uint32_t i = 0; i < inputs.count; i++) {
        ta_bytes bytes;
        tool_read read = {inputs.paths[i], arena};
        tool_text text;
        tool_text_over(&text, arena);
        if (t->platform->read(t->platform->context, &read, &bytes) != TA_OK) {
            tool_text_append(&text, ta_string("cannot read "));
            tool_text_append(&text, inputs.paths[i]);
            return refuse(out, &text, TOOL_EXIT_FAILURE);
        }
        uint32_t this_kind = tool_input_kind_of(bytes);
        if (this_kind == TOOL_INPUT_BLOB || this_kind == TOOL_INPUT_HOST_SNAPSHOT) {
            tool_text_append(&text, inputs.paths[i]);
            tool_text_append(&text, this_kind == TOOL_INPUT_BLOB
                                        ? ta_string(" is a hibernation or export blob: inspect reads it")
                                        : ta_string(" is a host snapshot without its images: inspect "
                                                    "reads it, and the other commands a pack "
                                                    "(ta_host_snapshot_pack)"));
            return refuse(out, &text, TOOL_EXIT_USAGE);
        }
        if (this_kind == TA_NIL || (kind != TA_NIL && (this_kind != kind || kind != TOOL_INPUT_CHART))) {
            tool_text_append(&text, inputs.paths[i]);
            tool_text_append(&text, this_kind == TA_NIL
                                        ? ta_string(" is neither an image (.tai), a snapshot, an SCXML "
                                                    "chart nor a system manifest")
                                        : ta_string(": give one image, one snapshot, one manifest, or "
                                                    "charts"));
            return refuse(out, &text, this_kind == TA_NIL ? TOOL_EXIT_REFUSED : TOOL_EXIT_USAGE);
        }
        kind = this_kind;
        sources[i].name = inputs.paths[i];
        sources[i].text = (ta_string_view){(const char *)bytes.pointer, (uint32_t)bytes.length};
        if (kind == TOOL_INPUT_IMAGE || kind == TOOL_INPUT_SNAPSHOT) out->bytes = bytes;
    }
    out->kind = kind;
    if (kind == TOOL_INPUT_SNAPSHOT && snapshot_image(request, out) != TA_OK) return out->status;
    if (kind != TOOL_INPUT_IMAGE && kind != TOOL_INPUT_SNAPSHOT) {
        tool_compilation compilation = {kind, inputs.flags, {sources, inputs.count}, arena, 0};
        ta_status kept = tool_compile_sources(t, &compilation, out);
        if (kept != TA_OK) {
            out->exit = TOOL_EXIT_FAILURE;
            return out->status = kept;
        }
        if (out->status != TA_OK) {
            out->exit = TOOL_EXIT_REFUSED;
            return out->status;
        }
    }
    out->status = ta_image_open(out->bytes, &out->image);
    if (out->status != TA_OK) {
        own_diagnostic(arena, out, (ta_diagnostic){TA_DIAGNOSTIC_ERROR, 0, 0, 0, inputs.paths[0],
                                                   ta_string("the image does not verify: it is damaged, "
                                                             "or of another format version")});
        out->exit = TOOL_EXIT_REFUSED;
        return out->status;
    }
    out->opened = 1;
    return TA_OK;
}

ta_status tool_load(tool *t, tool_inputs inputs, tool_image *out) {
    ta_status status = tool_image_make(t, &(tool_image_request){inputs, t->arena}, out);
    if (status == TA_OK) return TA_OK;
    if (out->complaint.length) tool_complain(t, out->complaint);
    t->exit = (int)out->exit;
    return status;
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
                          tool_count(t, TOOL_OPTION_WARNINGS_AS_ERRORS) ? TA_COMPILE_WERROR : 0u,
                          TA_NIL};
    const ta_cli_value *segment = tool_last(t, TOOL_OPTION_SEGMENT);
    uint64_t number = 0;
    if (segment && (tool_decimal(segment->text, &number) != TA_OK || number >= TA_NIL)) {
        tool_complain(t, ta_string("--segment takes a segment's number: 0, 1, ..."));
        t->exit = TOOL_EXIT_USAGE;
        return TA_ERROR_INVALID;
    }
    if (segment) inputs.segment = (uint32_t)number;
    ta_status status = tool_load(t, inputs, &out->image);
    if (status != TA_OK) {
        if (t->exit == TOOL_EXIT_REFUSED) tool_report_diagnostics(t, &out->image);
        return status;
    }
    return tool_select_charts(t, (tool_selection){&out->image, request.flags}, &out->charts);
}

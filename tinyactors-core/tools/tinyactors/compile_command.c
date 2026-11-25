/* compile_command.c — tinyactors compile and tinyactors check (tool.h).
 * Freestanding. */
#include "tool.h"

#include "base/memory.h"

/* The command's arguments as the inputs, with -W. */
static tool_inputs inputs_of(const tool *t) {
    tool_inputs inputs;
    inputs.count = t->parsed->argument_count;
    inputs.paths = t->parsed->arguments;
    inputs.flags = tool_count(t, TOOL_OPTION_WARNINGS_AS_ERRORS) ? TA_COMPILE_WERROR : 0;
    return inputs;
}

/* What an image holds, in brief: the members compile reports. */
static void report_summary(tool *t, const tool_image *image) {
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&image->image, &info);
    ta_report_number(&t->report, ta_string("bytes"), image->bytes.length);
    ta_report_text(&t->report, ta_string("hash"), tool_hexadecimal(t, info.content_hash));
    ta_report_number(&t->report, ta_string("charts"), info.chart_count);
    ta_report_number(&t->report, ta_string("bootActors"), info.boot_actor_count);
}

/* The image as C: an 8-aligned array of its bytes and its length. */
static ta_string_view as_c(tool *t, ta_bytes bytes, ta_string_view symbol) {
    static const char digits[] = "0123456789abcdef";
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string("/* A tinyactors image, written by tinyactors compile --emit c. */\n"
                                      "#include <stddef.h>\n\n_Alignas(8) const unsigned char "));
    tool_text_append(&text, symbol);
    tool_text_append(&text, ta_string("[] = {"));
    const unsigned char *byte = (const unsigned char *)bytes.pointer;
    for (size_t i = 0; i < bytes.length; i++) {
        char hexadecimal[6] = {' ', '0', 'x', digits[byte[i] >> 4], digits[byte[i] & 15u], ','};
        if (i % 12 == 0) tool_text_append(&text, ta_string("\n   "));
        tool_text_append(&text, (ta_string_view){hexadecimal, 6});
    }
    tool_text_append(&text, ta_string("\n};\nconst size_t "));
    tool_text_append(&text, symbol);
    tool_text_append(&text, ta_string("_length = "));
    tool_text_number(&text, bytes.length);
    tool_text_append(&text, ta_string(";\n"));
    return tool_text_view(&text);
}

/* A C identifier: a letter or '_', then letters, digits and '_'. */
static int is_identifier(ta_string_view name) {
    if (!name.length) return 0;
    for (uint32_t i = 0; i < name.length; i++) {
        char c = name.pointer[i];
        int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i && c >= '0' && c <= '9')) return 0;
    }
    return 1;
}

void tool_compile(tool *t) {
    const ta_cli_value *output = tool_last(t, TOOL_COMPILE_OUTPUT);
    const ta_cli_value *emit = tool_last(t, TOOL_COMPILE_EMIT);
    const ta_cli_value *symbol_value = tool_last(t, TOOL_COMPILE_SYMBOL);
    ta_string_view symbol = symbol_value ? symbol_value->text : ta_string("tinyactors_image");
    uint32_t kind = emit ? emit->choice : TOOL_EMIT_IMAGE;
    if (kind == TOOL_EMIT_C && !is_identifier(symbol)) {
        tool_complain(t, ta_string("--symbol takes a C identifier"));
        t->exit = TOOL_EXIT_USAGE;
        return;
    }
    if (output->text.length == 1 && output->text.pointer[0] == '-')
        t->report_stream = TOOL_STANDARD_ERROR; /* the image takes standard output */
    tool_image image;
    ta_status status = tool_load(t, inputs_of(t), &image);
    if (t->exit == TOOL_EXIT_USAGE || t->exit == TOOL_EXIT_FAILURE) return;
    if (status == TA_OK && image.kind == TOOL_INPUT_IMAGE) {
        tool_complain(t, ta_string("the input is an image already: compile takes charts or a manifest"));
        t->exit = TOOL_EXIT_USAGE;
        return;
    }
    ta_report_boolean(&t->report, ta_string("compiled"), status == TA_OK);
    if (status == TA_OK) {
        ta_report_text(&t->report, ta_string("output"), output->text);
        ta_report_text(&t->report, ta_string("emit"), kind == TOOL_EMIT_C ? ta_string("c") : ta_string("image"));
        report_summary(t, &image);
    }
    tool_report_diagnostics(t, &image);
    if (status != TA_OK) return;
    ta_bytes written = image.bytes;
    if (kind == TOOL_EMIT_C) {
        ta_string_view c = as_c(t, image.bytes, symbol);
        written = (ta_bytes){c.pointer, c.length};
    }
    tool_write request = {TOOL_STANDARD_OUTPUT, output->text, written};
    if (!written.pointer || t->platform->write(t->platform->context, &request) != TA_OK) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("cannot write "));
        tool_text_append(&text, output->text);
        tool_complain(t, tool_text_view(&text));
        t->exit = TOOL_EXIT_FAILURE;
    }
}

void tool_check(tool *t) {
    tool_image image;
    ta_status status = tool_load(t, inputs_of(t), &image);
    if (t->exit == TOOL_EXIT_USAGE || t->exit == TOOL_EXIT_FAILURE) return;
    ta_report_list(&t->report, (ta_report_list_names){ta_string("inputs"), ta_string("input")});
    for (uint32_t i = 0; i < t->parsed->argument_count; i++)
        ta_report_text(&t->report, ta_string(""), t->parsed->arguments[i]);
    ta_report_end(&t->report);
    ta_report_boolean(&t->report, ta_string("valid"), status == TA_OK);
    if (status == TA_OK) report_summary(t, &image);
    tool_report_diagnostics(t, &image);
}

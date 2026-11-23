/* ta-precompile-table: what a generator of precompiled code needs to know
 * about a chart, as JSON on standard output (a spike; tools/precompile).
 *
 *   ta-precompile-table chart.scxml
 *
 * For each chart of the compiled image: its name, its datamodel, the
 * fingerprint the layer checks at attach (ta_datamodel_precompiled_fingerprint),
 * its <data> declarations in order (the index init_data is given) and its
 * expressions in order (the index every other call is given), each with its
 * kind, line, column and text, as the compiler keeps them.
 */
#include <stdio.h>
#include <string.h>

#include <tinyactors/compile.h>
#include <tinyactors/datamodel_precompiled.h>
#include <tinyactors/image.h>
#include <tinyactors/runtime.h>

static void print_string(ta_string_view text) {
    putchar('"');
    for (uint32_t i = 0; i < text.length; i++) {
        unsigned char c = (unsigned char)text.pointer[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static const char *kind_name(uint32_t kind) {
    static const char *const names[] = {"none",   "condition", "value", "location",     "script",
                                        "string", "content",   "item",  "global_script"};
    return kind < sizeof names / sizeof names[0] ? names[kind] : "unknown";
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: ta-precompile-table chart.scxml\n");
        return 2;
    }
    ta_compile_result *compiled = NULL;
    ta_status status = ta_runtime_compile_file(argv[1], 0, &compiled);
    const ta_diagnostic *diagnostics;
    uint32_t count = compiled ? ta_compile_diagnostics(compiled, &diagnostics) : 0;
    for (uint32_t i = 0; i < count; i++)
        fprintf(stderr, "%.*s:%u:%u: %.*s\n", (int)diagnostics[i].file.length, diagnostics[i].file.pointer, diagnostics[i].line,
                diagnostics[i].column, (int)diagnostics[i].message.length, diagnostics[i].message.pointer);
    if (status != TA_OK) {
        fprintf(stderr, "ta-precompile-table: %s\n", ta_status_name(status));
        return 1;
    }
    ta_string_view bytes = ta_compile_image(compiled);
    ta_image image;
    if (ta_image_open((ta_bytes){bytes.pointer, bytes.length}, &image) != TA_OK) return 1;
    ta_image_info info = {sizeof info, 0, 0, 0, 0, 0, 0};
    ta_image_get_info(&image, &info);
    printf("{\"charts\": [");
    for (uint32_t chart = 0; chart < info.chart_count; chart++) {
        printf("%s\n  {\"name\": ", chart ? "," : "");
        print_string(ta_image_chart_name(&image, chart));
        printf(", \"datamodel\": ");
        print_string(ta_image_chart_datamodel(&image, chart));
        printf(", \"fingerprint\": \"%016llx\",\n   \"data\": [",
               (unsigned long long)ta_datamodel_precompiled_fingerprint(&image, chart));
        uint32_t data = ta_image_data_count(&image, chart);
        for (uint32_t i = 0; i < data; i++) {
            ta_data_info d;
            ta_image_data(&image, (ta_chart_item){chart, i}, &d);
            printf("%s{\"id\": ", i ? ", " : "");
            print_string(d.id);
            if (d.expression == TA_NIL) printf(", \"expression\": null}");
            else printf(", \"expression\": %u}", d.expression);
        }
        printf("],\n   \"expressions\": [");
        uint32_t expressions = ta_image_expression_count(&image, chart);
        for (uint32_t i = 0; i < expressions; i++) {
            ta_expression_info x;
            ta_image_expression(&image, (ta_chart_item){chart, i}, &x);
            printf("%s\n    {\"index\": %u, \"kind\": \"%s\", \"line\": %u, \"column\": %u, \"text\": ", i ? "," : "", i,
                   kind_name(x.kind), x.line, x.column);
            print_string(x.source);
            putchar('}');
        }
        printf("]}");
    }
    printf("\n]}\n");
    ta_compile_free(compiled);
    return 0;
}

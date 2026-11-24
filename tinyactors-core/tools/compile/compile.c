/* ta-compile: SCXML charts or a <system> manifest to an image (.tai), the
 * compiler's command line (include/tinyactors/compile.h).
 *
 *   ta-compile [options] chart.scxml... -o out.tai
 *   ta-compile [options] --system manifest.xml -o out.tai
 *
 * Diagnostics go to stderr as file:line:column: severity: message. The
 * first chart is the image's default boot chart; a static src is read
 * relative to the file that names it. Exit status: 0 compiled, 1 refused
 * (diagnostics printed), 2 a usage error or a file that cannot be read or
 * written. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/compile.h>
#include <tinyactors/image.h>
#include <tinyactors/runtime.h>

static const char usage[] =
    "usage: ta-compile [options] chart.scxml... -o out.tai\n"
    "       ta-compile [options] --system manifest.xml -o out.tai\n"
    "\n"
    "  -o, --output FILE      the image (- for stdout)\n"
    "      --system FILE      compile a <system> manifest, its charts and boot actors\n"
    "      --emit-c NAME      write the image as a C array named NAME instead of bytes\n"
    "      --dump             print the image's charts, states and boot actors\n"
    "  -W, --warnings-as-errors\n"
    "                         refuse a chart that has a warning\n"
    "      --version          print the library's version\n"
    "  -h, --help             print this\n";

typedef struct arguments {
    const char *output, *emit_c, *manifest;
    const char **charts;
    uint32_t chart_count;
    int dump, warnings_as_errors;
} arguments;

static int is(const char *argument, const char *name) {
    return strcmp(argument, name) == 0;
}

/* 0: parsed; 1: done (help, version); 2: a usage error, printed. */
static int parse(int count, char **values, arguments *out) {
    memset(out, 0, sizeof *out);
    out->charts = (const char **)calloc((size_t)count, sizeof *out->charts);
    if (!out->charts) return 2;
    for (int i = 1; i < count; i++) {
        const char *a = values[i];
        int takes = is(a, "-o") || is(a, "--output") || is(a, "--system") || is(a, "--emit-c");
        if (takes && i + 1 >= count) {
            fprintf(stderr, "ta-compile: %s needs a value\n%s", a, usage);
            return 2;
        }
        if (is(a, "-o") || is(a, "--output")) out->output = values[++i];
        else if (is(a, "--system")) out->manifest = values[++i];
        else if (is(a, "--emit-c")) out->emit_c = values[++i];
        else if (is(a, "--dump")) out->dump = 1;
        else if (is(a, "-W") || is(a, "--warnings-as-errors")) out->warnings_as_errors = 1;
        else if (is(a, "-h") || is(a, "--help")) {
            fputs(usage, stdout);
            return 1;
        } else if (is(a, "--version")) {
            ta_version version = ta_library_version();
            printf("ta-compile %u.%u.%u (ABI %u)\n", version.major, version.minor, version.patch,
                   version.abi);
            return 1;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "ta-compile: unknown option %s\n%s", a, usage);
            return 2;
        } else {
            out->charts[out->chart_count++] = a;
        }
    }
    if ((out->manifest != NULL) == (out->chart_count != 0)) {
        fprintf(stderr, "ta-compile: give charts or --system, one of the two\n%s", usage);
        return 2;
    }
    if (!out->output && !out->dump) {
        fprintf(stderr, "ta-compile: no output: give -o or --dump\n%s", usage);
        return 2;
    }
    return 0;
}

static const char *severity_name(uint32_t severity) {
    return severity == TA_DIAGNOSTIC_ERROR     ? "error"
           : severity == TA_DIAGNOSTIC_WARNING ? "warning"
                                               : "note";
}

static void print_diagnostics(const ta_compile_result *result) {
    const ta_diagnostic *diagnostic;
    uint32_t count = ta_compile_diagnostics(result, &diagnostic);
    for (uint32_t i = 0; i < count; i++) {
        const ta_diagnostic *d = &diagnostic[i];
        fprintf(stderr, "%.*s:%u:%u: %s: %.*s\n", (int)d->file.length,
                d->file.pointer ? d->file.pointer : "", d->line, d->column,
                severity_name(d->severity), (int)d->message.length, d->message.pointer);
    }
}

/* The sources, read whole: owned by `texts` until they are given back. */
typedef struct loaded {
    ta_source *sources;
    ta_bytes *texts;
    uint32_t count;
} loaded;

static void unload(loaded *files) {
    for (uint32_t i = 0; i < files->count; i++)
        ta_runtime_free_file(files->texts[i]);
    free(files->sources);
    free(files->texts);
}

static int load(const char *const *paths, uint32_t count, loaded *out) {
    out->count = 0;
    out->sources = (ta_source *)calloc(count, sizeof *out->sources);
    out->texts = (ta_bytes *)calloc(count, sizeof *out->texts);
    if (!out->sources || !out->texts) return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ta_runtime_load_file(paths[i], &out->texts[i]) != TA_OK) {
            fprintf(stderr, "ta-compile: cannot read %s\n", paths[i]);
            return 0;
        }
        out->count++;
        out->sources[i].name = (ta_string_view){paths[i], (uint32_t)strlen(paths[i])};
        out->sources[i].text =
            (ta_string_view){(const char *)out->texts[i].pointer, (uint32_t)out->texts[i].length};
    }
    return 1;
}

/* The image as a C array: `const unsigned char NAME[]` 8-aligned, and its
 * length as NAME_length. */
static void write_c(FILE *file, const char *name, ta_string_view image) {
    fprintf(file, "/* generated by ta-compile: a tinyactors image (.tai) */\n");
    fprintf(file, "#include <stddef.h>\n\n");
    fprintf(file, "_Alignas(8) const unsigned char %s[] = {", name);
    for (uint32_t i = 0; i < image.length; i++)
        fprintf(file, "%s0x%02x,", i % 12 ? " " : "\n    ", (unsigned char)image.pointer[i]);
    fprintf(file, "\n};\nconst size_t %s_length = %u;\n", name, image.length);
}

static int write_output(const arguments *arguments, ta_string_view image) {
    int to_stdout = is(arguments->output, "-");
    FILE *file = to_stdout ? stdout : fopen(arguments->output, arguments->emit_c ? "w" : "wb");
    if (!file) {
        fprintf(stderr, "ta-compile: cannot write %s\n", arguments->output);
        return 0;
    }
    if (arguments->emit_c) write_c(file, arguments->emit_c, image);
    else fwrite(image.pointer, 1, image.length, file);
    int ok = !ferror(file);
    if (!to_stdout && fclose(file) != 0) ok = 0;
    if (!ok) fprintf(stderr, "ta-compile: cannot write %s\n", arguments->output);
    return ok;
}

static void dump(ta_string_view bytes) {
    /* ta_image_open wants 8-aligned bytes: the compiler's need not be */
    void *copy = aligned_alloc(8, ((size_t)bytes.length + 7) & ~(size_t)7);
    if (!copy) return;
    memcpy(copy, bytes.pointer, bytes.length);
    ta_image image;
    if (ta_image_open((ta_bytes){copy, bytes.length}, &image) != TA_OK) {
        fprintf(stderr, "ta-compile: the image does not open\n");
        free(copy);
        return;
    }
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&image, &info);
    printf("image: %u bytes, format %u, %u charts, %u boot actors, %u event names, hash %016llx\n",
           bytes.length, info.version, info.chart_count, info.boot_actor_count,
           info.event_name_count, (unsigned long long)info.content_hash);
    for (uint32_t c = 0; c < info.chart_count; c++) {
        ta_string_view name = ta_image_chart_name(&image, c), source = ta_image_chart_source(&image, c),
                       datamodel = ta_image_chart_datamodel(&image, c);
        uint32_t states = ta_image_state_count(&image, c);
        printf("chart %u: %.*s (%.*s, datamodel %.*s): %u states, %u data, %u expressions\n", c,
               (int)name.length, name.pointer, (int)source.length, source.pointer,
               (int)datamodel.length, datamodel.pointer, states, ta_image_data_count(&image, c),
               ta_image_expression_count(&image, c));
        for (uint32_t s = 0; s < states; s++) {
            ta_string_view id = ta_image_state_name(&image, c, s);
            if (id.length) printf("  state %u: %.*s\n", s, (int)id.length, id.pointer);
            else printf("  state %u: (the root)\n", s);
        }
    }
    for (uint32_t i = 0; i < info.boot_actor_count; i++) {
        ta_boot_actor actor;
        if (ta_image_boot_actor(&image, i, &actor) != TA_OK) continue;
        ta_string_view chart = ta_image_chart_name(&image, actor.chart);
        printf("boot actor %u: %.*s of chart %.*s\n", i, (int)actor.name.length, actor.name.pointer,
               (int)chart.length, chart.pointer);
    }
    free(copy);
}

int main(int count, char **values) {
    arguments arguments;
    int parsed = parse(count, values, &arguments);
    if (parsed) {
        free(arguments.charts);
        return parsed == 1 ? 0 : 2;
    }
    loaded files = {0, 0, 0};
    int exit_status = 2;
    ta_runtime_files *resolved = ta_runtime_files_new();
    const char *const *paths = arguments.manifest ? &arguments.manifest : arguments.charts;
    if (resolved && load(paths, arguments.manifest ? 1 : arguments.chart_count, &files)) {
        ta_compile_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.flags = arguments.warnings_as_errors ? TA_COMPILE_WERROR : 0;
        options.alloc = ta_runtime_malloc();
        options.resolve = ta_runtime_resolve;
        options.resolve_context = resolved;
        ta_compile_result *result = NULL;
        ta_status status =
            arguments.manifest
                ? ta_compile_system(&options, &files.sources[0], &result)
                : ta_compile_charts(&options, (ta_source_list){files.sources, files.count}, &result);
        if (result) print_diagnostics(result);
        if (status == TA_OK) {
            ta_string_view image = ta_compile_image(result);
            exit_status = 0;
            if (arguments.dump) dump(image);
            if (arguments.output && !write_output(&arguments, image)) exit_status = 2;
        } else {
            if (!result || !ta_compile_diagnostics(result, &(const ta_diagnostic *){0}))
                fprintf(stderr, "ta-compile: refused: %s\n", ta_status_name(status));
            exit_status = 1;
        }
        ta_compile_free(result);
    }
    unload(&files);
    ta_runtime_files_free(resolved);
    free(arguments.charts);
    return exit_status;
}

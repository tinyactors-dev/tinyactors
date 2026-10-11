/* images.c — compiling charts and systems, opening images, image info and bytes (binding.h). */
#include "binding.h"

static void image_finalize(napi_env environment, void *data, void *hint) {
    (void)environment;
    (void)hint;
    nd_image *image = (nd_image *)data;
    free(image->bytes);
    free(image);
}

napi_value image_value(napi_env environment, const void *bytes, size_t length) {
    nd_image *image = (nd_image *)calloc(1, sizeof *image);
    napi_value r = NULL;
    if (!image || !(image->bytes = malloc(length + 8))) {
        free(image);
        napi_throw_error(environment, NULL, "out of memory");
        return NULL;
    }
    memcpy(image->bytes, bytes, length);
    image->length = length;
    if (ta_image_open(image->bytes, image->length, &image->image) != TA_OK) {
        image_finalize(environment, image, NULL);
        napi_throw_error(environment, NULL, "not a valid tinyactors image");
        return NULL;
    }
    napi_create_external(environment, image, image_finalize, NULL, &r);
    return r;
}

nd_image *get_image(napi_env environment, napi_value v) {
    void *p = NULL;
    if (type_of(environment, v) != napi_external ||
        napi_get_value_external(environment, v, &p) != napi_ok || !p) {
        napi_throw_type_error(environment, NULL, "expected an image");
        return NULL;
    }
    return (nd_image *)p;
}

ta_string_view source_name(ta_string_view source) {
    static const char *const prefix[] = {"file://", "file:", "./"};
    for (int k = 0; k < 3; k++) {
        uint32_t l = (uint32_t)strlen(prefix[k]);
        if (source.length > l && !memcmp(source.pointer, prefix[k], l)) {
            source.pointer += l;
            source.length -= l;
        }
    }
    return source;
}

static ta_status nd_resolve(void *context, ta_string_view base, ta_string_view source,
                            ta_source *out) {
    (void)base;
    const nd_files *files = (const nd_files *)context;
    source = source_name(source);
    ta_string_view base_name = source;
    for (uint32_t i = 0; i < source.length; i++)
        if (source.pointer[i] == '/')
            base_name = (ta_string_view){source.pointer + i + 1, source.length - i - 1};
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < files->count; i++) {
            ta_string_view want = pass ? base_name : source;
            if (files->source[i].name.length == want.length &&
                !memcmp(files->source[i].name.pointer, want.pointer, want.length)) {
                *out = files->source[i];
                return TA_OK;
            }
        }
    return TA_ERROR_INVALID;
}

/* {name, xml}[] -> sources (strings owned in `owned`); 0 on a type error (thrown) */
static int read_sources(napi_env environment, napi_value array, uint32_t n, ta_source *source,
                        char **owned) {
    for (uint32_t i = 0; i < n; i++) {
        napi_value o;
        size_t nl = 0, xl = 0;
        napi_get_element(environment, array, i, &o);
        owned[2 * i] = property_string(environment, o, "name", &nl);
        owned[2 * i + 1] = property_string(environment, o, "xml", &xl);
        if (!owned[2 * i] || !owned[2 * i + 1]) {
            napi_throw_type_error(environment, NULL, "compile: name and xml must be strings");
            return 0;
        }
        source[i].name = view_of(owned[2 * i], nl);
        source[i].text = view_of(owned[2 * i + 1], xl);
    }
    return 1;
}

static void throw_diagnostics(napi_env environment, const char *what, ta_compile_result *compiled) {
    char message[4096];
    snprintf(message, sizeof message, "%s", what);
    const ta_diagnostic *diagnostic;
    uint32_t nd = ta_compile_diagnostics(compiled, &diagnostic);
    for (uint32_t i = 0; i < nd && strlen(message) < sizeof message - 200; i++)
        snprintf(message + strlen(message), sizeof message - strlen(message),
                 "\n  %.*s:%u:%u: %.*s", (int)diagnostic[i].file.length,
                 diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "", diagnostic[i].line,
                 diagnostic[i].column, (int)diagnostic[i].message.length,
                 diagnostic[i].message.pointer);
    napi_throw_error(environment, NULL, message);
}

/* compile(sources: {name, xml}[], files?: {name, xml}[]) -> image: charts, plus
 * files their src="..." attributes may name. manifest: compileSystem. */
static napi_value compile_any(napi_env environment, napi_callback_info info, int system) {
    size_t argc = 2;
    napi_value argv[2], result = NULL;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    uint32_t n = system ? 1
                 : argc ? array_length(environment, argv[0])
                        : 0,
             nf = argc > 1 ? array_length(environment, argv[1]) : 0;
    if (!argc || (!system && type_of(environment, argv[0]) != napi_object) ||
        (system && type_of(environment, argv[0]) != napi_string)) {
        napi_throw_type_error(environment, NULL,
                              system ? "compileSystem(manifest: string, files?: {name, xml}[])"
                                     : "compile(sources: {name, xml}[], files?: {name, xml}[])");
        return NULL;
    }
    ta_source *source = (ta_source *)calloc(n + nf + 1, sizeof *source);
    char **owned = (char **)calloc(2 * (n + nf + 1), sizeof(char *));
    if (system) {
        size_t ml = 0;
        owned[1] = get_string(environment, argv[0], &ml);
        source[0].name = view_of("system.xml", 10);
        source[0].text = view_of(owned[1], ml);
    } else if (!read_sources(environment, argv[0], n, source, owned))
        goto out;
    if (nf && !read_sources(environment, argv[1], nf, source + n, owned + 2 * n)) goto out;
    {
        nd_files files = {source + n, nf};
        ta_compile_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.resolve = nd_resolve;
        options.resolve_context = &files;
        ta_compile_result *compiled = NULL;
        ta_status status = system ? ta_compile_system(&options, source, &compiled)
                                  : ta_compile_charts(&options, source, n, &compiled);
        if (status != TA_OK) {
            throw_diagnostics(
                environment,
                system ? "system compilation failed:" : "SCXML compilation failed:", compiled);
        } else {
            ta_string_view bytes = ta_compile_image(compiled);
            result = image_value(environment, bytes.pointer, bytes.length);
        }
        ta_compile_free(compiled);
    }
out:
    for (uint32_t i = 0; i < 2 * (n + nf + 1); i++)
        free(owned[i]);
    free(owned);
    free(source);
    return result;
}

napi_value js_compile(napi_env environment, napi_callback_info info) {
    return compile_any(environment, info, 0);
}

napi_value js_compile_system(napi_env environment, napi_callback_info info) {
    return compile_any(environment, info, 1);
}

/* writeXML(ops: Uint8Array, strings: string[]) -> string: XML from an element
 * tree, through ta_xml_writer. ops: 1 open (takes a name from `strings`), 2
 * attribute (a name and a value), 3 text (one), 4 close. */
napi_value js_write_xml(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], result = NULL;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *data = NULL;
    size_t count = 0;
    if (argc < 2 || !bytes_of(environment, argv[0], &data, &count) ||
        type_of(environment, argv[1]) != napi_object) {
        napi_throw_type_error(environment, NULL, "writeXML(ops: Uint8Array, strings: string[])");
        return NULL;
    }
    const uint8_t *ops = (const uint8_t *)data;
    uint32_t string_count = array_length(environment, argv[1]), next = 0;
    ta_xml_writer *writer = NULL;
    if (ta_xml_writer_new(NULL, &writer) != TA_OK) {
        napi_throw_error(environment, NULL, "writeXML: out of memory");
        return NULL;
    }
    ta_status status = TA_OK;
    for (size_t i = 0; i < count && status == TA_OK; i++) {
        uint32_t needs = ops[i] == 1 || ops[i] == 3 ? 1 : ops[i] == 2 ? 2 : 0;
        char *texts[2] = {NULL, NULL};
        ta_string_view view[2] = {{NULL, 0}, {NULL, 0}};
        if (ops[i] < 1 || ops[i] > 4 || next + needs > string_count) status = TA_ERROR_INVALID;
        for (uint32_t k = 0; k < needs && status == TA_OK; k++) {
            napi_value element;
            size_t length = 0;
            napi_get_element(environment, argv[1], next++, &element);
            texts[k] = get_string(environment, element, &length);
            if (!texts[k]) status = TA_ERROR_INVALID;
            view[k] = view_of(texts[k], length);
        }
        if (status == TA_OK) {
            if (ops[i] == 1) status = ta_xml_writer_open(writer, view[0]);
            if (ops[i] == 2) status = ta_xml_writer_attribute(writer, view[0], view[1]);
            if (ops[i] == 3) status = ta_xml_writer_text(writer, view[0]);
            if (ops[i] == 4) status = ta_xml_writer_close(writer);
        }
        free(texts[0]);
        free(texts[1]);
    }
    ta_string_view xml = {NULL, 0};
    if (status == TA_OK) status = ta_xml_writer_finish(writer, &xml);
    if (status == TA_OK)
        result = str_value(environment, xml);
    else
        napi_throw_error(environment, NULL,
                         status == TA_ERROR_NO_MEMORY
                             ? "writeXML: out of memory"
                             : "writeXML: not a well-formed element tree (an element or "
                               "attribute name that is not an XML name, a control character, "
                               "or an attribute after content)");
    ta_xml_writer_free(writer);
    return result;
}

/* openImage(bytes) -> image (copied) */
napi_value js_open_image(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *data = NULL;
    size_t n = 0;
    if (!argc || !bytes_of(environment, argv[0], &data, &n)) {
        napi_throw_type_error(environment, NULL, "openImage(bytes: Uint8Array)");
        return NULL;
    }
    return image_value(environment, data, n);
}

napi_value js_image_bytes(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_image *image = argc ? get_image(environment, argv[0]) : NULL;
    return image ? bytes_value(environment, image->bytes, image->length) : NULL;
}

static napi_value hash_value(napi_env environment, uint64_t hash) {
    char text[17];
    snprintf(text, sizeof text, "%016llx", (unsigned long long)hash);
    return str_value(environment, view_of(text, 16));
}

/* imageInfo(image) -> { hash, charts: [{ name, source, datamodel, states: [{ id,
 * parent, kind }] }], actors: [{ name, chart }] } */
napi_value js_image_info(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o, charts, actors;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_image *image = argc ? get_image(environment, argv[0]) : NULL;
    if (!image) return NULL;
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(&image->image, &image_info);
    napi_create_object(environment, &o);
    set(environment, o, "hash", hash_value(environment, image_info.content_hash));
    napi_create_array_with_length(environment, image_info.chart_count, &charts);
    for (uint32_t c = 0; c < image_info.chart_count; c++) {
        napi_value x, states;
        napi_create_object(environment, &x);
        set(environment, x, "name", str_value(environment, ta_image_chart_name(&image->image, c)));
        set(environment, x, "source",
            str_value(environment, ta_image_chart_source(&image->image, c)));
        set(environment, x, "datamodel",
            str_value(environment, ta_image_chart_datamodel(&image->image, c)));
        uint32_t n = ta_image_state_count(&image->image, c);
        napi_create_array_with_length(environment, n, &states);
        for (uint32_t s = 0; s < n; s++) {
            ta_state_info state;
            state.size = sizeof state;
            if (ta_image_state_info(&image->image, c, s, &state) != TA_OK) break;
            napi_value y;
            napi_create_object(environment, &y);
            set(environment, y, "id", str_value(environment, state.id));
            set(environment, y, "parent",
                num(environment, state.parent == TA_NIL ? -1.0 : (double)state.parent));
            set(environment, y, "kind", num(environment, state.kind));
            napi_set_element(environment, states, s, y);
        }
        set(environment, x, "states", states);
        napi_set_element(environment, charts, c, x);
    }
    set(environment, o, "charts", charts);
    napi_create_array_with_length(environment, image_info.boot_actor_count, &actors);
    for (uint32_t i = 0; i < image_info.boot_actor_count; i++) {
        ta_string_view name = {NULL, 0};
        uint32_t chart = 0;
        napi_value x;
        napi_create_object(environment, &x);
        if (ta_image_boot_actor(&image->image, i, &name, &chart) == TA_OK) {
            set(environment, x, "name", str_value(environment, name));
            set(environment, x, "chart", num(environment, chart));
        }
        napi_set_element(environment, actors, i, x);
    }
    set(environment, o, "actors", actors);
    return o;
}

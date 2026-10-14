/* ta-conform's datamodels (driver.md, the `datamodel` and `realm` params):
 * "quickjs" is ta_datamodel_ecmascript in the realm asked for, the default;
 * "test" is core's tests' datamodel (tests/support/datamodel_test.h), which
 * core's W3C check runs on too. ta-conform has no "native" datamodel of its
 * own: a call naming it, or any other, answers -32602 range. */
#include "conform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/runtime.h>

static int datamodel_make(conform_datamodel *datamodel) {
    if (datamodel->test) {
        datamodel->binding = datamodel_test_binding(datamodel->test);
        return 1;
    }
    ta_datamodel_ecmascript_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.realm = datamodel->realm;
    datamodel->ecmascript = ta_datamodel_ecmascript_new(&config);
    if (!datamodel->ecmascript) return 0;
    datamodel->binding = ta_datamodel_ecmascript_binding(datamodel->ecmascript);
    return 1;
}

static int view_is(ta_string_view view, const char *text) {
    return view.length == strlen(text) && !memcmp(view.pointer, text, view.length);
}

int conform_datamodel_open(conform_datamodel *datamodel, conform_call *call) {
    memset(datamodel, 0, sizeof *datamodel);
    ta_string_view name = ta_string("quickjs"), realm = ta_string("auto");
    if (!conform_text(call, "datamodel", &name) || !conform_text(call, "realm", &realm)) return 0;
    if (!view_is(name, "quickjs") && !view_is(name, "test")) {
        conform_fail(call, &(conform_failure){CONFORM_INVALID_PARAMS, "range", "params.datamodel"},
                     "ta-conform offers the datamodels quickjs and test, no native one");
        return 0;
    }
    if (view_is(name, "quickjs") && !view_is(realm, "auto") && !view_is(realm, "isolated")) {
        conform_fail(call, &(conform_failure){CONFORM_INVALID_PARAMS, "range", "params.realm"},
                     "a realm is auto or isolated");
        return 0;
    }
    datamodel->realm = view_is(realm, "isolated") ? TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED
                                                  : TA_DATAMODEL_ECMASCRIPT_REALM_AUTO;
    if (view_is(name, "test") && !(datamodel->test = datamodel_test_new())) return 0;
    return datamodel_make(datamodel);
}

int conform_datamodel_reopen(conform_datamodel *datamodel) {
    int test = datamodel->test != NULL;
    uint32_t realm = datamodel->realm;
    conform_datamodel_close(datamodel);
    datamodel->realm = realm;
    if (test && !(datamodel->test = datamodel_test_new())) return 0;
    return datamodel_make(datamodel);
}

void conform_datamodel_close(conform_datamodel *datamodel) {
    if (datamodel->ecmascript) ta_datamodel_ecmascript_free(datamodel->ecmascript);
    if (datamodel->test) datamodel_test_free(datamodel->test);
    memset(datamodel, 0, sizeof *datamodel);
}

ta_status conform_datamodel_read(conform_datamodel *datamodel, ta_bytes cbor, ta_payload *out) {
    *out = 0;
    const ta_datamodel_vtable *vtable = datamodel->binding.vtable;
    if (!vtable || !vtable->from_wire) return TA_ERROR_UNSUPPORTED;
    return vtable->from_wire(datamodel->binding.datamodel, cbor, out);
}

ta_status conform_datamodel_write(conform_datamodel *datamodel, ta_payload payload, ta_bytes *out) {
    const ta_datamodel_binding *binding = &datamodel->binding;
    if (!binding->vtable || !binding->vtable->to_wire) return TA_ERROR_UNSUPPORTED;
    return binding->vtable->to_wire(binding->datamodel, payload, out);
}

void conform_datamodel_release(conform_datamodel *datamodel, ta_payload payload) {
    if (payload && datamodel->binding.vtable) datamodel->binding.vtable->release(datamodel->binding.datamodel, payload);
}

static ta_status snapshot_write(void *context, ta_buffer *out) {
    return ta_datamodel_ecmascript_snapshot((ta_datamodel_ecmascript *)context, out);
}

ta_status conform_datamodel_snapshot(conform_datamodel *datamodel, ta_buffer *out) {
    if (!datamodel->ecmascript) return TA_ERROR_UNSUPPORTED;
    return conform_write_growing(&(conform_writing){snapshot_write, datamodel->ecmascript}, out);
}

ta_status conform_datamodel_restore(conform_datamodel *datamodel, ta_bytes state) {
    if (!datamodel->ecmascript) return TA_ERROR_UNSUPPORTED;
    return ta_datamodel_ecmascript_restore(datamodel->ecmascript, state);
}

ta_host_environment conform_datamodel_environment(const conform_datamodel *datamodel, ta_buffer *error) {
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodels = &datamodel->binding;
    environment.datamodel_count = 1;
    environment.allocator = ta_runtime_malloc();
    environment.error = error;
    return environment;
}

/* ---- images ------------------------------------------------------------------------ */

/* A src="" of a chart: the document of that name (ta_resolve_function). */
static ta_status document_resolve(void *context, const ta_resolve_request *request, ta_resolve_result *out) {
    const ta_source_list *files = (const ta_source_list *)context;
    ta_string_view name = request->source;
    static const char *const prefixes[] = {"file://", "file:", "./"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t length = strlen(prefixes[i]);
        if (name.length > length && !memcmp(name.pointer, prefixes[i], length)) {
            name.pointer += length;
            name.length -= (uint32_t)length;
        }
    }
    for (uint32_t i = 0; i < files->count; i++)
        if (files->pointer[i].name.length == name.length && !memcmp(files->pointer[i].name.pointer, name.pointer, name.length)) {
            out->source = files->pointer[i];
            return TA_OK;
        }
    return TA_ERROR_INVALID;
}

ta_status conform_image_compile(const conform_documents *documents, conform_image *out) {
    memset(out, 0, sizeof *out);
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.resolve = document_resolve;
    options.resolve_context = (void *)&documents->files;
    ta_compile_result *result = NULL;
    ta_status status = ta_compile_charts(&options, documents->charts, &result);
    if (status != TA_OK) {
        const ta_diagnostic *diagnostics;
        uint32_t count = result ? ta_compile_diagnostics(result, &diagnostics) : 0;
        for (uint32_t i = 0; i < count; i++)
            fprintf(stderr, "ta-conform: %.*s:%u: %.*s\n", (int)diagnostics[i].file.length,
                    diagnostics[i].file.pointer ? diagnostics[i].file.pointer : "", diagnostics[i].line,
                    (int)diagnostics[i].message.length, diagnostics[i].message.pointer);
        ta_compile_free(result);
        return status;
    }
    ta_string_view bytes = ta_compile_image(result);
    out->result = result;
    out->bytes = (ta_bytes){bytes.pointer, bytes.length};
    return TA_OK;
}

void conform_image_free(conform_image *image) {
    ta_compile_free(image->result);
    memset(image, 0, sizeof *image);
}

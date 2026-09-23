#include "codec.h"
#include "system.h"

/*
 * Import: decode, compile, register. The decode's arena holds the spec,
 * which may point into the input; compile copies what it keeps, so neither
 * outlives this call as far as the definition is concerned.
 */

static void ta_import_fail(const ta_definition_import_params *params, const ta_import_error *error) {
    if (params->error != NULL) {
        *params->error = *error;
    }
}

ta_result ta_definition_import(ta_system *system, const ta_definition_import_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL || (params->input.ptr == NULL && params->input.len != 0)) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_import_error){0};
    }
    if (params->codec.v->decode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }

    const ta_scxml_spec *decoded = NULL;
    ta_chart_error decode_error = {0};
    ta_result result = params->codec.v->decode(params->codec.self,
                                               &(ta_chart_decode_params){
                                                   .input = params->input,
                                                   .arena = params->arena,
                                                   .resolver = params->resolver,
                                                   .lowering = params->lowering,
                                                   .identity = params->identity,
                                                   .out = &decoded,
                                                   .error = &decode_error,
                                               });
    if (result == TA_ERR_INVALID) {
        ta_import_fail(params, &(ta_import_error){ .stage = TA_IMPORT_STAGE_DECODE,
                                                   .as.decode = decode_error,
                                                   .located = true,
                                                   .at = decode_error.at });
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    if (decoded == NULL) {
        return TA_ERR_INVALID; /* a codec that reports success must produce a spec */
    }

    /* No chart syntax carries an identity, so the caller's is the chart's,
     * whether or not the codec copied it across. */
    ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(ta_scxml_spec), _Alignof(ta_scxml_spec));
    if (spec == NULL) {
        return TA_ERR_NOMEM;
    }
    *spec = *decoded;
    spec->identity = params->identity;

    ta_definition *definition = NULL;
    result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                    .spec = spec,
                                                    .datamodel = params->datamodel,
                                                    .allocator = params->allocator,
                                                    .out = &definition,
                                                });
    if (result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) {
        /* Located once ADR 0058 reports where in the spec the fault is. */
        ta_import_fail(params, &(ta_import_error){ .stage = TA_IMPORT_STAGE_COMPILE,
                                                   .as.compile = { .code = TA_COMPILE_ERROR_NONE } });
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    result = ta_system_register_definition(system, definition, params->out);
    if (result != TA_OK) {
        /* Registration takes ownership only on success. */
        ta_definition_destroy(definition);
    }
    return result;
}

/*
 * Encode: export, then the codec's encode. The identity no syntax carries is
 * handed back beside the bytes.
 */
ta_result ta_definition_encode(const ta_system *system, const ta_definition_encode_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_chart_error){0};
    }
    if (params->codec.v->encode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    const ta_scxml_spec *spec = NULL;
    ta_result result = ta_definition_export(system, &(ta_definition_export_params){
                                                        .definition = params->definition,
                                                        .arena = params->arena,
                                                        .out = &spec,
                                                    });
    if (result != TA_OK) {
        return result;
    }
    ta_chart_error encode_error = {0};
    result = params->codec.v->encode(params->codec.self, &(ta_chart_encode_params){
                                                             .spec = spec,
                                                             .arena = params->arena,
                                                             .expressions = params->expressions,
                                                             .out = params->out,
                                                             .error = &encode_error,
                                                         });
    if (result == TA_ERR_INVALID && params->error != NULL) {
        *params->error = encode_error;
    }
    if (result == TA_OK && params->identity != NULL) {
        *params->identity = spec->identity;
    }
    return result;
}

#include "import.h"

#include <stdio.h>
#include <string.h>

#include "allocator.h"

/* Block size of the import's arena; a large document grows it on demand. */
#define TNA_IMPORT_ARENA_BLOCK ((size_t)16384)

void tna_import_failure_open(tna_import_failure *failure, char *buffer, size_t size) {
    *failure = (tna_import_failure){ .message = buffer, .message_size = size };
    if (size != 0) {
        buffer[0] = '\0';
    }
}

void tna_import_failure_reset(tna_import_failure *failure, ta_import_stage stage, bool located) {
    tna_import_failure_open(failure, failure->message, failure->message_size);
    failure->stage = stage;
    failure->located = located;
}

static void tna_import_text(tna_import_failure *failure, ta_str text) {
    size_t length = text.len < failure->message_size - 1u ? text.len : failure->message_size - 1u;
    if (length != 0) {
        memcpy(failure->message, text.ptr, length);
    }
    failure->message[length] = '\0';
}

static void tna_import_copy(char *into, size_t size, ta_str text) {
    size_t length = text.ptr == NULL ? 0 : (text.len < size - 1u ? text.len : size - 1u);
    if (length != 0) {
        memcpy(into, text.ptr, length);
    }
    into[length] = '\0';
}

/* A decode error: the codec's phrase, and the element and attribute it is about. */
static void tna_import_decode(tna_import_failure *failure, const ta_chart_error *error) {
    failure->code = error->code;
    tna_import_copy(failure->construct, sizeof failure->construct, error->construct);
    tna_import_copy(failure->attribute, sizeof failure->attribute, error->attribute);
    snprintf(failure->message, failure->message_size, "%.*s%s%.*s%s%.*s", (int)error->message.len,
             error->message.ptr != NULL ? error->message.ptr : "", error->construct.len != 0 ? " in <" : "",
             (int)error->construct.len, error->construct.ptr != NULL ? error->construct.ptr : "",
             error->construct.len != 0 ? (error->attribute.len != 0 ? "> attribute " : ">") : "",
             (int)error->attribute.len, error->attribute.ptr != NULL ? error->attribute.ptr : "");
}

static void tna_import_describe(tna_import_failure *failure, const ta_import_error *error, ta_arena *arena,
                                const ta_system *core) {
    tna_import_failure_reset(failure, error->stage, error->located);
    failure->line = error->at.line;
    failure->column = error->at.column;
    failure->offset = error->at.offset;
    /* A codec that gave no place for its refusal leaves the position zero. */
    if (error->stage == TA_IMPORT_STAGE_DECODE && error->at.line == 0) {
        failure->located = false;
    }
    if (error->stage == TA_IMPORT_STAGE_DECODE) {
        tna_import_decode(failure, &error->as.decode);
        return;
    }
    ta_str text = { 0 };
    if (error->stage == TA_IMPORT_STAGE_COMPILE &&
        ta_compile_error_format(&(ta_compile_error_format_params){
            .error = &error->as.compile, .arena = arena, .out = &text, .system = core }) == TA_OK) {
        tna_import_text(failure, text);
        return;
    }
    tna_import_text(failure, (ta_str){ "the document was refused", 24 });
}

ta_result tna_scxml_codec(ta_scxml_codec *codec, ta_chart_codec *out) {
    ta_result result = ta_scxml_codec_init(codec, &(ta_scxml_codec_params){ 0 });
    if (result == TA_OK) {
        *out = ta_scxml_codec_adapter(codec);
    }
    return result;
}

ta_result tna_import_scxml(tna_system *system, const tna_import_params *params) {
    ta_scxml_codec codec;
    tna_import_params scxml = *params;
    ta_result result = tna_scxml_codec(&codec, &scxml.codec);
    return result == TA_OK ? tna_import(system, &scxml) : result;
}

ta_result tna_import(tna_system *system, const tna_import_params *params) {
    ta_allocator allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE);
    ta_arena arena;
    ta_result result = ta_arena_init(&arena, allocator, TNA_IMPORT_ARENA_BLOCK);
    if (result != TA_OK) {
        return result;
    }
    ta_import_error error = { 0 };
    result = ta_definition_import(system->core, &(ta_definition_import_params){
                                                    .codec = params->codec,
                                                    .input = params->input,
                                                    .name = params->name,
                                                    .arena = &arena,
                                                    .resolver = params->resolver,
                                                    .identity = params->identity,
                                                    .datamodels = params->datamodels,
                                                    .datamodel_count = params->datamodel_count,
                                                    .allocator = allocator,
                                                    .out = params->out,
                                                    .error = &error,
                                                });
    if ((result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) && params->failure != NULL) {
        tna_import_describe(params->failure, &error, &arena, system->core);
    }
    ta_arena_destroy(&arena);
    return result;
}

ta_result tna_encode_arena(tna_system *system, ta_arena *arena) {
    return ta_arena_init(arena, tah_allocator_for(&system->allocator, TAH_MEMORY_CORE), TNA_IMPORT_ARENA_BLOCK);
}

/*
 * A codec that writes a chart as another data model's (napi ADR 0031): it
 * hands the codec it wraps a copy of the spec that names `datamodel`.
 */
typedef struct {
    ta_chart_codec inner;
    ta_str datamodel;
} tna_datamodel_codec;

static ta_result tna_datamodel_encode(void *self, const ta_chart_encode_params *params) {
    const tna_datamodel_codec *codec = self;
    ta_scxml_spec spec = *params->spec;
    spec.datamodel = codec->datamodel;
    ta_chart_encode_params inner = *params;
    inner.spec = &spec;
    return codec->inner.v->encode(codec->inner.self, &inner);
}

ta_result tna_encode(tna_system *system, const tna_encode_params *params) {
    ta_chart_error error = { 0 };
    ta_chart_codec codec = params->codec;
    tna_datamodel_codec renamed = { .inner = params->codec, .datamodel = params->datamodel };
    ta_chart_codec_vtable renamed_vtable;
    if (params->datamodel.len != 0 && params->codec.v != NULL && params->codec.v->encode != NULL) {
        renamed_vtable = *params->codec.v;
        renamed_vtable.decode = NULL;
        renamed_vtable.encode = tna_datamodel_encode;
        codec = (ta_chart_codec){ .self = &renamed, .v = &renamed_vtable };
    }
    ta_result result = ta_definition_encode(system->core, &(ta_definition_encode_params){
                                                              .definition = params->definition,
                                                              .codec = codec,
                                                              .arena = params->arena,
                                                              .out = params->out,
                                                              .spans = params->spans,
                                                              .identity = params->identity,
                                                              .expressions = params->expressions,
                                                              .error = &error,
                                                          });
    if (result == TA_ERR_INVALID && params->failure != NULL) {
        tna_import_failure_reset(params->failure, TA_IMPORT_STAGE_DECODE, error.at.line != 0);
        params->failure->line = error.at.line;
        params->failure->column = error.at.column;
        params->failure->offset = error.at.offset;
        tna_import_decode(params->failure, &error);
    }
    return result;
}

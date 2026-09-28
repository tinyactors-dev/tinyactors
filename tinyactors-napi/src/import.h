#pragma once
/*
 * Importing charts with a codec (napi ADRs 0019 and 0027, core ADRs 0053 and
 * 0076), and writing a definition back with one (napi ADR 0026, core ADR
 * 0085).
 *
 * define(text, { format }) decodes the document with the codec `format`
 * names: the SCXML codec (tinyactors-scxml, over tinyxml), or one written in
 * JavaScript (codecs.h). It compiles the spec against the data model the
 * document names and registers the definition, in one core call
 * (ta_definition_import), so a refusal keeps its line and column (ADR 0058).
 *
 * This module has no Node-API in it: napi_system.c reads the bytes and the
 * choices, and turns a refusal into a TinyactorsError.
 */
#include <stdbool.h>
#include <stdint.h>

#include "system.h"
#include "ta_scxml.h"
#include "tinyactors.h"

/* Why a document was refused, formatted for a person. */
typedef struct {
    ta_import_stage stage; /* decode or compile */
    bool located;          /* `line`, `column` and `offset` say where */
    uint32_t line;
    uint32_t column;
    uint64_t offset;
    /* A decode's or an encode's refusal: the codec's code, and the construct
     * and attribute it is about ("" when none). TA_CHART_ERROR_NONE for a
     * compile error. */
    ta_chart_error_code code;
    char construct[128];
    char attribute[128];
    char message[1024];
} tna_import_failure;

/* The SCXML codec's adapter over `codec`, which the caller keeps. */
ta_result tna_scxml_codec(ta_scxml_codec *codec, ta_chart_codec *out);

typedef struct {
    /* The codec to decode with: tna_scxml_codec(), or tna_js_codec(). */
    ta_chart_codec codec;
    ta_bytes input;
    /* What the document is called, such as its path or URL: the source its
     * spans name (core ADR 0085). Empty for a document with no name. */
    ta_str name;
    /* The data models the document may choose from (core ADR 0076). */
    const ta_datamodel *datamodels;
    uint32_t datamodel_count;
    ta_definition_id *out;
    /* Filled when the result is TA_ERR_INVALID or TA_ERR_UNSUPPORTED. */
    tna_import_failure *failure;
    /* Fetches <data src> and <script src> (core ADR 0053); a null `v`
     * refuses every src, as TA_CHART_ERROR_SRC_UNRESOLVED. */
    ta_chart_src_resolver resolver;
    /* Optional: an identity with an empty revision names the family of a
     * chart with no name of its own, as for an unnamed child (core ADR 0077). */
    const ta_definition_identity *identity;
} tna_import_params;

/*
 * Decodes, compiles and registers a document. TA_ERR_INVALID or
 * TA_ERR_UNSUPPORTED with `failure` saying why, TA_ERR_NOMEM. A codec in
 * JavaScript that throws leaves the exception pending and answers
 * TA_ERR_INVALID.
 */
ta_result tna_import(tna_system *system, const tna_import_params *params);

/* Decodes, compiles and registers an SCXML document: tna_import() with the SCXML codec. */
ta_result tna_import_scxml(tna_system *system, const tna_import_params *params);

/*
 * Writes `definition` in a codec's syntax, with where each element landed
 * in it (core ADR 0085): Definition.encode(), and the debugger's virtual
 * source for a chart whose spans name no file it can open.
 */
typedef struct {
    ta_definition_id definition;
    ta_chart_codec codec;
    /* An arena the caller made with tna_encode_arena(): the text and the
     * map live in it. */
    ta_arena *arena;
    ta_bytes *out;
    ta_source_map *spans;
    /* The definition's explicit identity, or null when it was fingerprinted. */
    const ta_definition_identity **identity;
    /* Writes a host expression as source text, in the language of the data
     * model the chart is written for (napi ADR 0031). */
    ta_chart_expression_printer expressions;
    /* The data model the written chart names, in place of its own; empty
     * for its own (napi ADR 0031). */
    ta_str datamodel;
    /* Filled when the result is TA_ERR_INVALID. */
    tna_import_failure *failure;
} tna_encode_params;

/* An arena for tna_encode(), over the system's core allocator. */
ta_result tna_encode_arena(tna_system *system, ta_arena *arena);

/*
 * TA_ERR_NOT_FOUND for no such definition. TA_ERR_UNSUPPORTED when the codec
 * only decodes. TA_ERR_INVALID with `failure` saying why when the codec
 * cannot express the chart, such as SCXML and an expression that is a
 * JavaScript function (`unrepresentable`). TA_ERR_NOMEM.
 */
ta_result tna_encode(tna_system *system, const tna_encode_params *params);

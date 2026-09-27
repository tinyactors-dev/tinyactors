#pragma once
/*
 * Importing SCXML documents (napi ADR 0019, core ADRs 0053 and 0076).
 *
 * define(xml, { format: 'scxml' }) decodes the document with the SCXML codec
 * (tinyactors-scxml, over tinyxml), compiles it against the data model its
 * `datamodel` attribute names, and registers the definition, in one core call
 * (ta_definition_import), so a refusal keeps its line and column (ADR 0058).
 *
 * This module has no Node-API in it: napi_system.c reads the bytes and the
 * choices, and turns a refusal into a TinyactorsError.
 */
#include <stdbool.h>
#include <stdint.h>

#include "system.h"
#include "tinyactors.h"

/* Why a document was refused, formatted for a person. */
typedef struct {
    ta_import_stage stage; /* decode or compile */
    bool located;          /* `line`, `column` and `offset` say where */
    uint32_t line;
    uint32_t column;
    uint64_t offset;
    char message[1024];
} tna_import_failure;

typedef struct {
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
 * Decodes, compiles and registers an SCXML document. TA_ERR_INVALID or
 * TA_ERR_UNSUPPORTED with `failure` saying why, TA_ERR_NOMEM.
 */
ta_result tna_import_scxml(tna_system *system, const tna_import_params *params);

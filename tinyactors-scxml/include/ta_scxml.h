/*
 * ta_scxml: the SCXML 1.0 XML codec for tinyactors (core ADR 0053).
 *
 * A ta_chart_codec that reads and writes SCXML documents over txml, the
 * freestanding XML library in ../tinyxml. It performs no I/O and uses no C
 * library: <data src> and <script src> go through the caller's
 * ta_chart_src_resolver, platform-specific executable content through its
 * ta_chart_lowering, and every allocation through the operation's arena.
 *
 *   ta_scxml_codec scxml;
 *   ta_scxml_codec_init(&scxml, &(ta_scxml_codec_params){0});  // defaults
 *   ta_definition_import(system, &(ta_definition_import_params){
 *       .codec = ta_scxml_codec_adapter(&scxml), .input = bytes, .arena = &arena, ... });
 */
#pragma once

#include "tinyactors.h"

/* The codec's name: the registered media type of SCXML documents. It is also
 * the media type of every literal <content> it decodes. */
/* 0.x: a minor version changes the API or ABI, a patch does not. */
#define TA_SCXML_VERSION_MAJOR 0
#define TA_SCXML_VERSION_MINOR 1
#define TA_SCXML_VERSION_PATCH 0
#define TA_SCXML_VERSION ((TA_SCXML_VERSION_MAJOR << 16) | (TA_SCXML_VERSION_MINOR << 8) | TA_SCXML_VERSION_PATCH)

#define TA_SCXML_MEDIA_TYPE "application/scxml+xml"

/* The SCXML namespace. Elements in it are SCXML; any other element in
 * executable content is platform-specific content (SCXML 4.10). */
#define TA_SCXML_NAMESPACE "http://www.w3.org/2005/07/scxml"

/*
 * The bounds a decode enforces. Zero selects the default beside each field.
 * The defaults are safe for untrusted input: a document within them costs at
 * most a few tens of MiB and a bounded stack. The largest W3C IRP test
 * document is under 8 KiB and 12 elements deep.
 *
 * Reaching a bound is TA_CHART_ERROR_LIMIT. Its `construct` is the field's
 * name ("max_depth", ...) and its `at` is where the input reached it.
 *
 * Bounds that txml also has are passed to txml and enforced there only:
 * max_depth, max_attributes, max_name_length, max_names,
 * max_entity_expansion and max_parse_bytes.
 */
typedef struct {
    /* Bytes of input. Checked before parsing. */
    uint64_t max_document_bytes;
#define TA_SCXML_DEFAULT_MAX_DOCUMENT_BYTES ((uint64_t)(4u * 1024u * 1024u))
    /* Bytes the parsed document may occupy: txml's max_document_bytes, its
     * node and value storage. Markup-dense input needs several times its own
     * size. At most UINT32_MAX. */
    uint64_t max_parse_bytes;
#define TA_SCXML_DEFAULT_MAX_PARSE_BYTES ((uint64_t)(32u * 1024u * 1024u))
    /* Element nesting, inline machines inside <content> included. It also
     * bounds the codec's own recursion. At most 65535. */
    uint32_t max_depth;
#define TA_SCXML_DEFAULT_MAX_DEPTH ((uint32_t)64u)
    /* Attributes on one element, namespace declarations included. */
    uint32_t max_attributes;
#define TA_SCXML_DEFAULT_MAX_ATTRIBUTES ((uint32_t)32u)
    /* Bytes of one text value the codec produces: an attribute value, a
     * <script> body, the bytes of a literal <content>, <data> body or
     * fetched `src`. */
    uint32_t max_text_bytes;
#define TA_SCXML_DEFAULT_MAX_TEXT_BYTES ((uint32_t)(1024u * 1024u))
    /* Bytes produced by expanding entities the document's internal subset
     * declares (txml's entity_expansion_budget); predefined and character
     * references are not charged. */
    uint64_t max_entity_expansion;
#define TA_SCXML_DEFAULT_MAX_ENTITY_EXPANSION ((uint64_t)(64u * 1024u))
    /* Bytes of one qualified name. */
    uint32_t max_name_length;
#define TA_SCXML_DEFAULT_MAX_NAME_LENGTH ((uint32_t)256u)
    /* Distinct expanded names (element and attribute) in one document. */
    uint32_t max_names;
#define TA_SCXML_DEFAULT_MAX_NAMES ((uint32_t)1024u)
} ta_scxml_codec_params;

/* A codec. Initialize it once and keep it while its adapter is used. */
typedef struct {
    /* The bounds in force: the params given, with every zero replaced by its
     * default. */
    ta_scxml_codec_params limits;
} ta_scxml_codec;

/*
 * Resolves `params` (null for all defaults) into `codec`. TA_ERR_INVALID when
 * a bound exceeds what txml can represent: max_depth above 65535, or
 * max_parse_bytes above UINT32_MAX.
 */
TA_API ta_result ta_scxml_codec_init(TA_OUT ta_scxml_codec *codec, TA_IN TA_OPTIONAL const ta_scxml_codec_params *params);

/* The ta_chart_codec for `codec`, which must outlive it. */
TA_API ta_chart_codec ta_scxml_codec_adapter(TA_INOUT ta_scxml_codec *codec);

/* tinyactors/datamodel_precompiled.h — a datamodel whose expressions were
 * compiled ahead of time, in the application's language: a spike on a
 * branch, not a decided API.
 *
 * A chart says datamodel="swift" (or "kotlin", "go"): its expressions are
 * that language's code. A generator reads the chart's expression table
 * (ta_image_expression) and writes a class per chart, which the language's
 * own compiler compiles with the application: a chart's variables are the
 * class's properties, and each expression is a case of a switch on its
 * index. This layer implements the datamodel vtable around that code: it
 * keeps the payloads (canonical CBOR), builds `_event` and the captures, and
 * frames the blobs; it calls the language through one function, given what
 * to do and to which expression.
 *
 * Values cross as CBOR (docs/protocol.md, "Values"), which the language
 * reads and writes with its own codec (Codable, kotlinx-serialization,
 * fxamacker/cbor). A session's variables stay the language's own objects.
 */
#ifndef TINYACTORS_DATAMODEL_PRECOMPILED_H
#define TINYACTORS_DATAMODEL_PRECOMPILED_H

#include <tinyactors/allocator.h>
#include <tinyactors/datamodel.h>
#include <tinyactors/image.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_datamodel_precompiled ta_datamodel_precompiled;

/* What a request asks of the language (ta_precompiled_request.operation). */
enum {
    /* the chart named `input` (UTF-8), whose expression table hashes to
     * `session` (ta_datamodel_precompiled_fingerprint): answer its handle in
     * `number`; a fingerprint the generated code does not hold is
     * TA_ERROR_INVALID: the chart changed since its code was generated */
    TA_PRECOMPILED_ATTACH = 1,
    /* a new session of `chart`: `input` is CBOR [sessionid, name,
     * {type: location}, params | undefined]; answer its handle in `session` */
    TA_PRECOMPILED_CREATE = 2,
    TA_PRECOMPILED_DESTROY = 3,
    /* initialize the <data> declaration `number` */
    TA_PRECOMPILED_INIT_DATA = 4,
    /* `_event` is `input`: a CBOR map of its eight fields */
    TA_PRECOMPILED_BIND_EVENT = 5,
    /* answer the condition `expression` in `number` (1 true, 0 false) */
    TA_PRECOMPILED_CONDITION = 6,
    /* the value of `expression`, as CBOR in `output` */
    TA_PRECOMPILED_VALUE = 7,
    /* the value of `expression` as text (UTF-8) in `output`: a *expr
     * attribute, a <log>'s expr */
    TA_PRECOMPILED_STRING = 8,
    /* the location `expression` set to the value of the expression `number` */
    TA_PRECOMPILED_ASSIGN = 9,
    /* the location `expression` set to the value `input` (CBOR) */
    TA_PRECOMPILED_ASSIGN_VALUE = 10,
    /* run the <script> `expression` */
    TA_PRECOMPILED_SCRIPT = 11,
    /* the session's variables as one CBOR map in `output` */
    TA_PRECOMPILED_SERIALIZE = 12,
    /* a new session of `chart` with the variables `input` wrote; `session`
     * answered as for CREATE, whose `input` comes first */
    TA_PRECOMPILED_DESERIALIZE = 13,
    /* the variable named `input`, as CBOR in `output` */
    TA_PRECOMPILED_VARIABLE = 14
};

/* One call into the language. Every field but the answers is the layer's,
 * valid during the call: the language copies what it keeps. */
typedef struct ta_precompiled_request {
    uint32_t size;
    uint32_t operation;  /* TA_PRECOMPILED_* */
    uint32_t chart;      /* the language's handle, from ATTACH */
    uint32_t expression; /* the expression's index in the chart's table */
    uint64_t session;    /* the language's handle, from CREATE; ATTACH: the fingerprint */
    uint32_t number;     /* in: a declaration, a value's expression; out: a truth, a chart */
    uint32_t pad;
    ta_bytes input;
    ta_buffer output;  /* room the layer lends: the language writes and sets `length` */
    ta_buffer message; /* room for why the call failed, UTF-8 */
    const void *scope; /* the layer's, for ta_datamodel_precompiled_in during the call */
} ta_precompiled_request;

/* The language's one entry point: TA_OK, or TA_ERROR_EXECUTION with the
 * exception's text in request->message (the chart sees error.execution). An
 * answer larger than `output`'s room is TA_ERROR_NO_MEMORY. */
typedef ta_status (*ta_precompiled_function)(void *context, ta_precompiled_request *request);

typedef struct ta_datamodel_precompiled_config {
    uint32_t size;
    uint32_t max_value_bytes; /* the room for one answer; 0: 64 KiB */
    const char *name;         /* the charts' datamodel attribute: "swift", "kotlin", "go" */
    ta_precompiled_function function;
    void *context;
    ta_allocator allocator;
} ta_datamodel_precompiled_config;

TA_API ta_status ta_datamodel_precompiled_new(const ta_datamodel_precompiled_config *config,
                                              ta_datamodel_precompiled **out);
TA_API ta_datamodel_binding ta_datamodel_precompiled_binding(ta_datamodel_precompiled *datamodel);
TA_API void ta_datamodel_precompiled_free(ta_datamodel_precompiled *datamodel);

/* SCXML's In(state) for the session a request is about, during the request:
 * 1 when the state named `state` is active, 0 when it is not or there is
 * no such state. */
TA_API int ta_datamodel_precompiled_in(const ta_precompiled_request *request, ta_string_view state);

/* The hash of a chart's expression table (each expression's kind and text,
 * and each <data>'s id, in order: FNV-1a, 64 bits). A generator writes it
 * into the code it generates; ATTACH compares the two. */
TA_API uint64_t ta_datamodel_precompiled_fingerprint(const ta_image *image, uint32_t chart);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_DATAMODEL_PRECOMPILED_H */

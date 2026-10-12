/* conform.h — ta-conform, the C reference implementation of the
 * conformance suite's driver protocol (../tinyactors-test/docs/driver.md):
 * length-prefixed CBOR calls on standard input, replies on standard output.
 *
 *   conform.c    frames, calls, params, replies, and the method table
 *   datamodel.c  the datamodels a call names: quickjs (both realms) and
 *                core's tests' datamodel ("test")
 *   vectors.c    the methods the protocol vectors use (host.create, ...)
 *   scenario.c   scenario.run: the scenario environment
 *   w3c.c        w3c.run: the W3C environment, with a BasicHTTP loopback
 *
 * Only the public API (include/tinyactors) is called, but for the tests'
 * datamodel (tests/support/datamodel_test.h), which core's W3C check runs
 * on too. */
#ifndef TINYACTORS_TOOLS_CONFORM_H
#define TINYACTORS_TOOLS_CONFORM_H

#include <tinyactors/cbor.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>

#include "support/datamodel_test.h"

/* ---- bytes the program owns ---------------------------------------------------- */

/* A buffer on the C library's heap: pointer and capacity as a ta_buffer. */
void conform_buffer_free(ta_buffer *buffer);
/* Room for `capacity` bytes; 0 when the heap refused. */
int conform_buffer_reserve(ta_buffer *buffer, size_t capacity);

/* A call that writes into a ta_buffer, and answers TA_ERROR_NO_MEMORY with
 * the length it needs: a ta_buffer convention of core (types.h). */
typedef struct conform_writing {
    ta_status (*write)(void *context, ta_buffer *out);
    void *context;
} conform_writing;
/* The call into `out`, grown until it fits; its status. */
ta_status conform_write_growing(const conform_writing *writing, ta_buffer *out);

/* ---- a call --------------------------------------------------------------------- */

/* The protocol's errors (driver.md, "Frames and messages"). */
enum {
    CONFORM_PARSE_ERROR = -32700,
    CONFORM_INVALID_REQUEST = -32600,
    CONFORM_UNKNOWN_METHOD = -32601,
    CONFORM_INVALID_PARAMS = -32602,
    CONFORM_STATE = -32002
};

typedef struct conform_error {
    int32_t code; /* 0: none */
    const char *reason;
    char path[96];
    char message[160];
} conform_error;

/* A call, decoded: its params map (TA_NIL: none) and the first error a
 * param reader found. */
typedef struct conform_call {
    ta_cbor_document document;
    uint32_t params;
    conform_error error;
} conform_call;

/* The param `name` (a token index), or TA_NIL. */
uint32_t conform_param(const conform_call *call, const char *name);
/* The call fails with `code`, `reason` and a message, about `path`. */
typedef struct conform_failure {
    int32_t code;
    const char *reason;
    const char *path; /* "" for the call as a whole */
} conform_failure;
void conform_fail(conform_call *call, const conform_failure *failure, const char *message);
/* Readers of the params: each answers 1 when the param is absent (leaving
 * `out` as it is) or of its type, else 0 with the error set. */
int conform_text(conform_call *call, const char *name, ta_string_view *out);
int conform_bytes(conform_call *call, const char *name, ta_bytes *out);
int conform_integer(conform_call *call, const char *name, int64_t *out);
int conform_boolean(conform_call *call, const char *name, int *out);
/* 1 when the param is there; else 0 with -32602 missing. */
int conform_required(conform_call *call, const char *name);
/* The text of the token at `at`, or 0 when it is not text. */
int conform_token_text(const conform_call *call, uint32_t at, ta_string_view *out);

/* The array of {"name", "text"} maps the param `name` holds (documents,
 * charts), as sources to compile; 0 with the error set when it is not one. */
typedef struct conform_sources {
    ta_source items[32];
    uint32_t count;
} conform_sources;
int conform_sources_read(conform_call *call, const char *name, conform_sources *out);

/* An item of the call as its own CBOR bytes (ta_cbor_encode of its tokens),
 * with each text "@<alias>" in it replaced by the alias's address
 * (driver.md, "Scenario steps"). `aliases` may be NULL. */
typedef struct conform_alias {
    char name[64];
    uint64_t session;
} conform_alias;
typedef struct conform_aliases {
    const conform_alias *pointer;
    uint32_t count;
} conform_aliases;
typedef struct conform_item {
    uint32_t at;
    const conform_aliases *aliases;
} conform_item;
ta_status conform_item_bytes(const conform_call *call, const conform_item *item, ta_buffer *out);

/* ---- a result -------------------------------------------------------------------- */

enum { CONFORM_FIELD_INTEGER = 1, CONFORM_FIELD_TEXT, CONFORM_FIELD_BYTES, CONFORM_FIELD_BYTES_LIST };
typedef struct conform_bytes_list {
    const ta_bytes *pointer;
    uint32_t count;
} conform_bytes_list;
typedef struct conform_field {
    const char *name;
    uint32_t kind;
    int64_t integer;
    ta_string_view text;
    ta_bytes bytes;
    conform_bytes_list list;
} conform_field;
/* A result map, its members in the order they were added. Text and bytes
 * are borrowed until the reply is written. */
typedef struct conform_result {
    conform_field fields[8];
    uint32_t count;
} conform_result;
void conform_result_integer(conform_result *result, const char *name, int64_t value);
void conform_result_text(conform_result *result, const char *name, ta_string_view text);
void conform_result_bytes(conform_result *result, const char *name, ta_bytes bytes);
void conform_result_list(conform_result *result, const char *name, conform_bytes_list list);

/* ---- datamodels ------------------------------------------------------------------- */

/* The datamodel of a host: quickjs (ta_datamodel_ecmascript, either realm),
 * or the tests' datamodel. "native" is not offered (driver.md). */
typedef struct conform_datamodel {
    ta_datamodel_ecmascript *ecmascript;
    datamodel_test *test;
    uint32_t realm;
    ta_datamodel_binding binding;
} conform_datamodel;
/* The datamodel the call's `datamodel` and `realm` params name, made new; 0
 * with the call's error set when they name none it offers. */
int conform_datamodel_open(conform_datamodel *datamodel, conform_call *call);
/* Again, as it was opened (a restart): 0 when the heap refused. */
int conform_datamodel_reopen(conform_datamodel *datamodel);
void conform_datamodel_close(conform_datamodel *datamodel);
/* The vtable's from_wire and to_wire (TA_ERROR_UNSUPPORTED without them);
 * to_wire grows `out`. */
ta_status conform_datamodel_read(conform_datamodel *datamodel, ta_bytes cbor, ta_payload *out);
ta_status conform_datamodel_write(conform_datamodel *datamodel, ta_payload payload, ta_buffer *out);
void conform_datamodel_release(conform_datamodel *datamodel, ta_payload payload);
/* Its state for a snapshot (ta_datamodel_ecmascript_snapshot), and back;
 * TA_ERROR_UNSUPPORTED for the tests' datamodel. */
ta_status conform_datamodel_snapshot(conform_datamodel *datamodel, ta_buffer *out);
ta_status conform_datamodel_restore(conform_datamodel *datamodel, ta_bytes state);
/* The environment of ta_host_create with this datamodel and `error`. */
ta_host_environment conform_datamodel_environment(const conform_datamodel *datamodel, ta_buffer *error);

/* ---- images ------------------------------------------------------------------------ */

/* An image compiled from documents and kept for as long as a host runs it. */
typedef struct conform_image {
    void *bytes;
    ta_image image;
} conform_image;
/* Charts to compile into one image (the first is the default chart), and
 * the documents a src="" of theirs may name (`files`, by name, with a
 * leading file://, file: or ./ removed). */
typedef struct conform_documents {
    ta_source_list charts;
    ta_source_list files;
} conform_documents;
/* ta_compile_charts, ta_compile_image, ta_image_open; the diagnostics go to
 * standard error. Its status. */
ta_status conform_image_compile(const conform_documents *documents, conform_image *out);
void conform_image_free(conform_image *image);

/* ---- outcomes ---------------------------------------------------------------------- */

/* A LOG effect with the label "Outcome" (the W3C IRP's final states, and
 * the scenarios'): "pass" when its text is pass, else "fail"; NULL for any
 * other effect. */
const char *conform_outcome(const ta_host_effect *effect);

/* ---- methods ------------------------------------------------------------------------ */

/* What lives between calls: the host of the vector methods. */
typedef struct conform_vectors {
    ta_host *host;
    conform_datamodel datamodel;
    conform_image images[16];
    uint32_t image_count;
} conform_vectors;

typedef struct conform {
    conform_vectors vectors;
    ta_buffer kept[64]; /* bytes results borrow, freed after each reply */
    uint32_t kept_count;
} conform;

/* A copy of `bytes` that lives until the reply is written. */
ta_bytes conform_keep(conform *state, ta_bytes bytes);

typedef void (*conform_method_function)(conform *state, conform_call *call, conform_result *result);
typedef struct conform_method {
    const char *name;
    conform_method_function function;
    const char *const *params; /* the params it takes, NULL-terminated */
} conform_method;

/* The methods of each file, NULL-terminated by name. */
extern const conform_method conform_vector_methods[];
extern const conform_method conform_scenario_methods[];
extern const conform_method conform_w3c_methods[];

/* Frees the vector methods' host: at host.free and at the end. */
void conform_vectors_close(conform_vectors *vectors);

#endif /* TINYACTORS_TOOLS_CONFORM_H */

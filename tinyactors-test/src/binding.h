#pragma once

/*
 * Runtime for the RPC bindings generated from tinyactors.api.json
 * (`mise run bindings`). It owns the per-request arena, the protocol handle
 * table, the adapter registry, and the primitive JSON codecs that generated
 * struct and function codecs are built from.
 *
 * Codec conventions:
 * - decoders take a borrowed jv and never consume it; on failure they record
 *   an error on the binding and return false;
 * - encoders return an owned jv;
 * - everything a decoder allocates lives in the per-request arena.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jv.h"
#include "ecmascript_datamodel.h"
#include "json_datamodel_jv.h"
#include "tinyactors.h"

/* Implementation-defined JSON-RPC server error: the bound core function is
 * declared in tinyactors.h but not yet implemented in tinyactors-core. */
enum { TAT_RPC_NOT_IMPLEMENTED = -32001 };

typedef struct {
    const char *type;
    void *ptr;
    bool live;
    bool fixture;
} tat_handle;

typedef struct tat_bind tat_bind;

/* Builds a vtable-bearing adapter value (ta_datamodel, ta_allocator, ...)
 * from its JSON config. `out` points at a value of the adapter's C type. */
typedef bool (*tat_adapter_factory)(tat_bind *bind, jv config, void *out);

typedef struct {
    const char *name;
    const char *type;
    size_t size;
    tat_adapter_factory make;
} tat_adapter;

struct tat_bind {
    ta_allocator allocator;
    ta_arena arena;
    /* Bytes the current request allocated; a large request's arena is freed. */
    size_t request_bytes;
    tat_json_datamodel_context json;
    tat_js_datamodel_context js;
    /* The data model ta_value inputs of the current request decode with. */
    ta_datamodel decode_model;
    tat_handle *handles;
    size_t handle_count;
    size_t handle_capacity;
    ta_value *values;
    size_t value_count;
    size_t value_capacity;
    jv error;
    void *host;
    /*
     * Named actor refs are interned address handles (ADR 0065). A request
     * that names a system interns into it; one that does not (the codec
     * round trips) interns into the binding's own system. Every reference a
     * request takes is released when it ends.
     */
    ta_system *request_system;
    ta_system *codec_system;
    ta_address **addresses;
    size_t address_count;
    size_t address_capacity;
};

typedef jv (*tat_method_fn)(tat_bind *bind, jv params, int *error_code);

typedef struct {
    const char *name;
    tat_method_fn handler;
    /* JSON text: {"function", "implemented", "params", "result", "out"}. */
    const char *schema;
} tat_method;

ta_result tat_bind_init(tat_bind *bind, ta_allocator allocator, void *host);
void tat_bind_destroy(tat_bind *bind);
/* Resets per-request state; call before and after each dispatched method. */
void tat_bind_begin(tat_bind *bind);
void tat_bind_end(tat_bind *bind);

/* Returns the recorded decode error as an RPC error result. */
jv tat_bind_take_error(tat_bind *bind, int *error_code);
bool tat_fail(tat_bind *bind, const char *format, ...);
/* Prefixes the recorded error with ".name" and returns false. */
bool tat_at(tat_bind *bind, const char *name);
bool tat_at_index(tat_bind *bind, size_t index);
jv tat_not_implemented(tat_bind *bind, const char *function, int *error_code);

void *tat_alloc(tat_bind *bind, size_t size, size_t alignment);
/* `count` elements of `size` bytes; fails instead of overflowing. */
void *tat_alloc_array(tat_bind *bind, size_t count, size_t size, size_t alignment);

/* Object members: true when present and not null; *out is owned. */
bool tat_member(jv object, const char *name, jv *out);
bool tat_expect_object(tat_bind *bind, jv value);
bool tat_check_keys(tat_bind *bind, jv object, const char *const *keys);
bool tat_expect_array(tat_bind *bind, jv value, size_t *count);

/* Primitive codecs. 64-bit values encode as JSON numbers up to 2^53-1 and as
 * decimal strings above; decoders accept both. */
bool tat_decode_u8(tat_bind *bind, jv value, uint8_t *out);
bool tat_decode_u16(tat_bind *bind, jv value, uint16_t *out);
bool tat_decode_u32(tat_bind *bind, jv value, uint32_t *out);
bool tat_decode_i32(tat_bind *bind, jv value, int32_t *out);
bool tat_decode_u64(tat_bind *bind, jv value, uint64_t *out);
bool tat_decode_size(tat_bind *bind, jv value, size_t *out);
bool tat_decode_bool(tat_bind *bind, jv value, bool *out);
jv tat_encode_u8(tat_bind *bind, const uint8_t *value);
jv tat_encode_u16(tat_bind *bind, const uint16_t *value);
jv tat_encode_u32(tat_bind *bind, const uint32_t *value);
jv tat_encode_i32(tat_bind *bind, const int32_t *value);
jv tat_encode_u64(tat_bind *bind, const uint64_t *value);
jv tat_encode_size(tat_bind *bind, const size_t *value);
jv tat_encode_bool(tat_bind *bind, const bool *value);

/* Strings are copied into the request arena; a null ptr encodes as null. */
bool tat_decode_ta_str(tat_bind *bind, jv value, ta_str *out);
jv tat_encode_ta_str(tat_bind *bind, const ta_str *value);
bool tat_decode_ta_bytes(tat_bind *bind, jv value, ta_bytes *out);
jv tat_encode_ta_bytes(tat_bind *bind, const ta_bytes *value);
/* A caller-supplied output buffer: the NUL-terminated prefix, as a string. */
jv tat_encode_buffer(const char *buffer, uint32_t capacity);

/* Overrides for types that cannot be mapped structurally. */
bool tat_decode_ta_value(tat_bind *bind, jv value, ta_value *out);
jv tat_encode_ta_value(tat_bind *bind, const ta_value *value);
bool tat_decode_ta_expression(tat_bind *bind, jv value, ta_expression *out);
jv tat_encode_ta_expression(tat_bind *bind, const ta_expression *value);
bool tat_decode_ta_actor_ref(tat_bind *bind, jv value, ta_actor_ref *out);
jv tat_encode_ta_actor_ref(tat_bind *bind, const ta_actor_ref *value);

/* Protocol handles: {"$handle": "<type>:<id>"}. IDs are never reused, so a
 * retired handle stays invalid. */
bool tat_handle_resolve(tat_bind *bind, jv value, const char *type, void **out);
jv tat_handle_register(tat_bind *bind, const char *type, void *ptr);
jv tat_handle_encode(tat_bind *bind, const char *type, const void *ptr);
void tat_handle_retire(tat_bind *bind, const void *ptr);
jv tat_handle_fixture(tat_bind *bind, const char *type);
jv tat_borrowed_encode(const char *type, const void *ptr);

/* Methods whose ta_value inputs belong to the data model of the actor,
 * definition or session image named by `member` (the API description's value
 * models). An image's data model is its definition's, found with
 * ta_session_image_definition(). The
 * generated bindings define the table; value_codec.c consults it. */
typedef enum { TAT_VALUE_MODEL_ACTOR, TAT_VALUE_MODEL_DEFINITION, TAT_VALUE_MODEL_SESSION_IMAGE } tat_value_model;

typedef struct {
    const char *method;
    const char *member;
    tat_value_model model;
} tat_value_receiver;

extern const tat_value_receiver TAT_VALUE_RECEIVERS[];
extern const size_t TAT_VALUE_RECEIVER_COUNT;

/* Adapters: {"$adapter": "<name>", "config": {...}}. */
bool tat_adapter_decode(tat_bind *bind, jv value, const char *type, void *out, size_t size);
jv tat_adapter_encode(tat_bind *bind, const char *type, const void *value, size_t size);
extern const tat_adapter TAT_ADAPTERS[];
extern const size_t TAT_ADAPTER_COUNT;

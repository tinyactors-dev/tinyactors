/* Private, synchronous Node-API subset used by shared/binding.c.
 * This is not a general Node-API implementation. Values are scoped JS table
 * handles, references are persistent handles, and externals own C finalizers.
 * Native builds use Node's own header instead. Keep enum/layout values aligned
 * with Node-API; native.mjs implements these imports for wasm32 only. */
#ifndef TINYACTORS_WEB_NODE_API_H
#define TINYACTORS_WEB_NODE_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef struct napi_ref__ *napi_ref;
typedef struct napi_callback_info__ *napi_callback_info;
typedef napi_value (*napi_callback)(napi_env, napi_callback_info);
typedef void (*napi_finalize)(napi_env, void *, void *);
typedef enum { napi_ok = 0 } napi_status;
typedef enum {
    napi_undefined,
    napi_null,
    napi_boolean,
    napi_number,
    napi_string,
    napi_symbol,
    napi_object,
    napi_function,
    napi_external,
    napi_bigint
} napi_valuetype;
typedef enum { napi_int8_array, napi_uint8_array } napi_typedarray_type;
enum { napi_default_method = 5 };
#define NAPI_AUTO_LENGTH SIZE_MAX

typedef struct {
    const char *utf8name;
    napi_value name;
    napi_callback method, getter, setter;
    napi_value value;
    uint32_t attributes;
    void *data;
} napi_property_descriptor;

#define API(name) __attribute__((import_module("napi"), import_name(#name))) napi_status name
API(napi_get_undefined)(napi_env, napi_value *);
API(napi_get_null)(napi_env, napi_value *);
API(napi_get_global)(napi_env, napi_value *);
API(napi_get_boolean)(napi_env, bool, napi_value *);
API(napi_create_object)(napi_env, napi_value *);
API(napi_create_array)(napi_env, napi_value *);
API(napi_create_array_with_length)(napi_env, size_t, napi_value *);
API(napi_create_double)(napi_env, double, napi_value *);
API(napi_create_int32)(napi_env, int32_t, napi_value *);
API(napi_create_uint32)(napi_env, uint32_t, napi_value *);
API(napi_create_string_utf8)(napi_env, const char *, size_t, napi_value *);
API(napi_create_error)(napi_env, napi_value, napi_value, napi_value *);
API(napi_typeof)(napi_env, napi_value, napi_valuetype *);
API(napi_get_value_bool)(napi_env, napi_value, bool *);
API(napi_get_value_double)(napi_env, napi_value, double *);
API(napi_get_value_int32)(napi_env, napi_value, int32_t *);
API(napi_get_value_uint32)(napi_env, napi_value, uint32_t *);
API(napi_get_value_string_utf8)(napi_env, napi_value, char *, size_t, size_t *);
API(napi_coerce_to_bool)(napi_env, napi_value, napi_value *);
API(napi_coerce_to_string)(napi_env, napi_value, napi_value *);
API(napi_strict_equals)(napi_env, napi_value, napi_value, bool *);
API(napi_is_array)(napi_env, napi_value, bool *);
API(napi_get_array_length)(napi_env, napi_value, uint32_t *);
API(napi_get_element)(napi_env, napi_value, uint32_t, napi_value *);
API(napi_set_element)(napi_env, napi_value, uint32_t, napi_value);
/* Names passed to the *_named_property calls must be string literals: the
 * adapter decodes each address once (bindings/web/native.mjs). */
API(napi_get_named_property)(napi_env, napi_value, const char *, napi_value *);
API(napi_set_named_property)(napi_env, napi_value, const char *, napi_value);
API(napi_has_named_property)(napi_env, napi_value, const char *, bool *);
API(napi_get_property)(napi_env, napi_value, napi_value, napi_value *);
API(napi_set_property)(napi_env, napi_value, napi_value, napi_value);
API(napi_get_property_names)(napi_env, napi_value, napi_value *);
API(napi_define_properties)(napi_env, napi_value, size_t, const napi_property_descriptor *);
API(napi_create_reference)(napi_env, napi_value, uint32_t, napi_ref *);
API(napi_delete_reference)(napi_env, napi_ref);
API(napi_get_reference_value)(napi_env, napi_ref, napi_value *);
API(napi_create_external)(napi_env, void *, napi_finalize, void *, napi_value *);
API(napi_get_value_external)(napi_env, napi_value, void **);
API(napi_create_function)(napi_env, const char *, size_t, napi_callback, void *, napi_value *);
API(napi_get_cb_info)(napi_env, napi_callback_info, size_t *, napi_value *, napi_value *, void **);
API(napi_call_function)(napi_env, napi_value, napi_value, size_t, const napi_value *, napi_value *);
API(napi_create_arraybuffer)(napi_env, size_t, void **, napi_value *);
API(napi_create_typedarray)(napi_env, napi_typedarray_type, size_t, napi_value, size_t,
                            napi_value *);
API(napi_get_typedarray_info)(napi_env, napi_value, napi_typedarray_type *, size_t *, void **,
                              napi_value *, size_t *);
API(napi_throw)(napi_env, napi_value);
API(napi_throw_error)(napi_env, const char *, const char *);
API(napi_throw_type_error)(napi_env, const char *, const char *);
API(napi_is_exception_pending)(napi_env, bool *);
API(napi_get_and_clear_last_exception)(napi_env, napi_value *);
#undef API
#endif

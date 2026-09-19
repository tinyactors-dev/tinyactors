#include "binding.h"

#include "rpc.h"
#include "value_codec.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAT_BIND_ARENA_BLOCK = 64 * 1024 };

static const double TAT_MAX_EXACT = 9007199254740991.0; /* 2^53 - 1 */

/* A distinct address that fixture handles point at; never dereferenced. */
static const char TAT_FIXTURE_TARGET = 0;

ta_result tat_bind_init(tat_bind *bind, ta_allocator allocator, void *host) {
    *bind = (tat_bind){.error = jv_invalid(), .host = host};
    ta_result result = ta_arena_init(&bind->arena, allocator, TAT_BIND_ARENA_BLOCK);
    if (result != TA_OK) {
        return result;
    }
    result = tat_json_datamodel_context_init(&bind->json, allocator);
    if (result != TA_OK) {
        ta_arena_destroy(&bind->arena);
        return result;
    }
    result = tat_js_datamodel_context_init(&bind->js, &(tat_js_datamodel_config){.allocator = allocator});
    if (result != TA_OK) {
        tat_json_datamodel_context_destroy(&bind->json);
        ta_arena_destroy(&bind->arena);
        return result;
    }
    bind->decode_model = tat_json_datamodel(&bind->json);
    return TA_OK;
}

void tat_bind_destroy(tat_bind *bind) {
    tat_bind_end(bind);
    free(bind->values);
    free(bind->handles);
    tat_js_datamodel_context_destroy(&bind->js);
    tat_json_datamodel_context_destroy(&bind->json);
    ta_arena_destroy(&bind->arena);
    *bind = (tat_bind){.error = jv_invalid()};
}

void tat_bind_begin(tat_bind *bind) {
    ta_arena_reset(&bind->arena);
    bind->decode_model = tat_json_datamodel(&bind->json);
    jv_free(bind->error);
    bind->error = jv_invalid();
}

void tat_bind_end(tat_bind *bind) {
    for (size_t index = 0; index < bind->value_count; index += 1) {
        ta_value_release(&bind->values[index]);
    }
    bind->value_count = 0;
    jv_free(bind->error);
    bind->error = jv_invalid();
    ta_arena_reset(&bind->arena);
}

jv tat_bind_take_error(tat_bind *bind, int *error_code) {
    *error_code = RPC_INVALID_PARAMS;
    jv message = jv_is_valid(bind->error) ? bind->error : jv_string(": invalid params");
    bind->error = jv_invalid();
    jv result = jv_string_fmt("params%s", jv_string_value(message));
    jv_free(message);
    return result;
}

bool tat_fail(tat_bind *bind, const char *format, ...) {
    if (jv_is_valid(bind->error)) {
        return false;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    bind->error = jv_string_fmt(": %s", text);
    return false;
}

static bool tat_prefix(tat_bind *bind, jv prefix) {
    jv current = jv_is_valid(bind->error) ? bind->error : jv_string(": invalid");
    bind->error = jv_string_concat(prefix, current);
    return false;
}

bool tat_at(tat_bind *bind, const char *name) {
    return tat_prefix(bind, jv_string_fmt(".%s", name));
}

bool tat_at_index(tat_bind *bind, size_t index) {
    return tat_prefix(bind, jv_string_fmt("[%zu]", index));
}

jv tat_not_implemented(tat_bind *bind, const char *function, int *error_code) {
    (void)bind;
    *error_code = TAT_RPC_NOT_IMPLEMENTED;
    return jv_string_fmt("%s is not implemented in tinyactors-core", function);
}

void *tat_alloc(tat_bind *bind, size_t size, size_t alignment) {
    void *memory = ta_arena_alloc(&bind->arena, size == 0 ? 1 : size, alignment);
    if (memory == NULL) {
        tat_fail(bind, "out of memory");
        return NULL;
    }
    memset(memory, 0, size);
    return memory;
}

bool tat_member(jv object, const char *name, jv *out) {
    if (jv_get_kind(object) != JV_KIND_OBJECT) {
        *out = jv_invalid();
        return false;
    }
    jv value = jv_object_get(jv_copy(object), jv_string(name));
    if (!jv_is_valid(value) || jv_get_kind(value) == JV_KIND_NULL) {
        jv_free(value);
        *out = jv_invalid();
        return false;
    }
    *out = value;
    return true;
}

bool tat_expect_object(tat_bind *bind, jv value) {
    if (jv_get_kind(value) == JV_KIND_OBJECT) {
        return true;
    }
    return tat_fail(bind, "expected an object");
}

bool tat_check_keys(tat_bind *bind, jv object, const char *const *keys) {
    bool ok = true;
    jv_object_foreach(jv_copy(object), key, value) {
        jv_free(value);
        bool known = false;
        for (const char *const *each = keys; *each != NULL; each += 1) {
            if (strcmp(*each, jv_string_value(key)) == 0) {
                known = true;
                break;
            }
        }
        if (!known && ok) {
            ok = tat_fail(bind, "unknown member \"%s\"", jv_string_value(key));
        }
        jv_free(key);
    }
    return ok;
}

bool tat_expect_array(tat_bind *bind, jv value, size_t *count) {
    if (jv_get_kind(value) != JV_KIND_ARRAY) {
        return tat_fail(bind, "expected an array");
    }
    *count = (size_t)jv_array_length(jv_copy(value));
    return true;
}

static bool tat_integer(tat_bind *bind, jv value, double low, double high, double *out) {
    if (jv_get_kind(value) != JV_KIND_NUMBER) {
        return tat_fail(bind, "expected an integer");
    }
    double number = jv_number_value(value);
    if (number != floor(number) || number < low || number > high) {
        return tat_fail(bind, "integer %.17g is out of range", number);
    }
    *out = number;
    return true;
}

bool tat_decode_u8(tat_bind *bind, jv value, uint8_t *out) {
    double number;
    if (!tat_integer(bind, value, 0, UINT8_MAX, &number)) {
        return false;
    }
    *out = (uint8_t)number;
    return true;
}

bool tat_decode_u16(tat_bind *bind, jv value, uint16_t *out) {
    double number;
    if (!tat_integer(bind, value, 0, UINT16_MAX, &number)) {
        return false;
    }
    *out = (uint16_t)number;
    return true;
}

bool tat_decode_u32(tat_bind *bind, jv value, uint32_t *out) {
    double number;
    if (!tat_integer(bind, value, 0, UINT32_MAX, &number)) {
        return false;
    }
    *out = (uint32_t)number;
    return true;
}

bool tat_decode_i32(tat_bind *bind, jv value, int32_t *out) {
    double number;
    if (!tat_integer(bind, value, INT32_MIN, INT32_MAX, &number)) {
        return false;
    }
    *out = (int32_t)number;
    return true;
}

bool tat_decode_u64(tat_bind *bind, jv value, uint64_t *out) {
    if (jv_get_kind(value) == JV_KIND_STRING) {
        const char *text = jv_string_value(value);
        if (text[0] == '\0') {
            return tat_fail(bind, "expected a decimal string");
        }
        uint64_t result = 0;
        for (const char *digit = text; *digit != '\0'; digit += 1) {
            if (*digit < '0' || *digit > '9') {
                return tat_fail(bind, "expected a decimal string");
            }
            uint64_t next = (uint64_t)(*digit - '0');
            if (result > (UINT64_MAX - next) / 10u) {
                return tat_fail(bind, "decimal string exceeds 64 bits");
            }
            result = result * 10u + next;
        }
        *out = result;
        return true;
    }
    double number;
    if (!tat_integer(bind, value, 0, TAT_MAX_EXACT, &number)) {
        return false;
    }
    *out = (uint64_t)number;
    return true;
}

bool tat_decode_size(tat_bind *bind, jv value, size_t *out) {
    uint64_t wide;
    if (!tat_decode_u64(bind, value, &wide)) {
        return false;
    }
    if (wide > SIZE_MAX) {
        return tat_fail(bind, "size exceeds size_t");
    }
    *out = (size_t)wide;
    return true;
}

bool tat_decode_bool(tat_bind *bind, jv value, bool *out) {
    jv_kind kind = jv_get_kind(value);
    if (kind != JV_KIND_TRUE && kind != JV_KIND_FALSE) {
        return tat_fail(bind, "expected a boolean");
    }
    *out = kind == JV_KIND_TRUE;
    return true;
}

jv tat_encode_u8(tat_bind *bind, const uint8_t *value) {
    (void)bind;
    return jv_number(*value);
}

jv tat_encode_u16(tat_bind *bind, const uint16_t *value) {
    (void)bind;
    return jv_number(*value);
}

jv tat_encode_u32(tat_bind *bind, const uint32_t *value) {
    (void)bind;
    return jv_number(*value);
}

jv tat_encode_i32(tat_bind *bind, const int32_t *value) {
    (void)bind;
    return jv_number(*value);
}

jv tat_encode_u64(tat_bind *bind, const uint64_t *value) {
    (void)bind;
    if (*value <= (uint64_t)TAT_MAX_EXACT) {
        return jv_number((double)*value);
    }
    return jv_string_fmt("%" PRIu64, *value);
}

jv tat_encode_size(tat_bind *bind, const size_t *value) {
    uint64_t wide = (uint64_t)*value;
    return tat_encode_u64(bind, &wide);
}

jv tat_encode_bool(tat_bind *bind, const bool *value) {
    (void)bind;
    return jv_bool(*value);
}

static bool tat_copy_string(tat_bind *bind, jv value, const char **ptr, uint32_t *len) {
    if (jv_get_kind(value) != JV_KIND_STRING) {
        return tat_fail(bind, "expected a string");
    }
    int length = jv_string_length_bytes(jv_copy(value));
    char *copy = tat_alloc(bind, (size_t)length + 1, 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, jv_string_value(value), (size_t)length);
    *ptr = copy;
    *len = (uint32_t)length;
    return true;
}

bool tat_decode_ta_str(tat_bind *bind, jv value, ta_str *out) {
    return tat_copy_string(bind, value, &out->ptr, &out->len);
}

jv tat_encode_ta_str(tat_bind *bind, const ta_str *value) {
    (void)bind;
    if (value->ptr == NULL) {
        return jv_null();
    }
    return jv_string_sized(value->ptr, (int)value->len);
}

bool tat_decode_ta_bytes(tat_bind *bind, jv value, ta_bytes *out) {
    const char *ptr = NULL;
    if (!tat_copy_string(bind, value, &ptr, &out->len)) {
        return false;
    }
    out->ptr = (const uint8_t *)ptr;
    return true;
}

jv tat_encode_ta_bytes(tat_bind *bind, const ta_bytes *value) {
    (void)bind;
    if (value->ptr == NULL) {
        return jv_null();
    }
    return jv_string_sized((const char *)value->ptr, (int)value->len);
}

jv tat_encode_buffer(const char *buffer, uint32_t capacity) {
    if (buffer == NULL) {
        return jv_null();
    }
    uint32_t length = 0;
    while (length < capacity && buffer[length] != '\0') {
        length += 1;
    }
    return jv_string_sized(buffer, (int)length);
}

bool tat_decode_ta_value(tat_bind *bind, jv value, ta_value *out) {
    if (bind->value_count == bind->value_capacity) {
        size_t capacity = bind->value_capacity == 0 ? 16 : bind->value_capacity * 2;
        ta_value *grown = realloc(bind->values, capacity * sizeof(*grown));
        if (grown == NULL) {
            return tat_fail(bind, "out of memory");
        }
        bind->values = grown;
        bind->value_capacity = capacity;
    }
    /* The receiving actor's data model (tat_value_codec_select) decodes it. */
    if (tat_value_decode(bind->decode_model, value, out) != TA_OK) {
        return tat_fail(bind, "could not create a %s value", bind->decode_model.v->name.ptr);
    }
    /* The request owns the decoded reference; callees retain what they keep. */
    bind->values[bind->value_count] = *out;
    bind->value_count += 1;
    return true;
}

jv tat_encode_ta_value(tat_bind *bind, const ta_value *value) {
    (void)bind;
    return tat_value_encode(value);
}

bool tat_decode_ta_expression(tat_bind *bind, jv value, ta_expression *out) {
    if (jv_get_kind(value) != JV_KIND_STRING) {
        return tat_fail(bind, "expected expression source text; host expressions cannot cross RPC");
    }
    out->kind = TA_EXPRESSION_SOURCE;
    return tat_decode_ta_str(bind, value, &out->as.source);
}

jv tat_encode_ta_expression(tat_bind *bind, const ta_expression *value) {
    switch (value->kind) {
        case TA_EXPRESSION_SOURCE:
            return tat_encode_ta_str(bind, &value->as.source);
        case TA_EXPRESSION_HOST:
            return jv_object_set(jv_object(), jv_string("$expression"), jv_string("host"));
        default:
            return jv_null();
    }
}

static const char *tat_handle_text(jv value) {
    if (jv_get_kind(value) != JV_KIND_OBJECT) {
        return NULL;
    }
    jv text = jv_object_get(jv_copy(value), jv_string("$handle"));
    if (jv_get_kind(text) != JV_KIND_STRING) {
        jv_free(text);
        return NULL;
    }
    /* The object keeps its member alive while `value` is borrowed. */
    const char *result = jv_string_value(text);
    jv_free(text);
    return result;
}

bool tat_handle_resolve(tat_bind *bind, jv value, const char *type, void **out) {
    const char *text = tat_handle_text(value);
    if (text == NULL) {
        return tat_fail(bind, "expected a %s handle {\"$handle\": \"%s:<id>\"}", type, type);
    }
    const char *colon = strrchr(text, ':');
    char *end = NULL;
    unsigned long long id = colon == NULL ? 0 : strtoull(colon + 1, &end, 10);
    if (colon == NULL || end == colon + 1 || *end != '\0' || id == 0 ||
        id > bind->handle_count) {
        return tat_fail(bind, "unknown handle \"%s\"", text);
    }
    const tat_handle *handle = &bind->handles[id - 1];
    size_t prefix = (size_t)(colon - text);
    if (strlen(handle->type) != prefix || strncmp(handle->type, text, prefix) != 0) {
        return tat_fail(bind, "unknown handle \"%s\"", text);
    }
    if (strcmp(handle->type, type) != 0) {
        return tat_fail(bind, "expected a %s handle, got \"%s\"", type, text);
    }
    if (!handle->live) {
        return tat_fail(bind, "handle \"%s\" was retired", text);
    }
    if (handle->fixture) {
        return tat_fail(bind, "fixture handle \"%s\" cannot reach native code", text);
    }
    *out = handle->ptr;
    return true;
}

static jv tat_handle_json(const tat_handle *handle, size_t id) {
    return jv_object_set(jv_object(), jv_string("$handle"),
                         jv_string_fmt("%s:%zu", handle->type, id));
}

static jv tat_handle_add(tat_bind *bind, const char *type, void *ptr, bool fixture) {
    if (bind->handle_count == bind->handle_capacity) {
        size_t capacity = bind->handle_capacity == 0 ? 16 : bind->handle_capacity * 2;
        tat_handle *grown = realloc(bind->handles, capacity * sizeof(*grown));
        if (grown == NULL) {
            return jv_null();
        }
        bind->handles = grown;
        bind->handle_capacity = capacity;
    }
    tat_handle *handle = &bind->handles[bind->handle_count];
    *handle = (tat_handle){.type = type, .ptr = ptr, .live = true, .fixture = fixture};
    bind->handle_count += 1;
    return tat_handle_json(handle, bind->handle_count);
}

jv tat_handle_register(tat_bind *bind, const char *type, void *ptr) {
    if (ptr == NULL) {
        return jv_null();
    }
    return tat_handle_add(bind, type, ptr, false);
}

jv tat_handle_encode(tat_bind *bind, const char *type, const void *ptr) {
    if (ptr == NULL) {
        return jv_null();
    }
    for (size_t index = bind->handle_count; index > 0; index -= 1) {
        const tat_handle *handle = &bind->handles[index - 1];
        if (handle->live && handle->ptr == ptr && strcmp(handle->type, type) == 0) {
            return tat_handle_json(handle, index);
        }
    }
    return jv_object_set(jv_object(), jv_string("$handle"), jv_null());
}

void tat_handle_retire(tat_bind *bind, const void *ptr) {
    for (size_t index = 0; index < bind->handle_count; index += 1) {
        if (bind->handles[index].ptr == ptr && !bind->handles[index].fixture) {
            bind->handles[index].live = false;
        }
    }
}

jv tat_handle_fixture(tat_bind *bind, const char *type) {
    return tat_handle_add(bind, type, (void *)&TAT_FIXTURE_TARGET, true);
}

jv tat_borrowed_encode(const char *type, const void *ptr) {
    if (ptr == NULL) {
        return jv_null();
    }
    return jv_object_set(jv_object(), jv_string("$borrowed"), jv_string(type));
}

static const tat_adapter *tat_adapter_find(const char *type, const char *name) {
    for (size_t index = 0; index < TAT_ADAPTER_COUNT; index += 1) {
        const tat_adapter *adapter = &TAT_ADAPTERS[index];
        if (strcmp(adapter->type, type) == 0 && (name == NULL || strcmp(adapter->name, name) == 0)) {
            return adapter;
        }
    }
    return NULL;
}

bool tat_adapter_decode(tat_bind *bind, jv value, const char *type, void *out, size_t size) {
    if (jv_get_kind(value) != JV_KIND_OBJECT) {
        return tat_fail(bind, "expected an adapter {\"$adapter\": \"<name>\"}");
    }
    jv name = jv_object_get(jv_copy(value), jv_string("$adapter"));
    if (jv_get_kind(name) != JV_KIND_STRING) {
        jv_free(name);
        return tat_fail(bind, "expected an adapter {\"$adapter\": \"<name>\"}");
    }
    static const char *const keys[] = {"$adapter", "config", NULL};
    if (!tat_check_keys(bind, value, keys)) {
        jv_free(name);
        return false;
    }
    const tat_adapter *adapter = tat_adapter_find(type, jv_string_value(name));
    if (adapter == NULL || adapter->size != size) {
        bool result = tat_fail(bind, "no %s adapter named \"%s\"", type, jv_string_value(name));
        jv_free(name);
        return result;
    }
    jv_free(name);
    jv config = jv_object_get(jv_copy(value), jv_string("config"));
    if (!jv_is_valid(config)) {
        config = jv_null();
    }
    memset(out, 0, size);
    bool ok = adapter->make(bind, config, out);
    jv_free(config);
    return ok;
}

jv tat_adapter_encode(tat_bind *bind, const char *type, const void *value, size_t size) {
    for (size_t index = 0; index < TAT_ADAPTER_COUNT; index += 1) {
        const tat_adapter *adapter = &TAT_ADAPTERS[index];
        if (strcmp(adapter->type, type) != 0 || adapter->size != size) {
            continue;
        }
        unsigned char prototype[64] = {0};
        if (size > sizeof(prototype) || !adapter->make(bind, jv_null(), prototype)) {
            continue;
        }
        /* Compare everything after `self`: the vtable or callback pointer. */
        if (memcmp(prototype + sizeof(void *), (const unsigned char *)value + sizeof(void *),
                   size - sizeof(void *)) == 0) {
            return jv_object_set(jv_object(), jv_string("$adapter"), jv_string(adapter->name));
        }
    }
    return jv_object_set(jv_object(), jv_string("$adapter"), jv_null());
}

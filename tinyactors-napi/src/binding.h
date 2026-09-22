#ifndef TINYACTORS_NAPI_BINDING_H
#define TINYACTORS_NAPI_BINDING_H

#include <node_api.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../tinyactors-core/include/tinyactors.h"

#if defined(__GNUC__) || defined(__clang__)
#define TINYACTORS_NAPI_UNUSED __attribute__((unused))
#else
#define TINYACTORS_NAPI_UNUSED
#endif

/*
 * The binding keeps N-API error handling in one place.  Callers can use the
 * boolean result in a guard and return NULL from a napi_value function.
 */
static bool tinyactors_napi_check(napi_env env, napi_status status, const char *operation) {
    if (status == napi_ok) {
        return true;
    }
    (void)napi_throw_error(env, NULL, operation);
    return false;
}

static const char *tinyactors_napi_result_name(ta_result result) {
    switch (result) {
    case TA_OK: return "ok";
    case TA_ERR_INVALID: return "invalid argument";
    case TA_ERR_NOMEM: return "out of memory";
    case TA_ERR_NOT_FOUND: return "not found";
    case TA_ERR_NOT_RECONSTRUCTIBLE: return "not reconstructible";
    case TA_ERR_BUSY: return "busy";
    case TA_ERR_UNSUPPORTED: return "unsupported";
    case TA_ERR_CORRUPT: return "corrupt";
    case TA_ERR_DEFINITION_MISMATCH: return "definition mismatch";
    case TA_ERR_UNLOADED: return "unloaded";
    case TA_ERR_STALE: return "stale";
    default: return "unknown Tinyactors error";
    }
}

static TINYACTORS_NAPI_UNUSED bool tinyactors_napi_check_result(napi_env env, ta_result result,
                                                                const char *operation) {
    if (result == TA_OK) {
        return true;
    }
    char message[128];
    (void)snprintf(message, sizeof(message), "%s: %s (%d)", operation,
                   tinyactors_napi_result_name(result), (int)result);
    (void)napi_throw_error(env, NULL, message);
    return false;
}

/* Convert one JavaScript string to an owned, NUL-terminated UTF-8 buffer. */
static TINYACTORS_NAPI_UNUSED bool tinyactors_napi_get_utf8(napi_env env, napi_value value,
                                                            char **out, size_t *length) {
    size_t needed = 0;
    if (out == NULL || !tinyactors_napi_check(env,
            napi_get_value_string_utf8(env, value, NULL, 0, &needed),
            "napi_get_value_string_utf8")) {
        return false;
    }
    char *text = malloc(needed + 1u);
    if (text == NULL) {
        (void)napi_throw_error(env, NULL, "out of memory");
        return false;
    }
    if (!tinyactors_napi_check(env,
            napi_get_value_string_utf8(env, value, text, needed + 1u, &needed),
            "napi_get_value_string_utf8")) {
        free(text);
        return false;
    }
    *out = text;
    if (length != NULL) {
        *length = needed;
    }
    return true;
}

static TINYACTORS_NAPI_UNUSED void tinyactors_napi_free_utf8(char *text) {
    free(text);
}

static TINYACTORS_NAPI_UNUSED bool tinyactors_napi_get_uint64(napi_env env, napi_value value,
                                                              uint64_t *out) {
    bool lossless = false;
    if (out == NULL || !tinyactors_napi_check(env,
            napi_get_value_bigint_uint64(env, value, out, &lossless),
            "expected a bigint")) {
        return false;
    }
    if (!lossless) {
        (void)napi_throw_range_error(env, NULL, "bigint is outside the uint64 range");
        return false;
    }
    return true;
}

static TINYACTORS_NAPI_UNUSED bool tinyactors_napi_create_uint64(napi_env env, uint64_t value,
                                                                 napi_value *out) {
    return tinyactors_napi_check(env, napi_create_bigint_uint64(env, value, out),
                                 "napi_create_bigint_uint64");
}

/* Tinyactors receives ownership-compatible allocations from the host. */
static void *tinyactors_napi_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (size == 0 || alignment > _Alignof(max_align_t)) {
        return NULL;
    }
    return malloc(size);
}

static void tinyactors_napi_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    if (params != NULL) {
        free(params->ptr);
    }
}

static const ta_allocator_vtable tinyactors_napi_allocator_vtable = {
    .alloc = tinyactors_napi_alloc,
    .free = tinyactors_napi_free,
};

static TINYACTORS_NAPI_UNUSED ta_allocator tinyactors_napi_allocator(void) {
    return (ta_allocator){
        .self = NULL,
        .v = &tinyactors_napi_allocator_vtable,
    };
}

#endif

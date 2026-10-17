/* The native datamodel's rows (AGENTS.md, "The binding's memory"; as
 * tinyactors-cpython/src/datamodel/rows.c): its charts, sessions, payloads
 * and <foreach> loops are rows of the host's tables (core's ta_table, grown
 * through the host's allocator, freed with the host). A chart's, a
 * payload's and a loop's row hold one napi_ref first; a session's scope is
 * the element at its row's slot of one array per host (`scopes`), so a
 * session holds no napi_ref (node's is about 96 B of malloc an actor). A payload's reference count lives in its row; at zero
 * the row goes, and its value is kept in the drops until the flush after
 * the outer call into core returns (dropping the last reference can run
 * JavaScript: bindings.md, section 8). While the host is freed (a
 * finalizer, the environment's teardown) a reference is deleted at once,
 * a basic-env call. */
#include "datamodel.h"

#include <string.h>

napi_value ta_napi_native_get(ta_napi_native *native, napi_ref reference) {
    napi_value value = NULL;
    if (reference == NULL || napi_get_reference_value(native->host->env, reference, &value) != napi_ok) return NULL;
    return value;
}

void ta_napi_native_drop(ta_napi_native *native, napi_ref reference) {
    if (reference == NULL) return;
    napi_env env = native->host->env;
    napi_value value = NULL, drops = NULL;
    uint32_t length = 0;
    if (!native->freeing) { /* kept until the flush: the drops hold it */
        napi_handle_scope scope;
        if (napi_open_handle_scope(env, &scope) == napi_ok) {
            if (napi_get_reference_value(env, reference, &value) == napi_ok && value != NULL &&
                (drops = ta_napi_native_get(native, native->drops)) != NULL &&
                napi_get_array_length(env, drops, &length) == napi_ok)
                napi_set_element(env, drops, length, value);
            napi_close_handle_scope(env, scope);
        }
    }
    napi_delete_reference(env, reference);
}

/* ---- sessions' scopes ------------------------------------------------------- */

static uint32_t ta_napi_scope_slot(uint64_t handle) {
    return (uint32_t)(handle & 0xffffffffu) - 1u; /* ta_table: generation << 32 | slot + 1 */
}

napi_value ta_napi_session_scope(ta_napi_native *native, uint64_t handle) {
    napi_value scopes = ta_napi_native_get(native, native->scopes), scope = NULL;
    if (scopes == NULL || napi_get_element(native->host->env, scopes, ta_napi_scope_slot(handle), &scope) != napi_ok)
        return NULL;
    return scope; /* the row exists, so its element is set */
}

bool ta_napi_session_hold(ta_napi_native *native, uint64_t handle, napi_value scope) {
    napi_env env = native->host->env;
    napi_value scopes = ta_napi_native_get(native, native->scopes);
    if (scopes == NULL &&
        (napi_create_array(env, &scopes) != napi_ok || napi_create_reference(env, scopes, 1, &native->scopes) != napi_ok))
        return false;
    return napi_set_element(env, scopes, ta_napi_scope_slot(handle), scope) == napi_ok;
}

void ta_napi_session_let_go(ta_napi_native *native, uint64_t handle) {
    if (native->freeing) return; /* the array goes with the datamodel: basic-env calls only */
    napi_env env = native->host->env;
    napi_handle_scope frame;
    if (napi_open_handle_scope(env, &frame) != napi_ok) return;
    napi_value scopes = ta_napi_native_get(native, native->scopes), drops = ta_napi_native_get(native, native->drops), scope, undefined;
    uint32_t slot = ta_napi_scope_slot(handle), length = 0;
    if (scopes != NULL && napi_get_element(env, scopes, slot, &scope) == napi_ok) {
        if (drops != NULL && napi_get_array_length(env, drops, &length) == napi_ok) napi_set_element(env, drops, length, scope);
        if (napi_get_undefined(env, &undefined) == napi_ok) napi_set_element(env, scopes, slot, undefined);
    }
    napi_close_handle_scope(env, frame);
}

void ta_napi_native_flush(ta_napi_native *native) {
    if (native->drops == NULL || native->freeing) return;
    napi_env env = native->host->env;
    napi_handle_scope scope;
    if (napi_open_handle_scope(env, &scope) != napi_ok) return;
    napi_value drops = ta_napi_native_get(native, native->drops), zero, pending = NULL;
    uint32_t length = 0;
    bool thrown = false;
    /* an exception being thrown stays: a finalizer does not replace it */
    napi_is_exception_pending(env, &thrown);
    if (thrown) napi_get_and_clear_last_exception(env, &pending);
    if (drops != NULL && napi_get_array_length(env, drops, &length) == napi_ok && length > 0 &&
        napi_create_uint32(env, 0, &zero) == napi_ok)
        napi_set_named_property(env, drops, "length", zero);
    if (pending != NULL) napi_throw(env, pending);
    napi_close_handle_scope(env, scope);
}

void ta_napi_native_free(ta_napi_native *native) {
    napi_env env = native->host->env;
    napi_ref *references[] = {&native->shared, &native->scopes, &native->drops, &native->strings};
    for (size_t i = 0; i < sizeof references / sizeof references[0]; i++) {
        if (*references[i] != NULL) napi_delete_reference(env, *references[i]);
        *references[i] = NULL;
    }
    if (native->output.pointer != NULL) {
        const ta_allocator *allocator = &native->host->allocator;
        allocator->free(allocator->context, native->output.pointer, native->output.capacity);
    }
    native->output = (ta_buffer){NULL, 0, 0};
    ta_growable_arena_free(&native->scratch);
}

/* ---- payloads ---------------------------------------------------------------- */

ta_status ta_napi_native_add(ta_napi_native *native, napi_value value, ta_payload *out) {
    napi_env env = native->host->env;
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, value, &type) != napi_ok) return TA_ERROR_EXECUTION;
    bool boxed = type != napi_object && type != napi_function;
    napi_value held = value;
    if (boxed && (napi_create_array_with_length(env, 1, &held) != napi_ok || napi_set_element(env, held, 0, value) != napi_ok))
        return TA_ERROR_NO_MEMORY;
    napi_ref reference;
    if (napi_create_reference(env, held, 1, &reference) != napi_ok) return TA_ERROR_NO_MEMORY;
    uint64_t handle = 0;
    ta_napi_payload_row *row = (ta_napi_payload_row *)ta_table_add(&native->host->payloads, &handle);
    if (row == NULL) {
        napi_delete_reference(env, reference);
        return TA_ERROR_NO_MEMORY;
    }
    *row = (ta_napi_payload_row){reference, 1, boxed};
    *out = handle;
    return TA_OK;
}

napi_value ta_napi_native_value(ta_napi_native *native, ta_payload payload) {
    napi_env env = native->host->env;
    napi_value value = NULL;
    ta_napi_payload_row *row = payload ? (ta_napi_payload_row *)ta_table_get(&native->host->payloads, payload) : NULL;
    if (row != NULL && (value = ta_napi_native_get(native, row->value)) != NULL && row->boxed &&
        napi_get_element(env, value, 0, &value) != napi_ok)
        value = NULL;
    if (value == NULL) napi_get_undefined(env, &value);
    return value;
}

void ta_napi_native_retain(void *datamodel, ta_payload payload) {
    ta_napi_native *native = (ta_napi_native *)datamodel;
    ta_napi_payload_row *row = (ta_napi_payload_row *)ta_table_get(&native->host->payloads, payload);
    if (row != NULL) row->count++;
}

void ta_napi_native_release(void *datamodel, ta_payload payload) {
    ta_napi_native *native = (ta_napi_native *)datamodel;
    ta_napi_payload_row *row = (ta_napi_payload_row *)ta_table_get(&native->host->payloads, payload);
    if (row == NULL || --row->count > 0) return;
    napi_ref reference = row->value;
    ta_table_remove(&native->host->payloads, payload);
    ta_napi_native_drop(native, reference); /* later: it can run JavaScript */
}

bool ta_napi_native_payload(ta_napi_native *native, napi_value value, ta_payload *out) {
    napi_env env = native->host->env;
    napi_value shared = ta_napi_realm_shared(native), kit, copy;
    *out = 0;
    if (shared == NULL || napi_get_element(env, shared, TA_NAPI_REALM_KIT, &kit) != napi_ok) return false;
    copy = ta_napi_kit(env, kit, &(ta_napi_kit_call){"copy", 1, {value}});
    if (copy == NULL) { /* the kit's refusal, as a TinyactorsError of its status; another error as it is */
        napi_value error = NULL, refusal, message;
        char text[16];
        size_t length = 0;
        napi_get_and_clear_last_exception(env, &error);
        if (error == NULL || (refusal = ta_napi_kit(env, kit, &(ta_napi_kit_call){"refusal", 1, {error}})) == NULL ||
            napi_get_value_string_utf8(env, refusal, text, sizeof text, &length) != napi_ok) {
            napi_value ignored;
            napi_get_and_clear_last_exception(env, &ignored);
            if (error != NULL) napi_throw(env, error);
            return false;
        }
        ta_status status = length == 7 && memcmp(text, "invalid", 7) == 0 ? TA_ERROR_INVALID : TA_ERROR_UNSUPPORTED;
        ta_napi_room room = {&native->output, &native->host->allocator, {NULL, 0}};
        if (napi_get_named_property(env, error, "message", &message) != napi_ok || !ta_napi_text_kept(env, message, &room))
            return false;
        ta_napi_throw(env, &(ta_napi_refusal){status, room.view.pointer});
        return false;
    }
    ta_status status = ta_napi_native_add(native, copy, out);
    if (status != TA_OK) {
        ta_napi_throw(env, &(ta_napi_refusal){status, "no memory for the payload"});
        return false;
    }
    return true;
}

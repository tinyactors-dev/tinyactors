/* The addon @tinyactors/node: what its files share. Every name is ours,
 * ta_napi_* (AGENTS.md, "Names"); the platform's names appear only where
 * Node-API requires them (module.c's NAPI_MODULE_INIT).
 *
 * State lives in the environment's instance data (ta_napi_instance, one per
 * main thread or worker; AGENTS.md, "C rules": no mutable globals) or, from
 * the host step on, on the Host. */
#ifndef TA_NAPI_ADDON_H
#define TA_NAPI_ADDON_H

#include <node_api.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

/* What one environment holds, made at module init and freed at the
 * environment's teardown (napi_set_instance_data's finalizer).
 *
 *   allocator  malloc and free, each block reported to the engine with
 *              napi_adjust_external_memory (AGENTS.md, "The host's
 *              allocator"); its context is this instance;
 *   arena      the scratch of the module's own calls (encode): a mark at
 *              entry, a release at exit, so a nested call (a getter that
 *              calls encode) allocates above the outer one; its chunks are
 *              kept, so a warmed call allocates nothing;
 *   error      the class TinyactorsError, exported;
 *   object     Object.prototype of this environment's realm: a plain
 *              object's prototype (or null). */
typedef struct ta_napi_instance {
    napi_env env;
    ta_allocator allocator;
    ta_growable_arena arena;
    napi_ref error;
    napi_ref object;
} ta_napi_instance;

/* The environment's instance, or NULL (and an exception pending) before
 * module init finished. */
ta_napi_instance *ta_napi_instance_of(napi_env env);

/* A refusal: the message and the status core names (ta_status_name). */
typedef struct ta_napi_refusal {
    ta_status status;
    const char *message;
} ta_napi_refusal;

/* Throws a TinyactorsError with `message` and `status` set to
 * ta_status_name(refusal->status); returns NULL, for a napi_callback to
 * return. */
napi_value ta_napi_throw(napi_env env, const ta_napi_refusal *refusal);

/* ---- values.c: the value table (docs/values.md) ------------------------- */

/* Writes `value` into `writer` as the value table's CBOR. On a refusal a
 * TinyactorsError is thrown (or the exception a getter threw stays
 * pending) and the result is false; the writer is then the caller's to
 * free, its contents partial. Allocates the call's text in the instance's
 * arena: the caller holds a mark around the call. */
bool ta_napi_value_write(napi_env env, napi_value value, ta_cbor_writer *writer);

/* The value of one CBOR item filling `cbor`, or NULL with a TinyactorsError
 * thrown. The bytes are read during the call only. */
napi_value ta_napi_value_read(napi_env env, ta_bytes cbor);

/* The exports encode(value) and decode(bytes). */
napi_value ta_napi_encode(napi_env env, napi_callback_info info);
napi_value ta_napi_decode(napi_env env, napi_callback_info info);

#endif /* TA_NAPI_ADDON_H */

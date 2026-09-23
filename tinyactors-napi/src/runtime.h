#pragma once
/*
 * The Node-API runtime shared by hand-written and generated code (ADR 0013).
 *
 * Step 2 provides the per-environment record and error throwing. Step 3 adds
 * the primitive and (slot, generation) codecs, result -> TinyactorsError for
 * every core result, the reentrancy guard and handle wrapping.
 *
 * The environment record (tna_env) is created at module init and belongs to
 * one Node-API environment (ADR 0001: no global registry). It holds:
 * - a reference to the JavaScript TinyactorsError class, registered by
 *   lib/errors.js through the native `setup` function;
 * - the list of systems that are still open, so the environment cleanup hook
 *   can close them (ADR 0012);
 * - counters that tests read: systems alive and bytes leaked at close.
 *
 * Teardown order is not fixed across runtimes: the cleanup hook may run
 * before or after the finalizers of System wrappers. So the record is
 * reference-counted: every native System holds a reference, and the record is
 * freed once the cleanup hook has run and the last System is freed.
 */
#include <node_api.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct tna_napi_system tna_napi_system;

typedef struct tna_env {
    napi_ref error_class; /* TinyactorsError, or NULL before setup */
    tna_napi_system *open_systems; /* intrusive list through tna_napi_system */
    uint32_t references;           /* live native Systems */
    bool torn_down;                /* the cleanup hook has run */
    uint64_t systems_created;
    uint64_t systems_closed;
    uint64_t leaked_bytes;         /* summed over closed systems */
} tna_env;

/* Creates the record, stores it as instance data, and adds the cleanup hook. */
napi_status tna_env_init(napi_env env, tna_env **out);
tna_env *tna_env_get(napi_env env);
void tna_env_retain(tna_env *data);
/* Drops one System's reference; frees the record when nothing needs it. */
void tna_env_release(tna_env *data);

/*
 * Throws a TinyactorsError with `code` (a TinyactorsErrorCode string) and
 * `message`. Falls back to an Error with a `code` property before setup.
 * Returns NULL so a callback can `return tna_throw(...)`.
 */
napi_value tna_throw(napi_env env, const char *code, const char *message);
/* The TinyactorsErrorCode for a core result (step 3 generates the table). */
const char *tna_result_code(ta_result result);

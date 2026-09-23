#pragma once
/*
 * The JavaScript System class over the native system (ADR 0012).
 *
 * A native System wraps one tna_system. close() is explicit, idempotent and
 * authoritative; after it every method except close() and the `closed`
 * getter throws a TinyactorsError with code "closed". The wrapper's finalizer
 * is a safety net: it closes a system that JavaScript dropped without
 * closing. The environment cleanup hook (runtime.c) closes every system still
 * open when the environment goes away.
 */
#include <node_api.h>

#include "runtime.h"
#include "system.h"

struct tna_napi_system {
    const void *tag; /* &tna_napi_system_tag: unwrapped objects are checked */
    tna_system *system;
    tna_env *env_data;
    /* The environment's list of open systems. */
    tna_napi_system *previous;
    tna_napi_system *next;
    bool listed;
    /*
     * Handle objects made from this system (ADR 0013 point 5). Each one holds
     * the wrapper, so the wrapper and its system outlive the JavaScript System
     * until the last handle object is finalized.
     */
    tna_napi_handle *handles;
    uint32_t holders; /* handle objects, and a pending pump turn */
    bool finalized; /* the JavaScript System is gone */

    /* The pump (pump.h, ADR 0007). */
    napi_env env;
    napi_ref turn_function;       /* the native setImmediate callback */
    napi_ref timer_function;      /* the native setTimeout callback (ADR 0007 point 3) */
    napi_ref timeout;             /* the pending Timeout, for clearTimeout, or NULL */
    bool timer_held;              /* a pending timer holds the wrapper */
    napi_ref facade;              /* weak: the public System, for ActorId views */
    napi_ref done_hook;           /* ADR 0012 */
    napi_ref fault_hook;          /* ADR 0008 */
    napi_ref dead_letter_hook;    /* ADR 0008 */
    bool destroy_finished;        /* finished: "destroy" (the default) */
    napi_deferred *waiters;       /* flush() promises */
    uint32_t waiter_count;
    uint32_t waiter_capacity;
};

extern const char tna_napi_system_tag;

/* Defines the System class on `exports`. */
napi_status tna_napi_system_define(napi_env env, napi_value exports);
/* Closes every system still open in the environment (the cleanup hook). */
void tna_napi_system_close_open(tna_env *data);
/* The native System of a JavaScript System object, or NULL. */
tna_napi_system *tna_napi_system_unwrap(napi_env env, napi_value value);

/* Adds a handle to the system's list; the handle holds the system. */
void tna_napi_system_adopt(tna_napi_system *wrapper, tna_napi_handle *handle);
/* Removes a handle from its system's list and frees it. */
void tna_napi_system_drop(tna_napi_handle *handle);
/* Drops a hold (a handle or a pending turn); frees a finalized wrapper at zero. */
void tna_napi_system_unhold(tna_napi_system *wrapper);
/* Releases the handles whose objects were finalized while core ran. */
void tna_napi_system_flush(tna_napi_system *wrapper);

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
    tna_system *system;
    tna_env *env_data;
    /* The environment's list of open systems. */
    tna_napi_system *previous;
    tna_napi_system *next;
    bool listed;
};

/* Defines the System class on `exports`. */
napi_status tna_napi_system_define(napi_env env, napi_value exports);
/* Closes every system still open in the environment (the cleanup hook). */
void tna_napi_system_close_open(tna_env *data);

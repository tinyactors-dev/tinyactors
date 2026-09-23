#pragma once
/*
 * The pump (ADR 0007): the Node-API side of scheduling.
 *
 * - Turns are scheduled with the global setImmediate, called from native
 *   code with one native turn function per system. setImmediate callbacks
 *   run after the event loop has polled for I/O, so I/O and timer callbacks
 *   run between two turns and at most one turn runs per loop iteration. A
 *   pending immediate keeps the process alive, and nothing else does.
 * - The system asks for a turn through its wake callback (system.h), which
 *   the pump sets. A pending turn holds the native System wrapper, so the
 *   turn function never runs against freed memory.
 * - A turn runs the native scheduler (scheduler.h), then calls the hooks
 *   for the turn's records, after the last core call (ADR 0007 point 5):
 *   `done` with { actor, finalState, data } (ADR 0012), then destroys the
 *   actor when `finished` is "destroy"; `fault` with
 *   { actor, operation, code, message } (ADR 0008); `deadLetter` with
 *   { target, event, reason } for a message whose target is gone, finished,
 *   or refused it (ADR 0008; at most `messageBudget` per turn). An exception from a hook
 *   does not stop the turn: the first one is rethrown when the turn ends,
 *   so it surfaces as an uncaught exception from the setImmediate callback.
 * - flush() returns a native promise that settles at the end of the turn
 *   that leaves the system quiet. close() rejects the waiting ones.
 *
 * Handle scopes: the scheduler has no Node-API in it, so it opens no scope
 * around a resume. No JavaScript runs inside a resume until the JavaScript
 * data model (step 11), whose callbacks open their own scope.
 */
#include <node_api.h>
#include <stdbool.h>

#include "napi_system.h"

/*
 * Reads the scheduling options and hooks from `options` (an object, or
 * undefined) and installs the wake callback. Throws and returns false on bad
 * options.
 */
bool tna_pump_init(napi_env env, tna_napi_system *wrapper, napi_value facade, napi_value options);
/* Rejects waiting flush promises (when `env_usable`) and drops the pump's references. */
void tna_pump_close(tna_napi_system *wrapper, bool env_usable);
/* flush(): a promise that settles when the system is quiet. */
napi_value tna_pump_flush(napi_env env, tna_napi_system *wrapper);

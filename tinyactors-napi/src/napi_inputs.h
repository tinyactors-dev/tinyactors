#pragma once
/*
 * The input log (napi ADR 0032 section 6): the Node-API half.
 *
 * lib/system.js turns the log on with setInputLog(families) and records the
 * host's own operations with inputHost(). At the end of each turn, after
 * the services, the pump calls tna_inputs_dispatch(), which hands the
 * turn's records to the environment's effects handler as one batch:
 * handler(system, "inputs", { turn, horizon, dispatched, inputs }). lib/
 * commits it through the application's sink and releases the turn's held
 * effects afterwards.
 */
#include <node_api.h>

#include "napi_system.h"

/* Hands the turn's input records to JavaScript while the log is on. An
 * exception becomes the turn's first error. */
void tna_inputs_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error);
/* setInputLog(families | null). */
napi_value tna_set_input_log_method(napi_env env, napi_callback_info info);
/* inputHost(kind, slot, generation, value?, text?, family?, name?): a host operation. */
napi_value tna_input_host_method(napi_env env, napi_callback_info info);

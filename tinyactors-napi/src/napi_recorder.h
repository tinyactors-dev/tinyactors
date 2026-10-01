#pragma once
/*
 * The recorder (kernel ADR 0006 section 2): the Node-API half.
 *
 * lib/system.js turns recording on with setRecorder(families). After each
 * turn the pump calls tna_recorder_dispatch(), which hands the turn's
 * records and wakes to the environment's effects handler as one batch:
 * handler(system, "record", { records: [{ address, event, at }], wakes }),
 * where `event` is an ActorEvent. lib/ calls the application's recorder.
 */
#include <node_api.h>

#include "napi_system.h"

/* Hands the turn's records and wakes to JavaScript. An exception becomes the turn's first error. */
void tna_recorder_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error);
/* setRecorder(families | null). */
napi_value tna_set_recorder_method(napi_env env, napi_callback_info info);

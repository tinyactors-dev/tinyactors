#pragma once
/*
 * The operator's actions and the recorder's lifecycle calls (core ADRs 0082
 * and 0084, kernel ADR 0006): the Node-API half.
 */
#include <node_api.h>

#include "napi_system.h"

/* recorderEvicted(slot, generation, wake?). */
napi_value tna_recorder_evicted_method(napi_env env, napi_callback_info info);
/* recorderForget(slot, generation). */
napi_value tna_recorder_forget_method(napi_env env, napi_callback_info info);
/* withdrawSend(slot, generation, send ID as a bigint). */
napi_value tna_withdraw_send_method(napi_env env, napi_callback_info info);
/* abandon(slot, generation). */
napi_value tna_abandon_method(napi_env env, napi_callback_info info);

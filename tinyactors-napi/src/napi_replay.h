#pragma once
/*
 * Replaying events (kernel ADR 0006 section 3): the Node-API glue over the
 * host kernel's tah_replay_event(). A replaying actor is resumed by the
 * replay itself, synchronously, until it is between macrosteps: it crosses
 * no border (core ADR 0089 section 3), so nothing it does waits for a turn:
 * the kernel runs its inputs through ta_run()'s session filter.
 */
#include <node_api.h>

/* replayEvent(slot, generation, event, at): an ActorEvent replayed; { consumed, finished }. */
napi_value tna_replay_event_method(napi_env env, napi_callback_info info);
/* replayEventsDone(slot, generation): the bootstrap of an empty journal runs; the scheduler takes the actor back. */
napi_value tna_replay_events_done_method(napi_env env, napi_callback_info info);

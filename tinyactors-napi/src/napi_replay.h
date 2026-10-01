#pragma once
/*
 * Replay and checkpoints (napi ADR 0034 sections 3 and 4): the Node-API
 * glue over the host kernel's replay driver and checkpoints (kernel ADR
 * 0002 sections 6 and 7, replay.h).
 *
 * A replay feed is a kernel replay JavaScript holds: the log's bytes, as
 * JavaScript hands them over in chunks cut anywhere, and the messages the
 * log shows accepted and not delivered until they are put back after the
 * publish. A replaying actor is resumed by the replay itself,
 * synchronously, until it is between macrosteps: it crosses no border (core
 * ADR 0089 section 3), so nothing it does waits for a turn, and the
 * scheduler leaves it alone (TAH_SCHED_REPLAYING).
 */
#include <node_api.h>

/* replayFeed(): a new feed (an external). */
napi_value tna_replay_feed_method(napi_env env, napi_callback_info info);
/* replayPush(feed, bytes): appends a chunk. */
napi_value tna_replay_push_method(napi_env env, napi_callback_info info);
/* replayPrepare(feed, definition, incumbentSlot, incumbentGeneration, family?, name?):
 * a replaying actor from the log's first unit ({ slot, generation }), or
 * null while the unit is not all there. */
napi_value tna_replay_prepare_method(napi_env env, napi_callback_info info);
/* replayRun(feed, slot, generation, records, between?): applies up to `records` records:
 * { state: "more" | "need" | "finished" | "ended" | "life", records, ignored: [{ time, name }], lastTime, buffered }.
 * `between` (after "ended"): only host records, stopping at the next life's first ("life"). */
napi_value tna_replay_run_method(napi_env env, napi_callback_info info);
/* replayFinish(feed, slot, generation): the replay is over; the scheduler takes the actor back. */
napi_value tna_replay_finish_method(napi_env env, napi_callback_info info);
/* replayLastTime(feed): the time of the last record read, or undefined. */
napi_value tna_replay_last_time_method(napi_env env, napi_callback_info info);
/* replayRedeliver(feed, slot, generation): puts the owed messages back in the published actor's mailbox. */
napi_value tna_replay_redeliver_method(napi_env env, napi_callback_info info);
/* replayClose(feed): releases what the feed holds. */
napi_value tna_replay_close_method(napi_env env, napi_callback_info info);
/* checkpoint(slot, generation, write): writes the actor's checkpoint through write(chunk). */
napi_value tna_checkpoint_method(napi_env env, napi_callback_info info);
/* replayEvent(slot, generation, event, at): an ActorEvent replayed (kernel ADR 0006). */
napi_value tna_replay_event_method(napi_env env, napi_callback_info info);
/* replayEventsDone(slot, generation): the replaying actor goes back to the scheduler. */
napi_value tna_replay_events_done_method(napi_env env, napi_callback_info info);

#pragma once

#include "event.h"
#include "routing.h"

typedef enum {
    TA_EXEC_FRAME_ACTIONS = 0,
    TA_EXEC_FRAME_FOREACH
} ta_exec_frame_kind;

/*
 * One open block of executable content, or one running <foreach> (ADR 0060).
 * ACTIONS: `actions` is the block and `cursor` the next action in it.
 * FOREACH: `actions` is the body, `cursor` the <foreach> action itself, and
 * the snapshot, position and count are the loop's (ADR 0033).
 */
typedef struct {
    ta_exec_frame_kind kind;
    ta_span32 actions;
    uint32_t cursor;
    uint32_t iteration;
    uint32_t iteration_count;
    ta_value iteration_snapshot;
} ta_exec_frame;

/* Where a microstep stands (ADR 0061 section 2). */
typedef enum {
    TA_MICRO_NONE = 0,
    TA_MICRO_EXIT,
    TA_MICRO_TRANSITIONS,
    TA_MICRO_ENTER
} ta_micro_phase;

/* The stages of one state inside a phase. */
enum {
    TA_EXIT_HANDLERS = 0,
    TA_EXIT_HANDLERS_DONE = 1
};
enum {
    TA_ENTER_ACTIVATE = 0,
    TA_ENTER_HANDLERS = 1,
    TA_ENTER_INITIAL = 2,
    TA_ENTER_HISTORY = 3,
    TA_ENTER_DONE = 4
};

typedef struct {
    ta_micro_phase phase;
    /*
     * EXIT: the states still to exit are the exit set below this ID, highest
     * first. TRANSITIONS: the index of the enabled transition whose content
     * runs. ENTER: the next state to consider, lowest first.
     */
    uint32_t state;
    uint32_t stage;
    /* The <onentry>/<onexit> handler being run. */
    uint32_t handler;
    /* The executor's frames hold an open block that the current step resumes. */
    bool block_open;
    /* Where the microstep's event came from, for the TA_ERR_NOMEM path. */
    bool restore_internal;
    bool restore_external;
} ta_micro_position;

/*
 * A microstep paused inside executable content, moved out of the
 * interpreter's scratch into one allocation the actor owns (ADR 0061). The
 * header is followed, at the offsets the accessors compute, by the four
 * state sets (active, to exit, to enter, default entry; `words` each), the
 * default-history map (`state_count`), the enabled transitions, and the
 * executor's frames. It holds no pointers, so copying `bytes` copies it;
 * the frames' snapshots are values a copy must retain.
 */
typedef struct ta_continuation {
    size_t bytes;
    ta_micro_position at;
    uint32_t words;
    uint32_t state_count;
    uint32_t enabled_count;
    uint32_t frame_count;
} ta_continuation;

/* The bytes a continuation of this shape takes; `shape` has the counts set. */
size_t ta_continuation_bytes(const ta_continuation *shape);
uint64_t *ta_continuation_sets(ta_continuation *continuation);
ta_state_id *ta_continuation_history(ta_continuation *continuation);
ta_transition_id *ta_continuation_enabled(ta_continuation *continuation);
ta_exec_frame *ta_continuation_frames(ta_continuation *continuation);
/* The alignment every continuation allocation needs. */
#define TA_CONTINUATION_ALIGN _Alignof(ta_exec_frame)

/* Frees a continuation and releases its frames' snapshots; null is fine. */
void ta_continuation_free(ta_allocator allocator, ta_continuation **continuation);
/* Releases the snapshots of a continuation's frames, and nothing else. */
void ta_continuation_release(ta_continuation *continuation);
/* Retains the snapshots of a continuation's frames, for a copy of it. */
void ta_continuation_retain(ta_continuation *continuation);

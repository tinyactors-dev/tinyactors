/**
 * @file continuation.h
 *
 * # Continuations
 *
 * A `ta_continuation` is a run stopped inside a phase, moved out of the
 * interpreter's scratch into one allocation the actor owns, so that the
 * run resumes there later (ADR 0061, ADR 0070). A run stops inside a
 * phase when it pauses (between two units of executable content, or at a
 * microstep's start) or runs out of memory. The unit also defines the
 * executor's frames (`ta_exec_frame`), the phases a run can stand in
 * (`ta_run_phase`), and the position inside one (`ta_micro_position`).
 *
 * ## Layout
 *
 * One block, laid out with the layout builder (`layout.h`), in this order:
 *
 * - the `ta_continuation` header: the block's size, the
 *   `ta_micro_position` (the phase, the state, transition or cursor it is
 *   at, the stage, the handler or data declaration being run, and whether
 *   the frames hold an open block) and the counts below;
 * - the four state sets, `words` 64-bit words each, side by side: the
 *   live active set, the exit set, the entry set and the default-entry
 *   set, computed once as the microstep began;
 * - the data declarations the binding supplies, while bootstrapping
 *   (`decl_words` words);
 * - the default-history map (`state_count` state IDs);
 * - the enabled transitions (`enabled_count`), by compiled ID; the
 *   bootstrap's initial transition and an operator transition are marks,
 *   since nothing reads them after the microstep has begun but their
 *   executable content, which an operator transition does not have;
 * - the executor's frames (`frame_count`): open blocks with their
 *   cursors, and each running `<foreach>` with its snapshot, position and
 *   count (ADR 0033).
 *
 * `ta_continuation_bytes` sizes a block from a shape's counts, and the
 * accessors find each region by laying the block out again from the
 * counts it holds. `bytes` is the allocation, which may be larger than the
 * counts need: the system keeps one spare, sized for the largest
 * definition it ran. Every block is allocated aligned to
 * `TA_CONTINUATION_ALIGN`, the largest alignment of its regions.
 *
 * ## Invariants
 *
 * The counts a continuation holds are never larger than those its
 * allocation was sized for, so laying it out again cannot fail. It holds
 * no pointers: a copy is a copy of its `bytes` plus a retain of each
 * `<foreach>` snapshot (`ta_continuation_retain`).
 *
 * ## Memory
 *
 * The actor owns its continuation, through its sparse state, from the
 * stop until the run resumes; the system owns its spare. Both come from
 * the system's allocator. The interpreter reserves a continuation large
 * enough for the definition before a run starts, so stopping never
 * allocates (ADR 0070). `ta_continuation_free` releases the frames'
 * snapshots and frees the block. Nothing here allocates, so each function
 * may run inside a macrostep.
 *
 * ## Complexity
 *
 * `ta_continuation_bytes` and each accessor are O(1): a layout of six
 * regions. Retaining, releasing and freeing are O(frames).
 *
 * ## Failure
 *
 * `ta_continuation_bytes` answers `TA_ERR_NOMEM` for counts whose block
 * would exceed `SIZE_MAX`; the reservation passes that on before the run
 * does anything. Nothing else fails.
 *
 * ## Users
 *
 * The interpreter (stopping, resuming, inspection of where a run stands),
 * session images (which carry a copy) and the actor's sparse state.
 * ADRs 0033, 0060, 0061, 0070, 0082, 0083, 0086.
 */
#pragma once

#include "event.h"
#include "routing.h"
#include "span.h"
#include "../include/tinyactors.h"

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

/*
 * Where a run stands between two steps it can stop at (ADR 0061 section 2,
 * ADR 0070). NONE: nothing is open, and the actor holds all its state. The
 * microstep's phases are START, EXIT, TRANSITIONS and ENTER. START: its
 * transitions are selected, its sets computed and its history recorded,
 * and nothing of it has run; a run stops there only when its caller asked
 * (ADR 0086). The others are the stretches outside a microstep that run
 * chart code or start services, and so can run out of memory after
 * something is visible:
 *
 *   DATA      the bootstrap's data: declaring (stage 0, `handler` the next
 *             declaration), then initializing (stage 1, `state` 0 for the
 *             top-level data and s + 1 for state s's, `handler` the next
 *             declaration)
 *   SCRIPT    the bootstrap's top-level <script>; the initial microstep
 *             follows
 *   FINALIZE  an external event was taken: its <finalize> (stage 0), then
 *             autoforwarding (stage 1, `state` the next invocation)
 *   SELECT    the taken external event's transitions are still to be chosen
 *   INVOKES   starting the invocations of the states the macrostep entered:
 *             `state` the state, `handler` the next of its <invoke>s
 *   FINISH    a top-level <final> was entered: exiting every state (stage
 *             0, `state` as in EXIT), then ending the session (stage 1)
 */
typedef enum {
    TA_PHASE_NONE = 0,
    TA_PHASE_START,
    TA_PHASE_EXIT,
    TA_PHASE_TRANSITIONS,
    TA_PHASE_ENTER,
    TA_PHASE_DATA,
    TA_PHASE_SCRIPT,
    TA_PHASE_FINALIZE,
    TA_PHASE_SELECT,
    TA_PHASE_INVOKES,
    TA_PHASE_FINISH
} ta_run_phase;

/* The stages of one state inside a phase. */
enum {
    TA_EXIT_HANDLERS = 0,
    TA_EXIT_HANDLERS_DONE = 1
};
enum {
    TA_ENTER_ACTIVATE = 0,
    TA_ENTER_DATA = 1,
    TA_ENTER_HANDLERS = 2,
    TA_ENTER_INITIAL = 3,
    TA_ENTER_HISTORY = 4,
    TA_ENTER_DONE = 5
};
enum {
    TA_DATA_DECLARE = 0,
    TA_DATA_INITIALIZE = 1
};
enum {
    TA_FINALIZE_BLOCK = 0,
    TA_FINALIZE_FORWARD = 1
};
enum {
    TA_FINISH_EXIT = 0,
    TA_FINISH_END = 1
};

typedef struct {
    ta_run_phase phase;
    /*
     * EXIT: the states still to exit are the exit set below this ID, highest
     * first. TRANSITIONS: the index of the enabled transition whose content
     * runs. ENTER: the next state to consider, lowest first.
     */
    uint32_t state;
    uint32_t stage;
    /* The <onentry>/<onexit> handler being run; in ENTER's DATA stage, the
     * next data declaration to initialize. */
    uint32_t handler;
    /* The executor's frames hold an open block that the current step resumes. */
    bool block_open;
} ta_micro_position;

/*
 * The header of a stopped run's block; the regions after it are listed in
 * the file comment, at the offsets the accessors compute. `bytes` is the
 * allocation, which may be larger than these counts need.
 */
typedef struct ta_continuation {
    size_t bytes;
    ta_micro_position at;
    uint32_t words;
    uint32_t decl_words;
    uint32_t state_count;
    uint32_t enabled_count;
    uint32_t frame_count;
} ta_continuation;

/**
 * The bytes a continuation of this shape takes, in `*out`; `shape` has the
 * counts set and is only read. Never allocates; may run anywhere. Answers
 * TA_ERR_NOMEM, leaving `*out` alone, when they would exceed SIZE_MAX.
 */
ta_result ta_continuation_bytes(const ta_continuation *shape, size_t *out);
/* Whether the phase is one of a microstep's (START, EXIT, TRANSITIONS, ENTER). */
bool ta_run_phase_in_microstep(ta_run_phase phase);
uint64_t *ta_continuation_sets(ta_continuation *continuation);
uint64_t *ta_continuation_decls(ta_continuation *continuation);
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

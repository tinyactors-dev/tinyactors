#pragma once

#include "layout.h"
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
 * Where the arrays that follow a continuation's header start, in bytes from
 * the start of the allocation. The state sets come first and start at
 * sizeof(ta_continuation), so they need no field (see the static assertion in
 * continuation.c). The offsets are 32-bit: ta_continuation_bytes refuses a
 * shape whose end does not fit, which keeps the header at 64 bytes where
 * size_t offsets would make it 88.
 */
typedef struct ta_continuation_offsets {
    uint32_t decls;
    uint32_t history;
    uint32_t enabled;
    uint32_t frames;
} ta_continuation_offsets;

/*
 * A run stopped inside a phase, by a pause (ADR 0061) or by running out of
 * memory (ADR 0070), moved out of the interpreter's scratch into one
 * allocation the actor owns. The header is followed, at the offsets it
 * stores, by the four state sets (active, to exit, to enter, default entry;
 * `words` each), the data declarations the binding supplies (`decl_words`,
 * while bootstrapping), the default-history map (`state_count`), the enabled
 * transitions, and the executor's frames. `bytes` is the allocation, which
 * may be larger than this shape needs: the system keeps one spare, sized for
 * the largest definition it ran. It holds no pointers, so copying `bytes`
 * copies it; the frames' snapshots are values a copy must retain.
 *
 * Lifetime: parked. The actor owns its paused continuation, and the system
 * owns the one spare; either frees it with `bytes` and TA_CONTINUATION_ALIGN.
 *
 * `offsets` are written once, where the allocation is made or loaded
 * (ta_reserve_stop, and the restore of an image), from the CAPACITY shape of
 * the definition (ta_continuation_capacity in interpreter.c): every array
 * sits in a slot sized for the most the definition can save. A stop writes
 * fewer enabled transitions and frames than the slots hold (`enabled_count`,
 * `frame_count`), never more, so each array stays inside its slot and no two
 * overlap. `words`, `decl_words` and `state_count` always equal the capacity
 * values. The accessors read the offsets and nothing can fail after the
 * allocation. The offsets are valid for one definition's shape: a spare that
 * is reused for another definition gets them rewritten, and an actor's
 * paused continuation keeps them because an actor's definition does not
 * change while it is paused.
 */
typedef struct ta_continuation {
    size_t bytes;
    ta_micro_position at;
    uint32_t words;
    uint32_t decl_words;
    uint32_t state_count;
    uint32_t enabled_count;
    uint32_t frame_count;
    ta_continuation_offsets offsets;
} ta_continuation;

/*
 * ta_continuation_bytes answers, in `*bytes`, the bytes a continuation of
 * this shape takes (`shape` has the counts set) and, in `*offsets` when it
 * is not NULL, where each array goes. It answers TA_ERR_NOMEM and writes
 * neither output when the shape does not fit in a size_t, or when its end
 * exceeds UINT32_MAX (every offset is at most the end, so one bound covers
 * them all). It never allocates. It is called only where a continuation is
 * allocated or loaded, which is a safe point that can fail cleanly; what is
 * needed afterwards is stored in the header.
 */
ta_result ta_continuation_bytes(const ta_continuation *shape, size_t *bytes, ta_continuation_offsets *offsets);
/* Whether the phase is one of a microstep's (START, EXIT, TRANSITIONS, ENTER). */
bool ta_run_phase_in_microstep(ta_run_phase phase);
/*
 * The accessors answer where each array of an existing continuation starts:
 * the allocation's address plus the stored offset. They read the header and
 * never fail or allocate. The pointers are valid until the continuation is
 * freed or recycled, and each array holds at most the count its name says
 * (`words` per set, `decl_words`, `state_count`, `enabled_count`,
 * `frame_count`).
 */
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

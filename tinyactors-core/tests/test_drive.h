/**
 * @file test_drive.h
 *
 * # Driving an actor in a test
 *
 * The one way core's test programs make an actor run: run what it holds
 * (its bootstrap after spawn, or the event delivered to it), or deliver an
 * external event and then run it. Every test that only needs an actor to
 * run, as a host would make it, calls these functions instead of core's
 * delivery and run calls in sequence, so how a host runs an actor is
 * written down here once.
 *
 * It lives with the tests and is never compiled into the library. Every
 * test program is one translation unit, so the helper is a header of
 * static inline functions; it includes the public header itself, and needs
 * nothing else, so a test linked against the library and one that
 * includes the amalgamation use it alike.
 *
 * ## Use
 *
 * ```c
 * test_drive_status status = TEST_DRIVE_ERROR;
 * CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
 * CHECK(status == TEST_DRIVE_STABLE);
 * CHECK(test_drive_deliver(system,
 *                          &(ta_actor_deliver_params){ .name = test_event_text(system, &event), .time = ta_system_time(system), .actor = actor,
 *                                                      .event = &event },
 *                          &(test_drive_run_params){ .status = &status }) == TA_OK);
 * ```
 *
 * ## Contract
 *
 * - `test_drive_run` is one run of the actor: one `ta_run()` call with the
 *   actor as its session filter and room for one outcome, so it runs what
 *   the actor holds (its bootstrap, the input admitted for it, or the
 *   macrostep a limit paused) until that input's macrostep ends, or until a
 *   limit in `test_drive_run_params` pauses it, where the next run
 *   continues. With `release`, a session a fault stopped is released first
 *   (`ta_actor_release()`), as a host that lets it go on does; without it,
 *   such a session's run finds nothing to run. Its result
 *   and its status are the run's own: the helper neither retries nor
 *   loops, so a test that needs several runs, or that checks the actor
 *   between pauses, calls it once per run.
 * - The status is the input's outcome: stable (COMPLETED or IGNORED), done
 *   (FINISHED), paused (the call paused inside the macrostep), or error (a
 *   FAULTED or REFUSED outcome, whose code the run answers, or a ta_run()
 *   that failed). A run that finds nothing to run is stable, or done for a
 *   finished actor, and answers TA_ERR_NOT_FOUND when there is no such
 *   actor.
 * - `test_drive_deliver` admits the event and, when the actor accepts it,
 *   runs the actor once as `test_drive_run` does. It answers the
 *   delivery's result when the actor refuses the event, and then runs
 *   nothing and leaves `status` and `ignored` untouched; otherwise it
 *   answers the run's result.
 * - A null `test_drive_run_params` is all defaults: the budget
 *   `TEST_DRIVE_BUDGET`, no other limit, no release, and no status
 *   reported.
 *
 * A test whose point is the exact result of admission or of a run (a full
 * pool, a refusal, running out of memory, a runaway) calls core directly
 * where it asserts that result.
 *
 * ## Memory
 *
 * The helper owns nothing and allocates nothing itself; what the calls
 * allocate is core's, from the system's allocator, as for any host. The
 * delivery parameters and the event are borrowed for the call, as core's
 * delivery borrows them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../include/tinyactors.h"

/** The number of microsteps a run may take when the test sets none. */
#define TEST_DRIVE_BUDGET 100u

/** How one run of an actor ended; see the file comment. */
typedef enum {
    TEST_DRIVE_STABLE = 0,
    TEST_DRIVE_DONE = 1,
    TEST_DRIVE_PAUSED = 2,
    TEST_DRIVE_ERROR = 3
} test_drive_status;

/**
 * How far one run may go, and where it reports. Zeroed, it is the
 * defaults: `TEST_DRIVE_BUDGET` microsteps, no other limit, no release,
 * nothing reported.
 */
typedef struct {
    /* Microsteps the run may take before it pauses; 0 is TEST_DRIVE_BUDGET. */
    uint32_t budget;
    /* Units of executable content the run may take before it pauses inside
     * a microstep; 0 is no such limit. */
    uint32_t action_budget;
    /* Pauses before the first unit of every microstep the run begins. */
    bool pause_at_microstep_start;
    /* Releases the session first when a fault stopped it
     * (`ta_actor_release()`), so the run continues its input. */
    bool release;
    /* When not null, receives how the run ended: stable, done, paused or
     * error. */
    test_drive_status *status;
    /* When not null, set when the run ends a macrostep whose external event
     * enabled no transition, cleared when it ends one that did; untouched
     * when it ends none. */
    bool *ignored;
} test_drive_run_params;

/**
 * Runs `actor` once: what it holds, until its input is done or a limit in
 * `run` (null for the defaults) pauses it. Answers the run's result: TA_OK
 * for a stable, done or paused run, or the error that stopped it.
 */
static inline ta_result test_drive_run(ta_system *system, ta_actor_id actor, const test_drive_run_params *run) {
    test_drive_run_params limits = run != NULL ? *run : (test_drive_run_params){0};
    test_drive_status ended = TEST_DRIVE_ERROR;
    test_drive_status *status = limits.status != NULL ? limits.status : &ended;
    if (limits.release) {
        (void)ta_actor_release(system, actor); /* a session a fault stopped goes on; any other answers no */
    }
    ta_input_outcome outcome = { .kind = TA_INPUT_TAKEN };
    uint32_t count = 0;
    ta_run_status run_status = TA_RUN_IDLE;
    ta_result result = ta_run(system, &(ta_run_params){
                                          .budget = limits.budget != 0 ? limits.budget : TEST_DRIVE_BUDGET,
                                          .action_budget = limits.action_budget,
                                          .pause_at_microstep_start = limits.pause_at_microstep_start,
                                          .session = actor,
                                          .outcomes = &outcome,
                                          .outcome_capacity = 1,
                                          .outcome_count = &count,
                                          .status = &run_status,
                                      });
    if (result != TA_OK) {
        *status = TEST_DRIVE_ERROR;
        return result;
    }
    if (count == 0) {
        if (run_status == TA_RUN_PAUSED) {
            *status = TEST_DRIVE_PAUSED;
            return TA_OK;
        }
        ta_actor_inspection view;
        result = ta_actor_inspect(system, actor, &view);
        *status = result != TA_OK                         ? TEST_DRIVE_ERROR
                  : view.residency == TA_ACTOR_DONE ? TEST_DRIVE_DONE
                                                          : TEST_DRIVE_STABLE;
        return result;
    }
    switch (outcome.kind) {
    case TA_INPUT_COMPLETED:
    case TA_INPUT_IGNORED:
        *status = TEST_DRIVE_STABLE;
        break;
    case TA_INPUT_FINISHED:
        *status = TEST_DRIVE_DONE;
        break;
    default:
        *status = TEST_DRIVE_ERROR;
        return outcome.code;
    }
    if (limits.ignored != NULL) {
        *limits.ignored = outcome.kind == TA_INPUT_IGNORED;
    }
    return TA_OK;
}

/**
 * Delivers `delivery`'s event to its actor and, when the actor accepts it,
 * runs that actor once with `run` (null for the defaults). Answers the
 * delivery's result when it is not TA_OK, having run nothing; otherwise
 * the run's.
 */
/**
 * The dotted text of `event`'s name, which a test built by ID, for
 * ta_actor_deliver_params.name: formatted by core into one of four
 * buffers used in turn, so a call's text stays valid for the next three
 * calls. Empty when the name has no text.
 */
static inline ta_str test_event_text(const ta_system *system, const ta_event *event) {
    static char buffers[4][512];
    static uint32_t next = 0;
    char *buffer = buffers[next++ % 4u];
    uint32_t needed = 0;
    if (ta_event_name_format(system, &(ta_event_name_format_params){ .name = event->name, .buffer = buffer,
                                                                     .capacity = 512u, .needed = &needed }) != TA_OK) {
        return (ta_str){ 0 };
    }
    return (ta_str){ .ptr = buffer, .len = needed - 1u };
}

static inline ta_result test_drive_deliver(ta_system *system, const ta_actor_deliver_params *delivery,
                                           const test_drive_run_params *run) {
    ta_result result = ta_actor_deliver(system, delivery);
    if (result != TA_OK) {
        return result;
    }
    return test_drive_run(system, delivery->actor, run);
}

/*
 * The earliest due time of a pending delayed send, read the way a host reads
 * it: a ta_system_set_time() to the current time with no budget, which
 * fires nothing and reports the wake. TA_ERR_NOT_FOUND when none is pending.
 */
static inline ta_result test_next_deadline(ta_system *system, ta_time *out) {
    bool pending = false;
    ta_result result = ta_system_set_time(system, &(ta_set_time_params){ .now = ta_system_time(system),
                                                                          .has_wake = &pending,
                                                                          .wake = out });
    return result != TA_OK ? result : pending ? TA_OK : TA_ERR_NOT_FOUND;
}

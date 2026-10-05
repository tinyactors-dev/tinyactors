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
 * ta_run_status status = TA_RUN_ERROR;
 * CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
 * CHECK(status == TA_RUN_STABLE);
 * CHECK(test_drive_deliver(system,
 *                          &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
 *                                                      .event = &event },
 *                          &(test_drive_run_params){ .status = &status }) == TA_OK);
 * ```
 *
 * ## Contract
 *
 * - `test_drive_run` is one run of the actor: it runs what the actor
 *   holds until the actor is idle (stable or done), or until a limit in
 *   `test_drive_run_params` stops it, paused, where the next run
 *   continues. Its result and its status are the run's own: the helper
 *   neither retries nor loops, so a test that needs several runs, or that
 *   checks the actor between pauses, calls it once per run.
 * - `test_drive_deliver` offers the event and, when the actor accepts it,
 *   runs the actor once as `test_drive_run` does. It answers the
 *   delivery's result when the actor refuses the event, and then runs
 *   nothing and leaves `status` and `ignored` untouched; otherwise it
 *   answers the run's result.
 * - A null `test_drive_run_params` is all defaults: the budget
 *   `TEST_DRIVE_BUDGET`, no other limit, and no status reported.
 *
 * A test whose point is the exact result of delivery or of a run (a busy
 * actor, a refusal, running out of memory, a runaway) calls core
 * directly where it asserts that result.
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

/**
 * How far one run may go, and where it reports. Zeroed, it is the
 * defaults: `TEST_DRIVE_BUDGET` microsteps, no other limit, nothing
 * reported.
 */
typedef struct {
    /* Microsteps the run may take before it pauses; 0 is TEST_DRIVE_BUDGET. */
    uint32_t budget;
    /* Units of executable content the run may take before it pauses inside
     * a microstep; 0 is no such limit. */
    uint32_t action_budget;
    /* Lifts the microstep limit for this run, as for an actor an operator
     * released after a runaway. */
    bool no_microstep_limit;
    /* Pauses before the first unit of every microstep the run begins. */
    bool pause_at_microstep_start;
    /* When not null, receives how the run ended: stable, done, paused or
     * error. */
    ta_run_status *status;
    /* When not null, set when the run ends a macrostep whose external event
     * enabled no transition, cleared when it ends one that did; untouched
     * when it ends none. */
    bool *ignored;
} test_drive_run_params;

/**
 * Runs `actor` once: what it holds, until it is idle or a limit in `run`
 * (null for the defaults) pauses it. Answers the run's result: TA_OK
 * whatever the status, or the error that stopped it.
 */
static inline ta_result test_drive_run(ta_system *system, ta_actor_id actor, const test_drive_run_params *run) {
    test_drive_run_params limits = run != NULL ? *run : (test_drive_run_params){0};
    ta_run_status status = TA_RUN_ERROR;
    return ta_actor_resume(system, &(ta_actor_resume_params){
                                                   .actor = actor,
                                                   .budget = limits.budget != 0 ? limits.budget : TEST_DRIVE_BUDGET,
                                                   .status = limits.status != NULL ? limits.status : &status,
                                                   .action_budget = limits.action_budget,
                                                   .no_microstep_limit = limits.no_microstep_limit,
                                                   .pause_at_microstep_start = limits.pause_at_microstep_start,
                                                   .ignored = limits.ignored,
                                               });
}

/**
 * Delivers `delivery`'s event to its actor and, when the actor accepts it,
 * runs that actor once with `run` (null for the defaults). Answers the
 * delivery's result when it is not TA_OK, having run nothing; otherwise
 * the run's.
 */
static inline ta_result test_drive_deliver(ta_system *system, const ta_actor_deliver_params *delivery,
                                           const test_drive_run_params *run) {
    ta_result result = ta_actor_deliver(system, delivery);
    if (result != TA_OK) {
        return result;
    }
    return test_drive_run(system, delivery->actor, run);
}

/*
 * Package run_support is the part of the tests that stands between a test's
 * "run this actor" and core's ta_run (ADR 0099 section 7). It is test
 * infrastructure, not a block of the library: it lives with the tests, is
 * header-only (static inline), includes only "tinyactors.h", and nothing in
 * src/ refers to it.
 *
 * What it is for. Before ADR 0099 a test delivered an event to an actor and
 * resumed that actor until its macrostep ended, and read how it ended
 * (stable, paused, done) and whether the event enabled nothing. ta_run runs
 * the whole system's admitted inputs, in the order they were admitted, and
 * reports how each ended in notices. About twenty-five test programs ask the
 * old question, so this adapter answers it with ta_run, and each program
 * changes a call and a type name rather than its structure.
 *
 * test_resume runs ta_run with the budgets given (the microstep budget is
 * for the whole call, as ta_run's is), keeps calling it while the notice
 * array is full, and answers the outcome of the macrostep as the old status
 * did: PAUSED when a budget stopped a macrostep, DONE when an input ended
 * with the session finished, RUNAWAY when one reached the microstep limit and
 * was abandoned, else STABLE. `.actor` is accepted for the old call sites and
 * is not used: ta_run takes the next input whoever it is for. A test with two
 * actors in flight must say in a comment how it orders them.
 *
 * Lifetime: call. It keeps no state between calls. It allocates nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../include/tinyactors.h"

typedef enum {
    TEST_RUN_STABLE = 0,
    TEST_RUN_PAUSED = 1,
    TEST_RUN_DONE = 2,
    TEST_RUN_ERROR = 3,
    TEST_RUN_RUNAWAY = 4
} test_run_status;

typedef struct {
    ta_actor_id actor; /* unused: see above */
    uint32_t budget;
    test_run_status *status;
    uint32_t action_budget;
    bool pause_at_microstep_start;
    bool *ignored;
} test_resume_params;

/*
 * test_resume: see the package comment. TA_ERR_INVALID for a null status or
 * no budget, as ta_run answers; TA_ERR_NOMEM leaves the input where it stands.
 */
static inline ta_result test_resume(ta_system *system, const test_resume_params *params) {
    ta_result result = TA_OK;
    ta_run_status stopped = TA_RUN_IDLE;
    test_run_status outcome = TEST_RUN_STABLE;
    bool ignored = false;
    if (params->status == NULL || params->budget == 0) {
        return TA_ERR_INVALID;
    }
    do {
        ta_run_notice notices[16];
        uint32_t count = 0;
        result = ta_run(system, &(ta_run_params){ .budget = params->budget,
                                                  .action_budget = params->action_budget,
                                                  .pause_at_microstep_start = params->pause_at_microstep_start,
                                                  .status = &stopped,
                                                  .notices = notices,
                                                  .notice_capacity = 16,
                                                  .notice_count = &count });
        for (uint32_t index = 0; index < count; index += 1u) {
            if (notices[index].kind != TA_NOTICE_INPUT_COMPLETED) {
                continue;
            }
            ignored = ignored || notices[index].ignored;
            if (notices[index].status == TA_INPUT_DONE) {
                outcome = TEST_RUN_DONE;
            } else if (notices[index].status == TA_INPUT_RUNAWAY) {
                outcome = TEST_RUN_RUNAWAY;
            }
        }
    } while (result == TA_OK && stopped == TA_RUN_NOTICES_FULL);
    if (result != TA_OK) {
        *params->status = TEST_RUN_ERROR;
        return result;
    }
    *params->status = stopped == TA_RUN_PAUSED ? TEST_RUN_PAUSED : outcome;
    if (params->ignored != NULL && *params->status != TEST_RUN_PAUSED) {
        *params->ignored = ignored;
    }
    return TA_OK;
}

/*
 * test_run_input runs at most one input and stops before the next is taken:
 * ta_run stops between inputs when the caller's array cannot hold another
 * input's notices (three), so an array of three holds one input's COMPLETED
 * notice and no more. A test that needs to act between two inputs (mail that a
 * macrostep sent and nobody has taken yet) uses it. A started macrostep (a
 * bootstrap) counts as an input. Answers ta_run's result.
 */
static inline ta_result test_run_input(ta_system *system) {
    ta_run_status stopped = TA_RUN_IDLE;
    ta_run_notice notices[3];
    uint32_t count = 0;
    return ta_run(system, &(ta_run_params){ .budget = 1000000u,
                                            .status = &stopped,
                                            .notices = notices,
                                            .notice_capacity = 3,
                                            .notice_count = &count });
}

/*
 * test_run_idle runs the system until nothing is runnable or paused, with a
 * budget no test reaches, and answers the last result. A test that only needs
 * "everything admitted has run" calls this.
 */
static inline ta_result test_run_idle(ta_system *system) {
    ta_result result = TA_OK;
    ta_run_status stopped = TA_RUN_MORE;
    while (result == TA_OK && stopped != TA_RUN_IDLE) {
        ta_run_notice notices[16];
        uint32_t count = 0;
        result = ta_run(system, &(ta_run_params){ .budget = 1000000u,
                                                  .status = &stopped,
                                                  .notices = notices,
                                                  .notice_capacity = 16,
                                                  .notice_count = &count });
    }
    return result;
}

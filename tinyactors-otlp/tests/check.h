#ifndef TAO_CHECK_H
#define TAO_CHECK_H

/**
 * @file
 * What tinyactors-otlp's tests share: a minimal assertion, an allocator
 * that counts what is in use and can refuse large allocations, and one
 * ta_run() call for one actor. The tests may use the host's C library.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tinyactors.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

/*
 * malloc and free as a ta_allocator, counting the bytes in use. From
 * check_refuse_from(), every allocation of at least that many bytes is
 * refused, as a full pool would: a size, not a count, names the allocation
 * to fail, so a test can fail one large allocation without depending on
 * how many small ones come before it.
 */
typedef struct {
    size_t bytes_in_use;
    size_t refuse_from; /* 0: refuse none */
} check_allocator;

static inline void *check_alloc(void *self, size_t size, size_t alignment) {
    check_allocator *allocator = self;
    if (allocator->refuse_from != 0 && size >= allocator->refuse_from) {
        return NULL;
    }
    void *ptr = NULL;
    if (alignment <= _Alignof(max_align_t)) {
        ptr = malloc(size);
    } else {
        ptr = aligned_alloc(alignment, (size + alignment - 1u) & ~(alignment - 1u));
    }
    if (ptr != NULL) {
        allocator->bytes_in_use += size;
    }
    return ptr;
}

static inline void check_free(void *self, const ta_allocator_free_params *params) {
    check_allocator *allocator = self;
    if (params->ptr != NULL) {
        allocator->bytes_in_use -= params->size;
    }
    free(params->ptr);
}

static const ta_allocator_vtable check_allocator_vtable = {
    .alloc = check_alloc,
    .free = check_free,
};

static inline ta_allocator check_allocator_of(check_allocator *allocator) {
    return (ta_allocator){ .self = allocator, .v = &check_allocator_vtable };
}

static inline void check_refuse_from(check_allocator *allocator, size_t bytes) {
    allocator->refuse_from = bytes;
}

/* How one run of a session's input ended (check_run). */
typedef enum { CHECK_RUN_STABLE, CHECK_RUN_DONE, CHECK_RUN_PAUSED, CHECK_RUN_ERROR } check_run_status;

/*
 * One ta_run() call for `actor` alone (its session filter), with room for
 * one outcome: it runs the actor's next input, or the macrostep a budget
 * paused, until it ends or `budget` microsteps pause it. Stable for an
 * input that completed or was ignored, or when nothing waited; done when
 * the actor finished; error for a fault, a refusal or a failed call.
 */
static inline check_run_status check_run(ta_system *core, ta_actor_id actor, uint32_t budget) {
    ta_input_outcome outcome = { .kind = TA_INPUT_TAKEN };
    uint32_t count = 0;
    ta_run_status status = TA_RUN_IDLE;
    ta_result result = ta_run(core, &(ta_run_params){ .budget = budget, .session = actor, .outcomes = &outcome,
                                                      .outcome_capacity = 1, .outcome_count = &count,
                                                      .status = &status });
    if (result != TA_OK) {
        return CHECK_RUN_ERROR;
    }
    if (count == 0) {
        return status == TA_RUN_PAUSED ? CHECK_RUN_PAUSED : CHECK_RUN_STABLE;
    }
    switch (outcome.kind) {
    case TA_INPUT_COMPLETED:
    case TA_INPUT_IGNORED:
        return CHECK_RUN_STABLE;
    case TA_INPUT_FINISHED:
        return CHECK_RUN_DONE;
    default:
        return CHECK_RUN_ERROR;
    }
}

#endif

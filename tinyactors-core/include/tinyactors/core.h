/* tinyactors/core.h — ADVANCED / UNSTABLE: the bare SCXML execution kernel.
 *
 * Most hosts should use tinyactors/system.h (a single interpreter is a system with
 * max_actors = 1). This header exists for embedders that want to drive one
 * instance themselves and own routing/timers.
 *
 * The kernel is a pure resumable step function: it never routes events,
 * touches other instances, or reads a clock. Side effects that cross the
 * instance boundary are *returned* as ta_step results; the caller performs
 * them and calls ta_core_step again.
 */
#ifndef TINYACTORS_CORE_H
#define TINYACTORS_CORE_H

#include <tinyactors/datamodel.h>
#include <tinyactors/image.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_core ta_core; /* kernel context: image, datamodel, event pool, workspace pool */
typedef struct ta_instance ta_instance; /* chart-dependent mutable instance */

enum {
    TA_STEP_YIELD = 0,          /* fuel exhausted; state preserved */
    TA_STEP_STABLE = 1,         /* macrostep complete; deliver an external event */
    TA_STEP_SEND = 2,           /* ta_core_out describes a send */
    TA_STEP_CANCEL = 3,         /* cancel delayed send */
    TA_STEP_INVOKE = 4,         /* start invoke */
    TA_STEP_UNINVOKE = 5,       /* cancel invoke */
    TA_STEP_REQUEST = 6,        /* external response needed */
    TA_STEP_NEED_WORKSPACE = 7, /* no free workspace */
    TA_STEP_DONE = 8,           /* top-level final reached */
    TA_STEP_FAULT = 9
};

TA_API size_t ta_core_instance_size(const ta_image *image, uint32_t chart);
TA_API size_t ta_core_workspace_size(const ta_image *image, uint32_t chart);

TA_API uint32_t ta_core_step(ta_core *core, ta_instance *instance, uint32_t *fuel);
TA_API ta_status ta_core_deliver(ta_core *core, ta_instance *instance, uint32_t event);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_CORE_H */

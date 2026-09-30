#pragma once
/*
 * The kernel's own layout of what tinyactors_host/host_metrics.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/host_metrics.h"

struct tah_host_metrics {
    tah_histogram turn_seconds;     /* one pump turn, wall time, hooks and services included */
    tah_histogram lateness_seconds; /* a delayed send's dispatch time minus its due time */
    tah_histogram load_seconds;     /* a loader call, from the first message to its result (napi ADR 0020) */
    /* The trace and span of the latest sampled macrostep that began in the
     * running turn: that turn's exemplar. The trace encoder sets it. */
    tah_exemplar turn_exemplar;
};

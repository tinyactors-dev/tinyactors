#pragma once
/*
 * The kernel's own layout of what tinyactors_host/system.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/system.h"
#include "allocator_private.h"
#include "definitions_private.h"
#include "directory_private.h"
#include "effects_private.h"
#include "host_metrics_private.h"
#include "loads_private.h"
#include "offloads_private.h"
#include "otlp_private.h"
#include "recorder_private.h"
#include "scheduler_private.h"
#include "watch_private.h"

typedef struct tah_clock {
    tah_clock_kind kind;
    ta_time initial;       /* core's time at creation */
    uint64_t origin;       /* realtime: `now` at creation, in nanoseconds */
    tah_now_fn now;        /* realtime: the monotonic clock (a test may replace it) */
    void *now_context;
    bool armed;            /* a host timer is pending */
    ta_time armed_deadline;
} tah_clock;

struct tah_system {
    /* What the binding gives the kernel from the machine (tinyactors_host.h). */
    tah_platform platform;
    tah_allocator allocator;
    tah_scratch scratch;
    ta_system *core;
    tah_sched sched;
    tah_directory directory; /* installed on core at creation (ADR 0006) */
    /* Loads of unloaded actors, and their wake-ups (loads.h, napi ADR 0020). */
    tah_loads loads;
    /* Actors offloaded to bytes while unloaded (core ADR 0074). */
    tah_offloads offloads;
    tah_turn_budgets budgets;
    uint32_t due_budget;   /* due sends dispatched in one turn, at least 1 */
    tah_clock clock;
    tah_pump_driver driver;
    bool turn_pending; /* wake was called and the turn has not run yet */
    bool closed;
    /*
     * Set around every core call (ADR 0013 point 4). A generated entry point
     * that finds it set throws "reentrant": data-model, I/O and invoker
     * callbacks may run JavaScript, but that JavaScript cannot call core.
     */
    bool inside_core;
    /* Bytes still in use when close finished; nonzero means a leak. */
    size_t leaked_bytes;
    tah_system_callbacks callbacks;
    /* Effects waiting for JavaScript (effects.h), in the turn arena. */
    tah_effects effects;
    /* The trace encoder (otlp.h), while a sink is installed (napi ADR 0016):
     * core's records become OTLP spans as they are written, and the pump
     * hands the spans that ended to JavaScript as bytes, once per turn. */
    tah_otlp otlp;
    bool tracing;
    /* <log> records go to JavaScript's log sink (setLogSink). */
    bool log_to_js;
    /* What is watched (napi ADR 0017). */
    tah_trace_watch trace_watch;
    /* The definition lifecycle as core tells it (core ADR 0079): counts and origins. */
    tah_definitions definitions;
    /* The last service handle a JavaScript invoker's start was given. */
    uint64_t next_service;
    /* Reports to parents that core has not taken yet (tah_quarantine_report). */
    tah_vec quarantine_reports;
    /* One record per registered I/O processor type (adapters/io.h, core ADR 0068). */
    tah_vec io_processors; /* tah_io_processor * */
    /* Turn duration and timer lateness (host_metrics.h, napi ADR 0017 section 7). */
    tah_host_metrics metrics;
    /* Set while core dispatches due delayed sends: what a host I/O processor is handed
     * then is a timer firing, and its lateness is recorded. */
    bool dispatching_due;
    /* The recorder (recorder.h, kernel ADR 0006 section 2). */
    tah_recorder recorder;
    /* The kernel's parameters, fixed at creation (params.h, napi ADR 0035). */
    tah_params params;
};

/* Points the scheduler's observer at the recorder while it is on. */
void tah_system_observe(tah_system *system);

/*
 * The parameters of one scheduler call that enters core: the turn's, and
 * what the replay driver uses to run core with the same budgets and the same
 * settling (tah_sched_run).
 */
tah_turn_params tah_system_turn_params(tah_system *system);

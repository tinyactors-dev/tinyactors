#ifndef TINYACTORS_HOST_H
#define TINYACTORS_HOST_H

/*
 * The host kernel (kernel ADR 0001): what every host that keeps actors in
 * memory needs, free of any language's API. A language binding (napi, the
 * test host, a future CPython or browser build) links it and implements
 * the host interface declared here; the kernel gives it back the system,
 * actors, turns, mailboxes, routing and the write-ahead log.
 *
 * The kernel is libc-free C, like core: no I/O, no threads, memory only
 * through the allocator it is given, no recursion, and no C standard
 * library beyond the freestanding headers. It builds for
 * wasm32-freestanding. Anything that needs a platform (a clock, a timer, an
 * event loop, a language's values) is an interface the binding provides.
 *
 * This header is the kernel's whole public API. It includes core's public
 * header and nothing of any language.
 */

#include "tinyactors.h"

#ifndef TAH_API
#define TAH_API
#endif

#define TAH_VERSION_MAJOR 0
#define TAH_VERSION_MINOR 1
#define TAH_VERSION_PATCH 0
#define TAH_VERSION ((TAH_VERSION_MAJOR << 16) | (TAH_VERSION_MINOR << 8) | TAH_VERSION_PATCH)

/* The kernel's version, TAH_VERSION of the build that was linked. */
TAH_API uint32_t tah_version(void);

/* ========================================================================
 * The host interface: what a binding gives the kernel (kernel ADR 0001,
 * section 3). Each part is an adapter, a self pointer and a vtable, as
 * core's interfaces are. Step 1 of the migration declares their shape;
 * the steps that move napi's code in fill in the operations each needs.
 */

/*
 * Values are core's `ta_value` (napi ADR 0011's model): the binding's data
 * model makes them and implements their retain and release. The kernel
 * holds values only through that adapter and never looks inside one.
 *
 * The data model, with its streaming codec (core ADR 0090, sections 1 and
 * 2), is core's `ta_datamodel`: a data model encodes and decodes its own
 * values, and the kernel calls its codec for the write-ahead log.
 */

/*
 * The platform: what the kernel needs from the machine and cannot do itself
 * (kernel ADR 0001, section 3; step 2b). The binding implements it once and
 * hands it to every system it creates.
 * - `allocator` is where the kernel's memory comes from: the system record,
 *   and the backing store of its counting allocator (allocator.h). A browser
 *   build gives an allocator over its own heap.
 * - `monotonic_ns` is a monotonic clock in nanoseconds; only differences
 *   between readings matter. It drives the realtime clock, load timeouts and
 *   turn durations.
 * - `wall_ns` is wall-clock time in nanoseconds since the Unix epoch, for
 *   the timestamps of exported telemetry. 0 when the platform has none.
 * - `random_seed` is 64 random bits, to seed core's trace-ID generator once
 *   per trace sink (core ADR 0073: core never reads the OS either).
 */
typedef struct tah_platform_vtable {
    uint64_t (*monotonic_ns)(void *self);
    uint64_t (*wall_ns)(void *self);
    uint64_t (*random_seed)(void *self);
} tah_platform_vtable;
typedef struct {
    void *self;
    const tah_platform_vtable *v;
    ta_allocator allocator;
} tah_platform;

/*
 * The pump driver: how the kernel's turns reach the binding's event loop.
 * The kernel decides what a turn does (tah_system_turn(), system.h); the
 * binding decides when it runs, through the system's `wake` callback (run
 * a turn soon, never inside the caller) and `arm` callback (one timer that
 * calls tah_system_timer_fired() after a delay in milliseconds). napi uses
 * setImmediate and setTimeout; the test host runs a turn when a scenario
 * says so; a browser build uses MessageChannel and setTimeout. They are
 * fields of the system, set by the binding after it creates one
 * (tah_wake_fn and tah_arm_fn in system.h).
 *
 * Hooks into the binding's language are callbacks on the system too: the
 * effects queue (effects.h) the binding drains after each turn, and the
 * per-system callbacks system.h lists (the definition an SCXML invoke
 * starts, the data of an error.communication, a quarantine report, a
 * fault). Errors are core's `ta_result`; the binding maps them to its
 * language's errors, so no mapping lives here.
 */


#endif

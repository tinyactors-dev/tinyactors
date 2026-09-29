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
 * The pump driver: how the kernel's turns reach the binding's event loop.
 * The kernel decides what a turn does; the driver decides when it runs.
 * napi asks setImmediate for a turn and setTimeout for a deadline; the test
 * host runs a turn when a scenario says so; a browser build uses
 * MessageChannel and setTimeout.
 */
typedef struct tah_pump_driver_vtable {
    /* Asks for tah_turn() to run soon. Idempotent until that turn runs. */
    void (*request_turn)(void *self);
    /* Asks for a turn at or after `due`, a monotonic time in nanoseconds;
     * a later call replaces the earlier deadline. UINT64_MAX disarms it. */
    void (*arm_timer)(void *self, uint64_t due);
    /* The binding's monotonic clock, in nanoseconds. */
    uint64_t (*now)(void *self);
} tah_pump_driver_vtable;
typedef struct {
    void *self;
    const tah_pump_driver_vtable *v;
} tah_pump_driver;

/*
 * Hooks: calls from the kernel into the binding's language, for what only
 * the language can do. They are filled in as the code that makes each call
 * moves into the kernel: effects (an I/O processor's send, an invoker's
 * start, cancel and forward), log records, trace requests, input-log
 * commits, loaders, and fault, done and dead-letter reports. Each hook
 * receives a record the kernel owns for the call; a binding that keeps
 * anything retains it.
 */
typedef struct tah_hooks_vtable {
    /* Effects queued by the last turn are ready to run in the language. */
    void (*effects_ready)(void *self);
} tah_hooks_vtable;
typedef struct {
    void *self;
    const tah_hooks_vtable *v;
} tah_hooks;

/*
 * Errors: the kernel answers `ta_result`, core's codes, and never throws.
 * The binding maps a result to its language's errors (napi's
 * TinyactorsError codes, a Python exception), so no mapping lives here.
 */

/* Everything a binding hands the kernel when it creates a system. */
typedef struct {
    ta_allocator allocator;
    tah_pump_driver pump;
    tah_hooks hooks;
} tah_host;

#endif

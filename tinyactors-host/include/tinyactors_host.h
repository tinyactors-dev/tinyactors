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
 * This header, with the module headers under tinyactors_host/ that it
 * includes, is the kernel's public API: a binding includes this header and
 * nothing from the kernel's src/. It includes core's public header and
 * nothing of any language.
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
 * section 3): the platform (tinyactors_host/platform.h), the pump driver
 * and the hooks, which are callbacks on the system (system.h).
 */

#include "tinyactors_host/platform.h"

/*
 * The kernel's modules. A binding includes this header, and nothing from the
 * kernel's src/: these headers are its public API. The system (system.h) is
 * the entry point; the rest are the plumbing it is made of, which a binding
 * reads directly (the scheduling table, mailboxes, effects, loads).
 */
#include "tinyactors_host/adapter_invoker.h"
#include "tinyactors_host/adapter_io.h"
#include "tinyactors_host/adapter_log.h"
#include "tinyactors_host/adapter_trace.h"
#include "tinyactors_host/allocator.h"
#include "tinyactors_host/containers.h"
#include "tinyactors_host/definitions.h"
#include "tinyactors_host/directory.h"
#include "tinyactors_host/effects.h"
#include "tinyactors_host/enumerate.h"
#include "tinyactors_host/events.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/host_metrics.h"
#include "tinyactors_host/inputs.h"
#include "tinyactors_host/wal.h"
#include "tinyactors_host/loads.h"
#include "tinyactors_host/offloads.h"
#include "tinyactors_host/otlp.h"
#include "tinyactors_host/params.h"
#include "tinyactors_host/replay.h"
#include "tinyactors_host/router.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "tinyactors_host/watch.h"

#endif

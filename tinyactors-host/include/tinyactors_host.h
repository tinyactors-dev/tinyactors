#ifndef TINYACTORS_HOST_H
#define TINYACTORS_HOST_H

/*
 * The host kernel (kernel ADR 0001): what every host that keeps actors in
 * memory needs, free of any language's API. A language binding (napi, the
 * test host, a future CPython or browser build) links it and implements
 * the host interface declared here; the kernel gives it back the system,
 * actors, turns, mailboxes, routing, the recorder and event replay.
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
 * the entry point; the rest are the plumbing it is made of (the scheduling
 * table, mailboxes, effects, loads). Their state is opaque: a binding gets
 * a part from the system's getters and calls its functions, and the layout
 * is in src/<module>_private.h. A struct stays defined here only when it is
 * data the binding fills or reads (a parameter record, a record or view
 * handed out, a snapshot, a table of callbacks, a container it embeds);
 * `mise run check-opaque` holds the list (kernel ADR 0001, "Step 2b").
 */
#include "tinyactors_host/adapter_invoker.h"
#include "tinyactors_host/adapter_io.h"
#include "tinyactors_host/adapter_log.h"
#include "tinyactors_host/allocator.h"
#include "tinyactors_host/containers.h"
#include "tinyactors_host/definitions.h"
#include "tinyactors_host/directory.h"
#include "tinyactors_host/effects.h"
#include "tinyactors_host/enumerate.h"
#include "tinyactors_host/events.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/host_metrics.h"
#include "tinyactors_host/loads.h"
#include "tinyactors_host/offloads.h"
#include "tinyactors_host/params.h"
#include "tinyactors_host/recorder.h"
#include "tinyactors_host/replay.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "tinyactors_host/watch.h"

#endif

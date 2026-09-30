#pragma once
/*
 * The definition lifecycle as the host counts it (core ADR 0079, napi ADR
 * 0017 section 7): how many definitions core registered, retired and freed,
 * and, for each definition still alive, who registered it.
 *
 * Core tells the addon of every event through its definition sink; this
 * module keeps what the metrics and the churn logs need:
 * - three counters, one per event kind;
 * - the origin of each live definition: the host (JavaScript's define()),
 *   nested (an inline child an import compiled when its parent was
 *   defined), or invoke (chart text compiled when an <invoke> ran). Core
 *   does not know who asked it to register a chart, so the addon marks
 *   the calls it makes on a child's behalf (`registering`) and every other
 *   registration is the host's. A definition leaves the map when core frees
 *   it, so the map is the size of what is alive.
 *
 * A steadily climbing registered count with a freed count that keeps pace
 * is churn: an invoke that compiles new chart text every time.
 *
 * This module has no Node-API in it.
 */
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

typedef enum {
    TAH_ORIGIN_HOST = 0,
    TAH_ORIGIN_NESTED,
    TAH_ORIGIN_INVOKE,
    TAH_ORIGINS
} tah_definition_origin;

typedef struct {
    tah_map origins; /* definition ID -> tah_definition_origin, for NESTED and INVOKE */
    uint64_t registered;
    uint64_t retired;
    uint64_t freed;
    /* The origin of the registrations core makes now; HOST outside a child's compile. */
    tah_definition_origin registering;
} tah_definitions;

void tah_definitions_init(tah_definitions *definitions, ta_allocator allocator);
void tah_definitions_destroy(tah_definitions *definitions);

/*
 * Counts one event and keeps or forgets the definition's origin, which it
 * returns: the origin being registered for REGISTERED, the one kept for
 * RETIRED and FREED. If the origin cannot be kept for lack of memory, the
 * definition reads as the host's.
 */
tah_definition_origin tah_definitions_note(tah_definitions *definitions, const ta_definition_event *event);

tah_definition_origin tah_definitions_origin(const tah_definitions *definitions, ta_definition_id definition);

const char *tah_definition_origin_name(tah_definition_origin origin);

/* The origin of the registrations core makes from now on: NESTED or INVOKE
 * while the binding compiles a child for core, HOST again after. */
void tah_definitions_set_registering(tah_definitions *definitions, tah_definition_origin origin);

/* Definitions registered, retired and freed so far (core ADR 0079). */
typedef struct {
    uint64_t registered;
    uint64_t retired;
    uint64_t freed;
} tah_definition_totals;
tah_definition_totals tah_definitions_totals(const tah_definitions *definitions);

/*
 * The live definitions by origin and by whether they are retired (still
 * alive because a child or an actor uses them): `alive[origin][retired]`.
 * It walks core's definitions (core ADR 0078).
 */
void tah_definitions_alive(const tah_definitions *definitions, const ta_system *core,
                           uint64_t alive[TAH_ORIGINS][2]);

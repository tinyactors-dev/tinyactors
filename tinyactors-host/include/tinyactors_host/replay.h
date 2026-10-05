#pragma once
/*
 * Replaying events (kernel ADR 0006 section 3): an actor prepared with `at`
 * (ta_actor_prepare) takes the events its journal holds, each at the time
 * it was taken, and is published (ta_actor_publish) once it has them all.
 * Nothing it does crosses the border until then.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct tah_system tah_system;

/*
 * Replaying one event (kernel ADR 0006 section 3): the event an actor took,
 * as its chart saw it, delivered again at the time it was taken to an
 * actor prepared with `at` (a replaying actor), whose macrostep then runs to
 * its end. Nothing it does crosses the border. The texts are borrowed for
 * the call; an empty one is absent. An event of type platform named
 * error.communication.invoke.<invoke_id>.<reason> is reported again as the
 * invocation's error report, so core retires the invocation as it did live
 * (reason "quarantined" keeps it).
 */
typedef struct {
    ta_actor_id actor;
    ta_str name;
    ta_event_type type;
    ta_str send_id;
    ta_str origin;
    ta_str origin_type;
    ta_str invoke_id;
    TA_IN TA_OPTIONAL const ta_value *data; /* borrowed; the event retains it */
    ta_time time;
    TA_OUT TA_OPTIONAL bool *consumed; /* a transition took the event */
    TA_OUT TA_OPTIONAL bool *finished; /* the actor reached a top-level final state */
} tah_replay_event_params;
/*
 * TA_ERR_INVALID: an empty name, a time earlier than the actor's last
 * input or later than the system's, or an actor that is not replaying;
 * TA_ERR_NOT_FOUND: no such actor, or an error report for an invocation that
 * is not active; TA_ERR_DONE: the actor finished already; TA_ERR_NOMEM.
 * A macrostep that runs into the microstep limit is abandoned, as it was
 * live (core ADR 0099 section 7): the call answers TA_OK and the actor stands
 * where its last microstep left it.
 */
ta_result tah_replay_event(tah_system *system, const tah_replay_event_params *params);
/*
 * The replaying actor is about to be published: its bootstrap runs to its
 * end if no event ran it (an empty journal), and the scheduler takes it
 * back. TA_ERR_DONE when it reached a final state; what core answers
 * otherwise.
 */
ta_result tah_replay_events_done(tah_system *system, ta_actor_id actor);

#pragma once
/*
 * Event names the kernel holds as text (core ADR 0100). Core keeps run-time
 * names as bytes and interns none a host gives it, so the kernel keeps the
 * names it is given in its own table: JavaScript caches the kernel's ID per
 * name text (lib/events.js), native post paths take the ID, and the kernel
 * hands core the text when it admits the message. A name is never removed,
 * so the cache never needs invalidation.
 *
 * A kernel name ID has TAH_NAME_KERNEL set. An event that came from core
 * carries its name as text: mail core queued gives it in the TAKEN
 * outcome's name, and only an observer that keeps the event (the recorder)
 * stores it under a kernel ID; an event forwarded to a host invoker names
 * it with core's text handle (TA_EVENT_NAME_TEXT), which the kernel stores
 * under a kernel ID when it keeps the event (tah_names_adopt), while the
 * handle is still valid.
 *
 * Validation happens once per name: the text must be non-empty, have no
 * empty segment ("a..b", ".a", "a."), and at most the system's
 * eventNameDepth segments (core ADR 0092). Core checks the same rules again.
 */
#include <stddef.h>

#include "tinyactors.h"

/* The bit of a kernel name ID; core's names never have it. */
#define TAH_NAME_KERNEL 0x40000000u

/* Core's ID for the name of a delayed send a host restores (core
 * ta_delayed_send_info.event), which core keeps by ID while delayed sends
 * are not envelopes (core ADR 0100's implementation notes): interned in
 * core. TA_ERR_INVALID for malformed text; TA_ERR_NOMEM from core. */
ta_result tah_core_event_name(ta_system *core, ta_str text, ta_event_name_id *out);


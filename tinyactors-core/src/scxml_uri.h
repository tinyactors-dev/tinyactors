#pragma once

#include "types.h"

/*
 * The SCXML Event I/O Processor target URI grammar (ADR 0044).
 *
 * Core owns it in both directions, so a literal `target`, an evaluated
 * `targetexpr`, and the text core writes for `$_event.origin` all mean the
 * same thing. `ta_scxml_target_parse()` and `ta_scxml_target_format()` are
 * the public entry points; this is the writer the data-model host view
 * shares with them.
 */

/* Writes the URI of `target`; false when its kind has no URI (SELF, INVALID). */
bool ta_scxml_uri_write(const ta_system *system, ta_text_writer *writer, const ta_scxml_target *target);
/* `#_actor_<family>/<name>` with both parts percent-encoded (ADR 0096). */
void ta_actor_uri_write(ta_text_writer *writer, const ta_actor_address *address);

/* A session ID's text, <incarnation>.<session>.<family> (ADR 0088). */
typedef struct {
    uint64_t incarnation;
    uint64_t session;
    ta_str family; /* borrowed from the parsed text */
} ta_session_text;

/* False when `text` is not in that form (a bare number is not). */
bool ta_session_text_parse(ta_str text, ta_session_text *out);
/* Writes a session of this system as text. */
void ta_session_text_write(const ta_system *system, ta_text_writer *writer, uint64_t session, ta_str family);
/* Writes a runtime reference's session: as text when its family is known,
 * from the reference or its live actor, otherwise as a bare number. */
void ta_session_ref_write(const ta_system *system, ta_text_writer *writer, const ta_actor_ref *ref);

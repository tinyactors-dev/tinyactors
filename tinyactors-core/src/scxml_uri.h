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

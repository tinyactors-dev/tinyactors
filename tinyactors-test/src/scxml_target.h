#pragma once

#include "tinyactors.h"

/*
 * Parses an SCXML Event I/O Processor target URI: #_internal, #_self,
 * #_parent, #_scxml_<actor id>, #_actor_<family>/<name> (the logical
 * address of a named actor), or #_<invoke id>. On failure returns false
 * and sets *error to a static message.
 *
 * A named address borrows `family` and `name` from `target`, so the caller
 * keeps that text alive for as long as the decoded target is used
 * (TA_LIFETIME(input) on ta_datamodel_decode_scxml_target_params.out).
 * TAT_TARGET_TEXT_MAX is the longest target a data model stores for that.
 */
bool tat_scxml_target_parse(ta_str target, ta_scxml_target *out, const char **error);

enum { TAT_TARGET_TEXT_MAX = 255 };

/*
 * Copies `text` into `buffer` (TAT_TARGET_TEXT_MAX + 1 bytes) and parses it
 * from there, so a decoded named address stays valid while the buffer does.
 */
bool tat_scxml_target_decode(const char *text, size_t length, char *buffer, ta_scxml_target *out,
                             const char **error);

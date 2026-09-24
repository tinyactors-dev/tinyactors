#pragma once

#include "event.h"
#include "fault.h"

typedef struct {
    ta_atom type;
    ta_io io;
} ta_io_registration;

/*
 * Rendering origins (ADR 0068). `session_id` and `address` describe the
 * session the origin was delivered to, for a COMPUTED origin's `text`
 * callback. Stored URIs come back as views; everything else is written into
 * `arena`.
 */
typedef struct {
    const ta_origin *origin;
    uint64_t session_id;
    ta_address *address;
    ta_allocator arena;
    ta_str *out;
} ta_io_origin_render_params;
ta_result ta_io_origin_uri(const ta_system *system, const ta_io_origin_render_params *params);
/* The processor type of an origin; TA_ERR_NOT_FOUND for none. */
ta_result ta_io_origin_type(const ta_system *system, const ta_origin *origin, ta_str *out);

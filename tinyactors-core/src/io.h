/**
 * @file io.h
 *
 * # I/O processors and origins
 *
 * The entry of the system's I/O processor registry (`ta_io_registration`:
 * a type atom and the processor) and the rendering of an event's origin: as the
 * URI a chart reads from `_event.origin` and as the processor type it reads
 * from `_event.origintype` (ADR 0068). The unit also defines the public
 * origin and scope calls of the I/O processor API.
 *
 * ## Contract and memory
 *
 * - An actor or invocation origin renders as an SCXML target URI and has
 *   the SCXML Event I/O Processor's type (or, for a non-SCXML invocation,
 *   the invocation's type). A processor's origin renders by its class:
 *   COMPUTED asks the processor's `text` callback, SCOPED and MESSAGE give
 *   back the URI core stores, as a view.
 * - Rendered text is written into the caller's `arena`; a stored URI is a
 *   view that lives as long as its scope or message entry.
 *
 * ## Failure
 *
 * An origin with nothing to render answers `TA_ERR_NOT_FOUND`; a full
 * arena answers what the arena answers.
 *
 * ## Users
 *
 * The data model's host view (`_event.origin`, `_event.origintype`),
 * session images (which store an origin as text), and the system's
 * registry. The SCXML Event I/O Processor is described in
 * `docs/internal/io.md`.
 */
#pragma once

#include "event.h"
#include "../include/tinyactors.h"

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

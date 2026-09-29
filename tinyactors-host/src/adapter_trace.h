#pragma once
/*
 * The trace sink (core ADR 0067, napi ADR 0016). Core writes a record
 * inside a core call; the adapter hands it to the system's OTLP encoder
 * (otlp.h), and the pump hands the spans that ended to the JavaScript hook
 * as bytes at the end of the turn. Installed only while a hook is
 * registered: without one the system runs with core's null sink, and
 * builds no record at all.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct tah_system tah_system;

/* The kind masks the sink asks core for (core ADR 0073 section 4). */
typedef struct {
    uint32_t kinds;           /* sampled macrosteps: the detail preset */
    uint32_t watched_kinds;   /* watched macrosteps: every kind */
    uint32_t unsampled_kinds; /* unsampled macrosteps */
} tah_trace_masks;

/* The sink, wanting `masks`. */
ta_trace_sink tah_trace_sink_adapter(tah_system *system, const tah_trace_masks *masks);

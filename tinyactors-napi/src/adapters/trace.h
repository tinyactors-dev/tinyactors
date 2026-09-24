#pragma once
/*
 * The buffered trace sink (core ADR 0067, napi ADR 0015). Core writes a
 * record inside a core call; the adapter queues a copy (traces.h), and the
 * pump hands the turn's records to the JavaScript hook as one array at the
 * end of the turn. Installed only while a hook is registered: without one
 * the system runs with core's null sink, and builds no record at all.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct tna_system tna_system;

/* The sink, wanting `kinds` (TA_TRACE_BIT masks), and unsampled macrosteps
 * too when `unsampled`. */
ta_trace_sink tna_trace_sink_adapter(tna_system *system, uint32_t kinds, bool unsampled);

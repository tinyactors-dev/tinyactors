#pragma once
/*
 * Telemetry in the kernel (core ADR 0067, ADR 0098; napi ADR 0016). Core
 * writes a trace record to its tel ring for every kind the masks enable;
 * the kernel reads the ring after each call it makes into core and hands
 * the records to the system's OTLP encoder (otlp.h). The pump hands the
 * spans that ended to the JavaScript hook as bytes at the end of the turn.
 * Masks are set only while a hook is registered: without one they are all
 * zero, and core writes no record at all.
 *
 * Lifetimes. The masks are the system's, held by core. A record is read in
 * place from the ring, decoded into a view (ta_tel_entry) that lives until
 * the record is released, and encoded at once; the encoder copies what it
 * keeps, so nothing outlives the call. The drain never allocates in core;
 * the encoder allocates its own staging, as before.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct tah_system tah_system;

/* The kind masks the host asks core for (core ADR 0073 section 4, ADR 0098 section 5). */
typedef struct {
    uint32_t kinds;           /* sampled macrosteps: the detail preset */
    uint32_t watched_kinds;   /* watched macrosteps: every kind */
    uint32_t unsampled_kinds; /* unsampled macrosteps */
    bool values;              /* have core render values (event data, assigned values) as text */
} tah_trace_masks;

/*
 * Sets core's masks (ta_system_set_trace_masks) to `masks`; all zero stops
 * tracing. Reads and encodes what the ring holds first, so records written
 * under the old masks are not lost to the change. Not while a macrostep runs.
 */
ta_result tah_trace_set_masks(tah_system *system, const tah_trace_masks *masks);

/*
 * Reads every record on the tel ring: each tel.trace goes to the OTLP
 * encoder (tah_otlp_write) and each tel.dropped to its drop note
 * (tah_otlp_dropped), when a hook is installed (tah_system_tracing); the
 * records are released either way, so the ring is empty. Asks for a turn
 * when a span ended. Returns how many records it read. The kernel calls it
 * after every call it makes into core that may have traced; a binding that
 * calls core itself calls it after those calls (and once per pump turn).
 * Never allocates in core, and cannot fail: a record that does not decode
 * is skipped and released.
 */
uint32_t tah_trace_drain(tah_system *system);

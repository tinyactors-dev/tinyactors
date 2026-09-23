#pragma once
/*
 * JavaScript I/O processors (core ADRs 0015, 0046, ADR 0001 rule 3).
 *
 * One adapter serves every type a system registers. Core calls it inside a
 * core call, for a <send> whose type names it, including a delayed send when
 * it falls due. The adapter queues the send as an effect (effects.h),
 * retaining its target and data and copying literal content, and answers
 * success: the processor runs at the end of the turn, so it cannot fail the
 * send synchronously. A processor that throws or rejects is reported to the
 * sender as an error.communication event (lib/system.js).
 */
#include "tinyactors.h"

typedef struct tna_system tna_system;

ta_io tna_io_adapter(tna_system *system);

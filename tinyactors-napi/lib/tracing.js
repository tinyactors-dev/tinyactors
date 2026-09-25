'use strict';

// Tracing (core ADR 0067, napi ADR 0016). The addon hands a registered hook
// the spans that ended in each pump turn as OTLP protobuf bytes
// (System.setTraceSink); lib/otlp.js decodes them for those who want
// objects. This module holds what JavaScript still needs: the kind mask,
// W3C traceparent parsing and formatting, and span IDs derived from step
// keys exactly as core derives them. It depends on no telemetry package.

const native = require('./native');
const { TinyactorsError } = require('./errors');

/** Record kinds, in core's order; a sink's mask is built from these names. */
const TRACE_KINDS = Object.freeze([...native.traceKinds]);

/** The mask for `kinds` (names from TRACE_KINDS); every kind when undefined. */
function kindMask(kinds) {
  if (kinds === undefined) {
    return 2 ** TRACE_KINDS.length - 1;
  }
  if (!Array.isArray(kinds)) {
    throw new TinyactorsError('setTraceSink(): options.kinds must be an array of record kinds', 'invalid');
  }
  let mask = 0;
  for (const kind of kinds) {
    const bit = TRACE_KINDS.indexOf(kind);
    if (bit < 0) {
      throw new TinyactorsError(`setTraceSink(): unknown record kind "${kind}"`, 'invalid');
    }
    mask |= 2 ** bit;
  }
  return mask;
}

// ------------------------------------------------------------------------
// W3C trace context: traceparent = version "-" trace-id "-" parent-id "-" flags

const TRACEPARENT = /^([0-9a-f]{2})-([0-9a-f]{32})-([0-9a-f]{16})-([0-9a-f]{2})$/;
const ZERO_TRACE = '0'.repeat(32);
const ZERO_SPAN = '0'.repeat(16);

/**
 * Parses a `traceparent` header: { traceId, parentSpanId, flags, sampled },
 * or undefined when it is not a valid one (the W3C rule: a receiver that
 * cannot parse it starts a new trace).
 */
function parseTraceparent(header) {
  if (typeof header !== 'string') {
    return undefined;
  }
  const match = TRACEPARENT.exec(header.trim().toLowerCase());
  if (match === null || match[1] === 'ff' || match[2] === ZERO_TRACE || match[3] === ZERO_SPAN) {
    return undefined;
  }
  const flags = Number.parseInt(match[4], 16);
  return { traceId: match[2], parentSpanId: match[3], flags, sampled: (flags & 1) === 1 };
}

/** A `traceparent` header for a span: { traceId, spanId, sampled | flags }. */
function formatTraceparent({ traceId, spanId, sampled = true, flags }) {
  const bits = flags ?? (sampled ? 1 : 0);
  return `00-${traceId}-${spanId}-${bits.toString(16).padStart(2, '0')}`;
}

const MASK64 = (1n << 64n) - 1n;

function mix(value) {
  let v = value;
  v ^= v >> 33n;
  v = (v * 0xff51afd7ed558ccdn) & MASK64;
  v ^= v >> 33n;
  v = (v * 0xc4ceb9fe1a85ec53n) & MASK64;
  v ^= v >> 33n;
  return v;
}

const hex16 = (value) => value.toString(16).padStart(16, '0');

/**
 * The 16-hex-digit span ID of step (session, macrostep, microstep). A host
 * cause (session 0) is the host's own span ID, `macrostep`.
 */
function stepSpanId(session, macrostep, microstep = 0) {
  const s = BigInt(session);
  const m = BigInt(macrostep);
  if (s === 0n) {
    return hex16(m);
  }
  let span = mix(s);
  span = mix(span ^ m);
  span = mix(span ^ (BigInt(microstep) | (1n << 32n)));
  return hex16(span === 0n ? 1n : span);
}

module.exports = {
  TRACE_KINDS,
  kindMask,
  parseTraceparent,
  formatTraceparent,
  stepSpanId,
};

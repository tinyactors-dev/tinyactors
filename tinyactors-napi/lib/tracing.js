'use strict';

// Tracing (core ADR 0067, ADR 0015). The addon hands a registered hook the
// records core wrote, one array per pump turn (System.setTraceSink). This
// module turns them into plain data a tracer can take: span-shaped objects
// with W3C trace and span IDs, parents, times, attributes and events. It
// depends on no telemetry package; a user maps these objects onto the
// OpenTelemetry SDK or any other tracer (see docs/tracing.md).

const native = require('./native');
const { TinyactorsError } = require('./errors');

/** Record kinds, in core's order; a sink's mask is built from these names. */
const TRACE_KINDS = Object.freeze([...native.traceKinds]);

const LIFECYCLE = new Set(['spawned', 'loaded', 'unloaded', 'finished', 'destroyed']);

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

// What post(), send() and spawn() pass to the addon for options.traceparent.
function nativeTrace(traceparent, what) {
  if (traceparent === undefined) {
    return undefined;
  }
  const parsed = parseTraceparent(traceparent);
  if (parsed === undefined) {
    throw new TinyactorsError(`${what}: options.traceparent is not a W3C traceparent`, 'invalid');
  }
  return {
    high: BigInt(`0x${parsed.traceId.slice(0, 16)}`),
    low: BigInt(`0x${parsed.traceId.slice(16)}`),
    flags: parsed.flags,
    span: BigInt(`0x${parsed.parentSpanId}`),
  };
}

// The same, as ta_actor_spawn_params.trace for the generated binding.
function spawnTrace(traceparent) {
  const trace = nativeTrace(traceparent, 'spawn()');
  if (trace === undefined) {
    return undefined;
  }
  return {
    context: { trace_id_high: trace.high, trace_id_low: trace.low, flags: trace.flags },
    cause: { session: 0n, macrostep: trace.span, microstep: 0 },
  };
}

// ------------------------------------------------------------------------
// Span IDs, derived from step keys exactly as core's ta_step_key_span_id()

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

// A trace ID for a macrostep that came with none: derived from its step, so
// it is stable and every host derives the same one.
function derivedTraceId(session, macrostep) {
  return stepSpanId(session, macrostep, 0) + stepSpanId(session, macrostep, 0xffffffff);
}

/** A traceparent naming the step a send or an invocation came from (the "trace" of an I/O request). */
function traceparentOf(trace) {
  if (trace === undefined) {
    return undefined;
  }
  const traceId = hex16(trace.traceIdHigh) + hex16(trace.traceIdLow);
  if (traceId === ZERO_TRACE) {
    return undefined;
  }
  const { session, macrostep, microstep } = trace.cause;
  return formatTraceparent({ traceId, spanId: stepSpanId(session, macrostep, microstep), flags: trace.flags });
}

// ------------------------------------------------------------------------
// Records to span-shaped data

// Attribute names are ours: OpenTelemetry has no statechart convention.
const EVENT_NAMES = {
  condition: 'scxml.condition',
  raised: 'scxml.raise',
  dataInit: 'scxml.data',
  assign: 'scxml.assign',
  script: 'scxml.script',
  foreach: 'scxml.foreach',
  actionFailed: 'scxml.error',
  sent: 'scxml.send',
  cancelled: 'scxml.cancel',
  invoked: 'scxml.invoke',
  invokeCancelled: 'scxml.invoke.cancel',
  paused: 'scxml.paused',
  resumed: 'scxml.resumed',
  stopped: 'scxml.stopped',
};

function valueText(value, limit) {
  if (value === undefined) {
    return undefined;
  }
  let text;
  try {
    text = typeof value === 'string' ? value : JSON.stringify(value);
  } catch {
    text = String(value);
  }
  return text === undefined || text.length <= limit ? text : `${text.slice(0, limit)}...`;
}

function position(record, attributes) {
  if (record.at !== undefined) attributes['scxml.position'] = record.at;
  if (record.state !== undefined) attributes['scxml.state'] = String(record.state);
  if (record.transition !== undefined) attributes['scxml.transition'] = record.transition;
  if (record.action !== undefined) attributes['scxml.action'] = record.action;
}

function eventAttributes(event, attributes) {
  if (event === undefined) return;
  attributes['scxml.event.name'] = event.name;
  attributes['scxml.event.type'] = event.type;
  if (event.sendId !== undefined && event.sendId !== null) attributes['scxml.event.sendid'] = event.sendId;
  if (event.invokeId !== undefined) attributes['scxml.event.invokeid'] = event.invokeId;
}

/**
 * Turns trace records into finished spans, handed to `onSpan` as each one
 * ends: one span per macrostep, a child span per microstep, and the data
 * and decision records as span events on the step they happened in.
 * Lifecycle records (spawned, finished, ...) are spans of no duration.
 *
 * options:
 * - onSpan(span): required;
 * - parent: "cause" (the default) puts a macrostep under the step that
 *   caused it: the host's span for a delivery that came with a
 *   traceparent, else the sending step's span. "link" makes every
 *   macrostep a root of its trace and links it to its cause instead.
 * - values: include values (assignments, data, event data) as JSON text
 *   attributes, cut at `valueLimit` characters (default 256). Off by
 *   default: values may hold personal data.
 *
 * A span: { traceId, spanId, parentSpanId?, name, kind: "internal",
 * startTime, endTime (milliseconds since the epoch), attributes,
 * events: [{ name, time, attributes }], links: [{ traceId, spanId }],
 * status: { code: "ok" | "error" } }.
 */
class TraceSpans {
  #onSpan;
  #parenting;
  #values;
  #limit;
  // Open macrosteps by session: { span, microstep }.
  #open = new Map();
  // Trace IDs of spans seen, by span ID, so a macrostep a traced step
  // caused joins that trace even when its record carries none.
  #traces = new Map();

  constructor(options) {
    const { onSpan, parent = 'cause', values = false, valueLimit = 256 } = options ?? {};
    if (typeof onSpan !== 'function') {
      throw new TinyactorsError('TraceSpans: options.onSpan must be a function', 'invalid');
    }
    if (parent !== 'cause' && parent !== 'link') {
      throw new TinyactorsError('TraceSpans: options.parent must be "cause" or "link"', 'invalid');
    }
    this.#onSpan = onSpan;
    this.#parenting = parent;
    this.#values = values === true;
    this.#limit = valueLimit;
  }

  /** Feeds a batch of records, as the trace sink receives them. */
  write(records) {
    for (const record of records) {
      this.#record(record);
    }
  }

  #record(record) {
    if (LIFECYCLE.has(record.kind)) {
      this.#lifecycle(record);
      return;
    }
    switch (record.kind) {
      case 'macrostepBegin':
        this.#begin(record);
        return;
      case 'macrostepEnd':
        this.#end(record);
        return;
      case 'microstep':
        this.#microstep(record);
        return;
      default:
        this.#event(record);
    }
  }

  #traceOf(record, causeSpan) {
    return record.traceId ?? this.#traces.get(causeSpan) ?? derivedTraceId(record.session, record.macrostep);
  }

  #causeSpan(cause) {
    if (cause === undefined) return undefined;
    if (cause.session === 0n && cause.macrostep === 0n) return undefined;
    return stepSpanId(cause.session, cause.macrostep, cause.microstep);
  }

  #span({ traceId, spanId, parentSpanId, name, time, attributes, links = [] }) {
    this.#traces.set(spanId, traceId);
    return { traceId, spanId, parentSpanId, name, kind: 'internal', startTime: time, endTime: time, attributes,
             events: [], links, status: { code: 'ok' } };
  }

  #baseAttributes(record) {
    return {
      'scxml.session_id': record.session.toString(),
      'scxml.macrostep': record.macrostep,
      'scxml.actor': `${record.actor.slot}:${record.actor.generation}`,
      'scxml.time': record.now,
    };
  }

  #begin(record) {
    const causeSpan = this.#causeSpan(record.cause);
    const traceId = this.#traceOf(record, causeSpan);
    const attributes = this.#baseAttributes(record);
    eventAttributes(record.event, attributes);
    if (this.#values && record.event?.data !== undefined) {
      attributes['scxml.event.data'] = valueText(record.event.data, this.#limit);
    }
    const linked = this.#parenting === 'link' && causeSpan !== undefined;
    const span = this.#span({
      traceId,
      spanId: stepSpanId(record.session, record.macrostep, 0),
      parentSpanId: linked ? undefined : causeSpan,
      name: `scxml.macrostep ${record.event?.name ?? (record.macrostep === 1 ? 'bootstrap' : '(no event)')}`,
      time: record.wallTime,
      attributes,
      links: linked ? [{ traceId: this.#traces.get(causeSpan) ?? traceId, spanId: causeSpan }] : [],
    });
    this.#open.set(record.session, { span, microstep: undefined });
  }

  #closeMicrostep(open, time) {
    if (open.microstep !== undefined) {
      open.microstep.endTime = time;
      this.#onSpan(open.microstep);
      open.microstep = undefined;
    }
  }

  #microstep(record) {
    const open = this.#open.get(record.session);
    if (open === undefined) return;
    this.#closeMicrostep(open, record.wallTime);
    const attributes = this.#baseAttributes(record);
    attributes['scxml.microstep'] = record.microstep;
    eventAttributes(record.event, attributes);
    attributes['scxml.transition'] = record.transitions;
    attributes['scxml.state.exited'] = record.exited.map(String);
    attributes['scxml.state.entered'] = record.entered.map(String);
    if (record.history !== undefined) attributes['scxml.state.history'] = record.history.map(String);
    open.microstep = this.#span({
      traceId: open.span.traceId,
      spanId: stepSpanId(record.session, record.macrostep, record.microstep),
      parentSpanId: open.span.spanId,
      name: `scxml.microstep ${record.event?.name ?? '(eventless)'}`,
      time: record.wallTime,
      attributes,
    });
  }

  #end(record) {
    const open = this.#open.get(record.session);
    if (open === undefined) return;
    this.#closeMicrostep(open, record.wallTime);
    open.span.attributes['scxml.state'] = record.states.map(String);
    open.span.endTime = record.wallTime;
    this.#open.delete(record.session);
    this.#onSpan(open.span);
  }

  #event(record) {
    const open = this.#open.get(record.session);
    if (open === undefined) return;
    const target = open.microstep !== undefined && record.microstep !== 0 ? open.microstep : open.span;
    const attributes = {};
    position(record, attributes);
    eventAttributes(record.event, attributes);
    if (record.expr !== undefined) attributes['scxml.expression'] = record.expr;
    if (record.result !== undefined) attributes['scxml.result'] = record.result;
    if (record.name !== undefined) attributes['scxml.data.name'] = record.name;
    if (record.kind === 'foreach') attributes['scxml.iteration'] = record.iteration;
    if (record.sendId !== undefined && record.sendId !== null) attributes['scxml.send_id'] = record.sendId;
    if (record.invokeId !== undefined) attributes['scxml.invoke_id'] = record.invokeId;
    if (record.fault !== undefined) {
      attributes['scxml.fault.code'] = record.fault.code;
      if (record.fault.detail !== undefined) attributes['scxml.fault.detail'] = record.fault.detail;
      target.status = { code: 'error' };
    }
    if (this.#values && record.value !== undefined) {
      attributes['scxml.value'] = valueText(record.value, this.#limit);
    }
    if (record.kind === 'sent') {
      // What the send starts elsewhere names this step as its cause.
      attributes['scxml.cause.span_id'] = stepSpanId(record.session, record.macrostep, record.microstep);
    }
    target.events.push({ name: EVENT_NAMES[record.kind] ?? `scxml.${record.kind}`, time: record.wallTime, attributes });
  }

  #lifecycle(record) {
    const causeSpan = this.#causeSpan(record.cause);
    const attributes = { 'scxml.session_id': record.session.toString(),
                         'scxml.actor': `${record.actor.slot}:${record.actor.generation}` };
    if (record.finalState !== undefined) attributes['scxml.state'] = String(record.finalState);
    const traceId = record.traceId ?? this.#traces.get(causeSpan) ?? derivedTraceId(record.session, 0);
    this.#onSpan(this.#span({
      traceId,
      spanId: stepSpanId(record.session, 0, TRACE_KINDS.indexOf(record.kind) + 1),
      parentSpanId: causeSpan,
      name: `scxml.${record.kind}`,
      time: record.wallTime,
      attributes,
    }));
  }
}

module.exports = {
  TRACE_KINDS,
  kindMask,
  parseTraceparent,
  formatTraceparent,
  traceparentOf,
  stepSpanId,
  nativeTrace,
  spawnTrace,
  TraceSpans,
};

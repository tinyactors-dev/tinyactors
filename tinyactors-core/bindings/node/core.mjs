/**
 * tinyactors — the JavaScript API of the Node.js binding, a thin layer over
 * its `native` interface (docs/proposals/thin-js-binding.md has the mapping
 * of every item to the core functions behind it). The web binding has the
 * same API on its own implementation.
 *
 * The shape borrows xstate's names (createActor, send, getSnapshot,
 * subscribe, system.get); what each call does is what the core does. Charts
 * are SCXML: fromSCXML(xml), or built with the helpers of `scxml.chart()`,
 * which write the same document. The outside world comes in through I/O
 * processors (<send type>) and invokers (<invoke type>), other systems
 * through links. Everything else — values, timers, routing, versions,
 * hibernation, migration — is the core's.
 *
 * `native` is the addon (src/*.c); `platform` supplies the clock,
 * timers, deferral, logging and HTTP posts.
 */

export function createRuntime({ native, platform }) {
  const K = native.constants;
  const NONE = Object.freeze([]);
  const wallClock = () => (globalThis.performance && performance.timeOrigin ? performance.timeOrigin + performance.now() : Date.now());
  const randomID = () =>
    (globalThis.crypto && globalThis.crypto.randomUUID ? globalThis.crypto.randomUUID() : `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`)
      .replace(/-/g, '')
      .slice(0, 12);

  /* ------------------------------------------------------------------ charts
   * One helper per SCXML element: identifying attributes positionally, then
   * an attributes object under SCXML's names, then children (elements, or
   * text). c.expr(fn) marks code: written into the document as the
   * function's source, and called directly (with the session's variables
   * and _event, _sessionid, _name, _ioprocessors, In) by the native
   * ecmascript datamodel. */

  class Expr {
    constructor(fn) {
      if (typeof fn !== 'function') throw new TypeError('expr(fn): a function of the session\'s variables');
      this.fn = fn;
    }
  }

  class Element {
    constructor(tag, attrs, children) {
      this.tag = tag;
      this.attrs = attrs;
      this.children = children;
    }
  }

  /** A chart: SCXML text, the functions its c.expr expressions name, and the documents its src attributes name. */
  class Chart {
    constructor(xml, { name, expressions, documents } = {}) {
      if (typeof xml !== 'string') throw new TypeError('a chart is SCXML text');
      this.xml = xml;
      this.name = name;
      this.expressions = expressions || new Map();
      this.documents = documents || {};
    }
  }

  /** A chart of SCXML text, compiled as it is; `documents` answer its src attributes. */
  function fromSCXML(xml, options = {}) {
    return new Chart(xml, { name: options.name, documents: options.documents });
  }

  const plain = (x) => x !== null && typeof x === 'object' && !(x instanceof Element) && !(x instanceof Expr) && !Array.isArray(x);
  const positional = (x) =>
    x === undefined || x === null || typeof x !== 'object' || x instanceof Expr || (Array.isArray(x) && !x.some((y) => y instanceof Element || Array.isArray(y)));

  /* positional attributes of each element ('#text': a text child) */
  const ELEMENTS = {
    scxml: ['name'], state: ['id'], parallel: ['id'], final: ['id'], history: ['id'], initial: [],
    transition: ['event', 'target'], onentry: [], onexit: [], datamodel: [], data: ['id', 'expr'],
    assign: ['location', 'expr'], donedata: [], content: ['#content'], param: ['name', 'expr'],
    script: ['#text'], raise: ['event'], if: ['cond'], elseif: ['cond'], else: [],
    foreach: ['array', 'item'], log: ['label', 'expr'], send: ['event'], cancel: ['sendid'],
    invoke: [], finalize: [],
  };

  /** A chart builder: c.expr and one helper per SCXML element; c.scxml(...) makes the chart. */
  function chart() {
    const expressions = new Map(); /* text -> function */
    const texts = new Map(); /* function -> text */
    const code = (x) => {
      if (x instanceof Expr) {
        let text = texts.get(x.fn);
        if (text !== undefined) return text;
        text = String(x.fn);
        for (let n = 2; expressions.has(text); n++) text = `${String(x.fn)} /* ${n} */`;
        expressions.set(text, x.fn);
        texts.set(x.fn, text);
        return text;
      }
      return typeof x === 'string' ? x : JSON.stringify(x);
    };
    const element = (tag) => (...args) => {
      const attrs = {};
      const children = [];
      const slots = ELEMENTS[tag];
      let i = 0;
      for (; i < slots.length && i < args.length && positional(args[i]); i++) {
        const v = args[i];
        if (v === undefined || v === null) continue;
        if (slots[i] === '#text') children.push(code(v));
        else if (slots[i] === '#content') {
          if (v instanceof Expr) attrs.expr = code(v);
          else children.push(typeof v === 'string' ? v : JSON.stringify(v));
        } else attrs[slots[i]] = code(v);
      }
      const add = (x) => {
        if (x === undefined || x === null || x === false) return;
        if (Array.isArray(x)) x.forEach(add);
        else if (x instanceof Element || typeof x === 'string') children.push(x);
        else if (x instanceof Expr) children.push(code(x));
        else if (plain(x) && tag === 'datamodel') for (const [id, v] of Object.entries(x)) children.push(new Element('data', { id, expr: v === undefined ? undefined : code(v) }, []));
        else if (plain(x)) for (const [k, v] of Object.entries(x)) attrs[k] = v === undefined || v === null ? undefined : code(v);
        else children.push(String(x));
      };
      args.slice(i).forEach(add);
      return new Element(tag, attrs, children);
    };
    const c = { expr: (fn) => new Expr(fn) };
    for (const tag of Object.keys(ELEMENTS)) c[tag] = element(tag);
    c.scxml = (...args) => {
      const root = element('scxml')(...args);
      root.attrs = { xmlns: 'http://www.w3.org/2005/07/scxml', version: '1.0', ...root.attrs };
      return new Chart(writeXML(root), { name: root.attrs.name, expressions });
    };
    return c;
  }

  /** XML of an element tree, written by C (ta_xml_writer): 1 open, 2 attribute, 3 text, 4 close. */
  function writeXML(root) {
    const ops = [];
    const strings = [];
    (function walk(node) {
      if (typeof node === 'string') {
        ops.push(3);
        strings.push(node);
        return;
      }
      ops.push(1);
      strings.push(node.tag);
      for (const [k, v] of Object.entries(node.attrs)) {
        if (v === undefined) continue;
        ops.push(2);
        strings.push(k, String(v));
      }
      node.children.forEach(walk);
      ops.push(4);
    })(root);
    return native.writeXML(Uint8Array.from(ops), strings);
  }

  /* expression kinds (TA_EXPRESSION_* in image.h) */
  const CONDITION = 1, VALUE = 2, LOCATION = 3, SCRIPT = 4, STRING = 5, CONTENT = 6;
  const compiled = new Map(); /* `${kind}:${text}` -> function of the scope (pure: shared) */

  /* An expression's text as a function of the scope, compiled once. */
  function compileExpression(text, kind) {
    const key = `${kind}:${text}`;
    let f = compiled.get(key);
    if (f) return f;
    try {
      if (kind === CONTENT) {
        let value = text;
        try {
          value = JSON.parse(text);
        } catch {
          /* not JSON: the text */
        }
        f = () => (value !== null && typeof value === 'object' ? structuredClone(value) : value);
      } else if (kind === SCRIPT) {
        f = new Function('__scope', `with (__scope) {\n${text}\n}`);
      } else if (kind === LOCATION) {
        const root = (/^\s*([A-Za-z_$][\w$]*)/.exec(text) || [])[1];
        const assign = new Function('__scope', '__value', `with (__scope) { (${text}) = __value; }`);
        f = (scope, value) => {
          if (!root || !(root in scope)) throw new ReferenceError(`${text}: not a declared location`);
          assign(scope, value);
        };
      } else {
        f = new Function('__scope', `with (__scope) return (\n${text}\n);`);
      }
    } catch (e) {
      f = () => {
        throw e;
      };
    }
    compiled.set(key, f);
    return f;
  }

  /* --------------------------------------------------------- pure functions
   * Hand-written wrappers of the core's C: addresses, frames, trace values,
   * OTLP. */

  /** An address's parts (C's parser), or null: { system, local, kind: 'session' | 'name', session (BigInt) | name }. */
  function parseAddress(text) {
    const a = native.parseAddress(text);
    if (!a) return null;
    return a.kind === 'session' ? { system: a.system, local: a.local, kind: 'session', session: BigInt(a.session) } : { system: a.system, local: a.local, kind: 'name', name: a.name, encoded: a.encoded };
  }

  /** tinyactors://<system>/#_scxml_<session> (a number or BigInt) or tinyactors://<system>/@<name> (any text). */
  function formatAddress(system, local) {
    return native.formatAddress(String(system), local);
  }

  /** One frame: the length prefix (ta_frame_header) and the UTF-8 bytes of `text`. */
  const utf8 = new TextEncoder();
  const utf8Strict = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
  function encodeFrame(text) {
    const body = utf8.encode(String(text));
    const out = new Uint8Array(4 + body.length);
    out.set(native.frameHeader(body.length), 0);
    out.set(body, 4);
    return out;
  }

  /** Bytes in, frames out (ta_frame_read): push() takes chunks as they arrive (any split)
   * and returns the frames they complete — a string each, or for a refused
   * frame { reason: 'malformed', length, detail }. */
  class FrameDecoder {
    constructor({ maxFrame = K.FRAME_MAX } = {}) {
      this.maxFrame = maxFrame;
      this.reader = native.frameReader(maxFrame);
    }
    push(chunk) {
      return native.frameRead(this.reader, chunk).map((f) => {
        if (typeof f === 'number') return { reason: 'malformed', length: f, detail: `a frame of ${f} bytes is above max_frame (${this.maxFrame})` };
        try {
          return utf8Strict.decode(f);
        } catch {
          return { reason: 'malformed', length: f.length, detail: 'a frame that is not UTF-8' };
        }
      });
    }
  }

  /** A W3C traceparent's fields (C's parser), or undefined when it is not a valid one. */
  function parseTraceparent(header) {
    const p = native.parseTraceparent(header);
    return p ? { traceID: p.traceID, parentSpanID: p.parentSpanID, flags: p.flags, sampled: (p.flags & 1) === 1 } : undefined;
  }

  /** A traceparent naming `spanID` as the parent (sampled unless `sampled: false` or `flags` says). */
  function formatTraceparent({ traceID, spanID, sampled, flags } = {}) {
    const text = native.formatTraceparent(String(traceID), String(spanID), flags !== undefined ? flags : sampled === false ? 0 : 1);
    if (text === null) throw new TypeError('formatTraceparent: a 32-digit traceID and a 16-digit spanID, in hex, neither all zeros');
    return text;
  }

  const decimal = (v, what) => {
    const m = /^\D*(\d+)$/.exec(String(v));
    if (!m) throw new TypeError(`stepSpanID: ${what} is not a number`);
    return m[1];
  };
  /** The span ID of a step (C's mix): session, macrostep, microstep (0: the macrostep's span). */
  function stepSpanID(session, macrostep, microstep = 0) {
    return native.stepSpanID(decimal(session, 'session'), decimal(macrostep, 'macrostep'), microstep >>> 0);
  }

  /** A compiled image (bytes from the tac compiler, or image.bytes()): its charts and boot actors. */
  class Image {
    constructor(handle) {
      this._native = handle;
      const info = native.imageInfo(handle);
      this.hash = info.hash;
      this.charts = info.charts.map((c) => ({ name: c.name, source: c.source, datamodel: c.datamodel }));
      this.actors = info.actors;
      this._info = info;
    }
    bytes() {
      return native.imageBytes(this._native);
    }
  }
  function loadImage(bytes) {
    return new Image(native.openImage(bytes));
  }

  /* ---------------------------------------------------------------- tracing
   * Core writes trace records (docs/adr/0006 in the repository root);
   * the addon (src/trace.c) encodes them as OTLP protobuf after every run, and this code
   * hands the bytes to a sink and to an OTLP/HTTP exporter, as they are. */

  /** Core's record kinds, in core's order (TA_TRACE_* in tinyactors/trace.h, from 1). */
  const TRACE_KINDS = Object.freeze([
    'dropped', 'spawned', 'finished', 'destroyed', 'unloaded', 'loaded', 'macrostepBegin', 'macrostepEnd',
    'microstep', 'condition', 'raised', 'sent', 'invoked', 'cancelled', 'assign', 'log', 'actionFailed',
  ]);
  if (TRACE_KINDS.length + 1 !== K.TRACE_KIND_COUNT) throw new Error('tinyactors: the trace kinds of this build differ from core.mjs');
  const kindsMask = (kinds) =>
    kinds.reduce((mask, kind) => {
      const i = TRACE_KINDS.indexOf(kind);
      if (i < 0) throw new TypeError(`not a trace kind: ${kind} (one of ${TRACE_KINDS.join(', ')})`);
      return (mask | (1 << (i + 1))) >>> 0;
    }, 0);
  const DETAIL = { summary: K.TRACE_SUMMARY >>> 0, decisions: K.TRACE_DECISIONS >>> 0, full: K.TRACE_FULL >>> 0 };
  const SAMPLE_WHOLE = 1000000;
  const EXPORT_DELAY_MS = 250; /* bytes gather this long before a post */
  const EXPORT_BYTES = 1 << 20; /* or until this many wait */
  const EXPORT_MAX_BYTES = 16 << 20; /* waiting beyond this is dropped */
  const FAILURE_LOG_MS = 10000;
  const METRICS_INTERVAL_MS = 60000; /* OTEL_METRIC_EXPORT_INTERVAL's default */
  const RULE_AT = { start: K.TRACE_AT_START, take: K.TRACE_AT_TAKE };
  const RULE_FROM = { host: K.TRACE_FROM_HOST, actor: K.TRACE_FROM_ACTOR, timer: K.TRACE_FROM_TIMER, spawn: K.TRACE_FROM_SPAWN, sampled: K.TRACE_FROM_SAMPLED, unsampled: K.TRACE_FROM_UNSAMPLED };
  const WATCH_MS = 15 * 60 * 1000; /* a watch ends by itself after this, unless told otherwise */
  /* the kinds of a rule's or a watch's detail: "summary" | "decisions" | "full" | "off" | { kinds, values } */
  function detailKinds(detail, values) {
    let kinds;
    if (detail === 'off') kinds = 0;
    else if (detail === undefined) kinds = DETAIL.full;
    else if (typeof detail === 'string') {
      kinds = DETAIL[detail];
      if (kinds === undefined) throw new TypeError(`detail: "summary", "decisions", "full" or "off", not ${detail}`);
    } else if (detail && Array.isArray(detail.kinds)) {
      kinds = kindsMask(detail.kinds);
      values = values || detail.values;
    } else throw new TypeError('detail: "summary", "decisions", "full", "off" or { kinds, values }');
    return (values ? kinds | K.TRACE_VALUES : kinds) >>> 0;
  }
  const bitsOf = (names, table, what) =>
    [].concat(names === undefined ? [] : names).reduce((bits, name) => {
      if (!(name in table)) throw new TypeError(`${what}: one of ${Object.keys(table).join(', ')}, not ${name}`);
      return bits | table[name];
    }, 0);
  /* a rule as traceConfigure takes it; `classOf` gives a detail its class */
  function nativeRule(rule, classOf) {
    if (rule === null || typeof rule !== 'object') throw new TypeError('a trace rule is an object');
    let sample = K.TRACE_KEEP, parts = 0, whole = 0;
    if (rule.sample === true) sample = K.TRACE_SAMPLE;
    else if (rule.sample === false) sample = K.TRACE_DROP;
    else if (rule.sample !== undefined) {
      sample = K.TRACE_RATIO;
      parts = sampleParts(rule.sample);
      whole = SAMPLE_WHOLE;
    }
    const chart = rule.chart instanceof Chart ? rule.chart.name : rule.chart;
    return {
      at: bitsOf(rule.at, RULE_AT, 'at'),
      chart: chart === undefined ? '' : String(chart),
      actor: rule.actor === undefined ? '' : String(rule.actor),
      event: rule.event === undefined ? '' : String(rule.event),
      session: rule.session === undefined ? '0' : String(rule.session),
      from: bitsOf(rule.from, RULE_FROM, 'from'),
      sample,
      parts,
      whole,
      detail: rule.detail === undefined ? -1 : classOf(detailKinds(rule.detail, rule.values)),
      perSecond: rule.perSecond || 0,
      burst: rule.burst || 0,
    };
  }
  const METRIC_KINDS = { sum: K.OTLP_METRIC_SUM, gauge: K.OTLP_METRIC_GAUGE, histogram: K.OTLP_METRIC_HISTOGRAM };
  const METRIC_KIND_NAMES = Object.fromEntries(Object.entries(METRIC_KINDS).map(([k, v]) => [v, k]));
  /* tinyactors.run.duration: how long one run blocked the host's loop, in seconds */

  function traceKinds(options) {
    let kinds;
    if (options.kinds) kinds = kindsMask(options.kinds);
    else {
      kinds = DETAIL[options.detail || 'full'];
      if (kinds === undefined) throw new TypeError(`detail: "summary", "decisions" or "full", not ${options.detail}`);
    }
    return options.values ? (kinds | K.TRACE_VALUES) >>> 0 : kinds;
  }
  function sampleParts(sample) {
    if (sample === undefined) return SAMPLE_WHOLE;
    if (typeof sample !== 'number' || !(sample >= 0 && sample <= 1)) throw new RangeError('sample: a ratio from 0 to 1');
    return Math.round(sample * SAMPLE_WHOLE);
  }
  /* the trace ID generator's seed, as two 32-bit halves */
  function traceSeed(seed) {
    if (seed === undefined) {
      const words = new Uint32Array(2);
      if (globalThis.crypto && globalThis.crypto.getRandomValues) globalThis.crypto.getRandomValues(words);
      else for (let i = 0; i < 2; i++) words[i] = Math.floor(Math.random() * 2 ** 32);
      return [words[0], words[1]];
    }
    const value = BigInt.asUintN(64, BigInt(seed));
    return [Number(value >> 32n), Number(value & 0xffffffffn)];
  }

  /* concatenated protobuf messages of one type are one message (their repeated fields append) */
  function concatBytes(parts) {
    if (parts.length === 1) return parts[0];
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Uint8Array(n);
    n = 0;
    for (const p of parts) {
      out.set(p, n);
      n += p.length;
    }
    return out;
  }

  const fetchPost = (url, headers, body) =>
    globalThis.fetch(url, { method: 'POST', headers: { 'content-type': 'application/x-protobuf', ...headers }, body }).then((response) => {
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
    });

  /* Posts OTLP protobuf to /v1/traces, /v1/logs and /v1/metrics: gathered for
   * a moment, one request in flight per signal; failures are quiet, logged at
   * most once every ten seconds. */
  class Exporter {
    constructor(options) {
      const base = options.endpoint ? String(options.endpoint).replace(/\/+$/, '') : '';
      this.urls = {
        traces: options.tracesEndpoint || (base && `${base}/v1/traces`),
        logs: options.logsEndpoint || (base && `${base}/v1/logs`),
        metrics: options.metricsEndpoint !== undefined ? options.metricsEndpoint : base && `${base}/v1/metrics`, /* '': none */
      };
      this.headers = { ...(options.headers || {}) };
      this.post = options.post || platform.post || fetchPost;
      this.waiting = { traces: [], logs: [], metrics: [] };
      this.bytes = { traces: 0, logs: 0, metrics: 0 };
      this.busy = { traces: false, logs: false, metrics: false };
      this.outcome = { posted: 0, failed: 0, dropped: 0 }; /* bytes: tinyactors.telemetry.bytes */
      this.timer = null;
      this.lastFailure = -Infinity;
      this.failed = 0;
      this.idle = [];
    }
    add(signal, bytes) {
      if (!bytes || !this.urls[signal]) return;
      if (this.bytes[signal] + bytes.length > EXPORT_MAX_BYTES) {
        this.outcome.dropped += bytes.length;
        return this.fail(new Error(`more than ${EXPORT_MAX_BYTES} bytes waiting`));
      }
      this.waiting[signal].push(bytes);
      this.bytes[signal] += bytes.length;
      if (this.bytes[signal] >= EXPORT_BYTES) this.flush();
      else if (!this.timer) this.timer = platform.setTimer(() => this.flush(), EXPORT_DELAY_MS);
    }
    flush() {
      platform.clearTimer(this.timer);
      this.timer = null;
      for (const signal of ['traces', 'logs', 'metrics']) {
        if (this.busy[signal] || !this.waiting[signal].length) continue;
        const body = concatBytes(this.waiting[signal].splice(0));
        this.bytes[signal] = 0;
        this.busy[signal] = true;
        let sent;
        try {
          sent = Promise.resolve(this.post(this.urls[signal], this.headers, body));
        } catch (e) {
          sent = Promise.reject(e);
        }
        sent
          .then(
            () => {
              this.outcome.posted += body.length;
            },
            (e) => {
              this.outcome.failed += body.length;
              this.fail(e);
            },
          )
          .then(() => {
            this.busy[signal] = false;
            if (this.waiting[signal].length) this.flush();
            else if (this.quiet()) for (const resolve of this.idle.splice(0)) resolve();
          });
      }
    }
    fail(e) {
      this.failed++;
      const now = platform.now();
      if (now - this.lastFailure < FAILURE_LOG_MS) return;
      this.lastFailure = now;
      platform.log(`tinyactors: exporting telemetry failed (${this.failed} so far): ${e && e.message ? e.message : e}`);
    }
    quiet() {
      return Object.keys(this.waiting).every((signal) => !this.busy[signal] && !this.waiting[signal].length);
    }
    /* resolves once nothing waits and nothing is in flight */
    settled() {
      this.flush();
      if (Object.keys(this.busy).every((signal) => !this.busy[signal])) return Promise.resolve();
      return new Promise((resolve) => this.idle.push(resolve));
    }
  }

  const SPAN_KINDS = ['unspecified', 'internal', 'server', 'client', 'producer', 'consumer'];
  const STATUS_CODES = ['unset', 'ok', 'error'];
  function decodedValue(v) {
    if (v === null || typeof v !== 'object') return v;
    if (Array.isArray(v)) return v.map(decodedValue);
    if ('int' in v) return BigInt(v.int);
    if ('list' in v) return decodedAttributes(v.list);
    if ('bytes' in v) return v.bytes;
    return v;
  }
  function decodedAttributes(pairs) {
    const out = {};
    for (const [key, value] of pairs) out[key] = decodedValue(value);
    return out;
  }
  const millis = (ns) => Number(ns / 1000n) / 1000;

  /* Metrics as the native writer takes them (src/metrics.c, "metrics"). */
  function nativeMetrics(metrics) {
    if (!Array.isArray(metrics)) throw new TypeError('metricsToOTLP: snapshot.metrics must be an array of metrics');
    return metrics.map((m) => {
      const kind = METRIC_KINDS[m && m.kind];
      if (!kind || typeof m.name !== 'string' || !Array.isArray(m.points)) throw new TypeError('metricsToOTLP: a metric needs a name, a kind ("sum", "gauge" or "histogram") and points');
      return {
        name: m.name,
        unit: m.unit === undefined ? '' : String(m.unit),
        kind,
        points: m.points.map((p) => {
          const attributes = Object.entries(p.attributes || {}).map(([k, v]) => [String(k), String(v)]);
          if (kind !== METRIC_KINDS.histogram) return { attributes, value: Number(p.value), integer: Number.isInteger(p.value) };
          return {
            attributes,
            count: p.count,
            sum: p.sum,
            min: p.min,
            max: p.max,
            bounds: p.bounds || [],
            buckets: p.buckets || [],
            exemplars: (p.exemplars || []).map((e) => ({ traceID: e.traceID, spanID: e.spanID, time: e.time, value: e.value })),
          };
        }),
      };
    });
  }
  /* A decoded request (src/metrics.c, "decodeMetrics") as Metric objects. */
  function snapshotMetrics(decoded) {
    return decoded.map((m) => ({
      name: m.name,
      unit: m.unit,
      kind: METRIC_KIND_NAMES[m.kind] || 'gauge',
      points: m.points.map((p) => {
        const point = { attributes: Object.fromEntries(p.attributes.map(([k, v]) => [k, String(v)])) };
        if (p.value !== undefined) return { ...point, value: p.value };
        const out = { ...point, count: p.count, sum: p.sum, bounds: p.bounds, buckets: p.buckets };
        if (p.min !== undefined) Object.assign(out, { min: p.min, max: p.max });
        out.exemplars = p.exemplars.map((e) => {
          let bucket = p.bounds.findIndex((b) => e.value <= b);
          if (bucket < 0) bucket = p.bounds.length;
          return { bucket, value: e.value, time: e.time, traceID: e.traceID, spanID: e.spanID };
        });
        return out;
      }),
    }));
  }
  /* Attributes as [[key, value]] of strings. */
  const resourcePairs = (resource) => Object.entries(resource || {}).map(([k, v]) => [String(k), String(v)]);

  /** One OTLP ExportMetricsServiceRequest (protobuf, for /v1/metrics) of a metrics snapshot (ta_otlp_add_metric). */
  function metricsToOTLP(snapshot, options = {}) {
    if (snapshot === null || typeof snapshot !== 'object' || !Array.isArray(snapshot.metrics)) throw new TypeError('metricsToOTLP(snapshot): expected { startTime, time, metrics }');
    const time = snapshot.time === undefined ? wallClock() : Number(snapshot.time);
    const start = snapshot.startTime === undefined ? time : Number(snapshot.startTime);
    return native.metricsToOTLP(nativeMetrics(snapshot.metrics), resourcePairs(options.resource), start, time);
  }

  /** Reads OTLP protobuf trace bytes (the ta_otlp_next_* readers); throws on a malformed message. */
  function decodeTraces(bytes) {
    return native.decodeTraces(bytes).map((s) => {
      const start = BigInt(s.start), end = BigInt(s.end);
      const span = {
        traceID: s.traceID,
        spanID: s.spanID,
        name: s.name,
        kind: SPAN_KINDS[s.kind] || 'unspecified',
        startTime: millis(start),
        endTime: millis(end),
        startTimeUnixNano: start,
        endTimeUnixNano: end,
        attributes: decodedAttributes(s.attributes),
        events: s.events.map((e) => {
          const time = BigInt(e.time);
          return { name: e.name, time: millis(time), timeUnixNano: time, attributes: decodedAttributes(e.attributes) };
        }),
        links: s.links.map((l) => ({ traceID: l.traceID, spanID: l.spanID, attributes: decodedAttributes(l.attributes) })),
        status: s.statusMessage ? { code: STATUS_CODES[s.status] || 'unset', message: s.statusMessage } : { code: STATUS_CODES[s.status] || 'unset' },
        resource: { attributes: decodedAttributes(s.resource) },
        scope: { name: s.scopeName, version: s.scopeVersion, attributes: decodedAttributes(s.scopeAttributes) },
      };
      if (s.parentSpanID) span.parentSpanID = s.parentSpanID;
      return span;
    });
  }

  /* --------------------------------------------------------------- systems */

  /* a snapshot: a magic line, the header's length (4 bytes), the header (JSON), then its blobs */
  const SNAPSHOT_MAGIC = utf8.encode('tinyactors-snapshot 2\n');
  function pack(header, blobs) {
    const head = utf8.encode(JSON.stringify({ ...header, blobs: blobs.map((b) => (b ? b.length : -1)) }));
    const total = SNAPSHOT_MAGIC.length + 4 + head.length + blobs.reduce((n, b) => n + (b ? b.length : 0), 0);
    const out = new Uint8Array(total);
    let at = 0;
    out.set(SNAPSHOT_MAGIC, at);
    at += SNAPSHOT_MAGIC.length;
    new DataView(out.buffer).setUint32(at, head.length);
    at += 4;
    out.set(head, at);
    at += head.length;
    for (const b of blobs) if (b) out.set(b, (at += b.length) - b.length);
    return out;
  }
  function unpack(bytes) {
    const bad = () => new TypeError('not a system snapshot (system.snapshot())');
    if (!(bytes instanceof Uint8Array) || bytes.length < SNAPSHOT_MAGIC.length + 4) throw bad();
    for (let i = 0; i < SNAPSHOT_MAGIC.length; i++) if (bytes[i] !== SNAPSHOT_MAGIC[i]) throw bad();
    let at = SNAPSHOT_MAGIC.length;
    const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(at);
    at += 4;
    const header = JSON.parse(new TextDecoder().decode(bytes.subarray(at, at + length)));
    at += length;
    const blobs = header.blobs.map((n) => (n < 0 ? null : bytes.subarray(at, (at += n))));
    return { header, blobs };
  }

  const ROUTE_OUTCOMES = ['delivered', 'relayed', 'duplicate', 'refused'];
  const REASONS = ['', 'unknown-target', 'unreachable', 'denied', 'expired', 'hops', 'malformed'];

  class System {
    /**
     * A new system: one ta_host with its datamodel and run loop.
     *   systemID      its id in addresses (tinyactors://<systemID>/...)
     *   ecmascript    'native' (JS values, c.expr functions) or 'quickjs' (the C datamodel)
     *   clock         () => ms: time moves only in flush()
     *   ioProcessors  { [type]: { location, sync, send(message, context) } }; a sync
     *                 processor's send runs inside the run, before the sender goes on
     *   invokers      { [type]: { start(invocation, context), receive?(invokeid, event) } }
     *   logger        (label, text) => void, for <log> and exceptions in expressions
     *   hibernation   { store }: get/set/delete of blobs by session (a Map will do)
     *   restore       system.snapshot()'s bytes; `charts` gives c.expr charts their functions
     *   capacities    { maxActors, maxEvents, maxTimers, stringBytes } of each segment
     *   traces, telemetry   trace sink and exporter (docs/adr/0006, 0007)
     */
    constructor(options = {}) {
      this.options = options;
      const restored = options.restore ? unpack(options.restore) : null;
      this.id = (restored && restored.header.systemID) || options.systemID || randomID();
      this._ecmascript = (restored && restored.header.ecmascript) || options.ecmascript || 'native';
      if (this._ecmascript !== 'native' && this._ecmascript !== 'quickjs') throw new TypeError('ecmascript: "native" or "quickjs"');
      this._charts = new Map(); /* Chart -> its name in this system */
      this._sources = new Map(); /* source name -> Chart */
      this._segments = []; /* by segment: its Image */
      this._actors = new Map(); /* session -> Actor the application holds */
      this._invocations = new Map(); /* `${segment}:${token}` and `${session}#${invokeid}` -> invocation */
      this._jobs = new Map(); /* migration job -> { resolve, moved } */
      this._hibernated = new Set();
      this._pending = new Map(); /* session -> blob while the store writes it */
      this._waking = new Set();
      this._woken = []; /* [session, blob] loaded, to wake */
      this._store = options.hibernation ? options.hibernation.store : null;
      if (this._store && typeof this._store.get !== 'function') throw new TypeError('hibernation: { store } with get, set and delete');
      this._peers = new Map(); /* router peer -> transport connection */
      this._links = new Set();
      this._stopped = false;
      this._timer = null; /* the one timer the addon arms (callbacks.schedule) */
      this._immediate = null;
      this._clockTimers = [];
      this._abort = new AbortController();
      /* I/O processors and invokers: one entry of ta_host_config.io_processors each
       * (an invoker's type too: SCXML sends to an invoked service with its type) */
      const processors = options.ioProcessors || {};
      const invokers = options.invokers || {};
      this._io = Object.entries(processors).map(([type, p]) => ({ type, location: p.location === undefined ? '' : String(p.location), sync: !!p.sync, processor: p, invoker: invokers[type] }));
      for (const [type, invoker] of Object.entries(invokers)) if (!processors[type]) this._io.push({ type, location: '', invoker });
      this._invokers = invokers;
      if (this._io.length > 16) throw new RangeError('at most 16 I/O processors and invokers');
      const callbacks = {
        resolve: (source, texts, kinds) => this._resolve(source, texts, kinds),
        effects: (effects, changed, traces, logs) => this._effects(effects, changed, traces, logs),
        /* a send to a sync processor, inside the run: what it sends comes before the sender goes on */
        sync: (e) => this._ioSend(e),
        ref: (address) => this.ref(address),
        schedule: (ms) => this._schedule(ms),
        clock: platform.wallClock || wallClock,
      };
      if (options.clock) callbacks.now = options.clock; /* own clock: time moves in flush() */
      const traces = options.traces || {};
      const config = {
        systemID: this.id,
        ecmascript: this._ecmascript,
        ioProcessors: this._io.map(({ type, location, sync }) => ({ type, location, sync })),
        seed: traceSeed(traces.seed),
        ...(options.capacities || {}),
      };
      if (restored) {
        const { header, blobs } = restored;
        const images = blobs.slice(2).map((b) => (b ? native.openImage(b) : null));
        images.forEach((image, segment) => image && this._register(segment, new Image(image), options.charts || []));
        config.now = header.now;
        this._sys = native.restoreSystem(callbacks, config, images, blobs[0], blobs[1]);
      } else {
        this._sys = native.createSystem(callbacks, config);
      }
      /* tracing: the platform's exporter settings (Node.js: OTEL_* variables), the application's over them */
      const telemetry = options.telemetry === false ? { disabled: true } : { ...(platform.telemetry || {}), ...(options.telemetry || {}) };
      this._telemetry = telemetry;
      this._exporter = !telemetry.disabled && (telemetry.endpoint || telemetry.tracesEndpoint || telemetry.metricsEndpoint) ? new Exporter(telemetry) : null;
      this._traceSink = null;
      this._traceOptions = {};
      this._watches = [];
      this._traceApply();
      if (traces.sink) this.setTraceSink(traces.sink, traces);
      this._clock = platform.wallClock || wallClock;
      this._metricsStart = this._clock();
      const interval = telemetry.metricsInterval === undefined ? METRICS_INTERVAL_MS : Number(telemetry.metricsInterval);
      this._metricsTimer = this._exporter && this._exporter.urls.metrics && interval > 0 ? platform.repeat(() => this._exportMetrics(), interval) : null;
      if (restored) native.pump(this._sys);
    }

    /* ---- the run loop is the addon's (src/system.c): it arms this one timer */

    _now() {
      return native.now(this._sys);
    }

    /* callbacks.schedule: 0 run soon, -1 nothing to do, else in `ms` */
    _schedule(ms) {
      platform.clearTimer(this._timer);
      this._timer = null;
      if (this._immediate !== null) platform.cancelDefer(this._immediate);
      this._immediate = null;
      const tick = () => {
        this._timer = this._immediate = null;
        if (!this._stopped) native.pump(this._sys, true);
      };
      if (ms === 0) this._immediate = platform.defer(tick);
      else if (ms > 0) this._timer = platform.setTimer(tick, ms);
    }

    /**
     * Run everything due at the system's clock (events, timers, eventless
     * chains) until idle, or `maxRounds` pumps. With a `clock` option this is
     * how time moves: advance the clock, then flush(). Returns true when idle.
     */
    flush(maxRounds = 10000) {
      if (this._stopped) return false;
      for (let i = 0; i < maxRounds; i++) {
        this._expire();
        if (native.pump(this._sys) !== -2) {
          if (!this._clockTimers.some((t) => t.at <= this._now())) return true;
        }
      }
      return false;
    }

    /* Run `fn` when this system's clock passes `at`; returns a cancel function
     * (link resumption, whose window follows the router's queue TTL). */
    _at(at, fn) {
      if (!this.options.clock) {
        const t = platform.setTimer(fn, Math.max(0, at - this._now()));
        if (t && typeof t.unref === 'function') t.unref();
        return () => platform.clearTimer(t);
      }
      const entry = { at, fn };
      this._clockTimers.push(entry);
      return () => {
        const i = this._clockTimers.indexOf(entry);
        if (i >= 0) this._clockTimers.splice(i, 1);
      };
    }

    _expire() {
      const now = this._now();
      const due = this._clockTimers.filter((t) => t.at <= now);
      this._clockTimers = this._clockTimers.filter((t) => t.at > now);
      for (const t of due) t.fn();
    }

    /* ---- charts */

    /* the functions of a chart's expressions: its c.expr functions, else its text compiled */
    _resolve(source, texts, kinds) {
      const chart = this._sources.get(source);
      const expressions = chart ? chart.expressions : null;
      return texts.map((text, e) => {
        if (text === undefined) return undefined;
        const f = expressions && kinds[e] !== LOCATION ? expressions.get(text) : undefined;
        return f || compileExpression(text, kinds[e]);
      });
    }

    _register(segment, image, charts) {
      this._segments[segment] = image;
      for (const c of image._info.charts) {
        let chart = this._sources.get(c.source);
        if (!chart) {
          const base = c.source.replace(/~\d+$/, '');
          chart = charts.find((x) => x.name === base || x.name === c.name);
          if (chart) this._sources.set(c.source, chart);
        }
        if (chart && !this._charts.has(chart)) this._charts.set(chart, c.name);
      }
    }

    /** Compile charts into one image (ta_compile_charts) and add it (ta_host_add_image):
     * their names (<scxml name>) get these versions. `documents` answer src attributes. */
    load(charts, options = {}) {
      this._compile(charts, options, (add) => add());
    }

    /* compile, then `add` the image: now (inside a run of _do) or through _do */
    _compile(charts, options, then) {
      if (this._stopped) throw new Error('the system was stopped');
      const list = [].concat(charts);
      for (const c of list) if (!(c instanceof Chart)) throw new TypeError('load(charts): charts from fromSCXML() or scxml.chart()');
      const sources = list.map((chart) => {
        const base = String(chart.name || 'chart').replace(/[^\w.-]/g, '_');
        let name = base;
        for (let n = 2; this._sources.has(name); n++) name = `${base}~${n}`;
        this._sources.set(name, chart);
        return { name, xml: chart.xml };
      });
      const documents = Object.assign({}, ...list.map((c) => c.documents), options.documents || {});
      /* SCXML documents are charts of the image too: an <invoke srcexpr> names them at run time */
      const documentCharts = Object.entries(documents).filter(([name, xml]) => /<scxml[\s>]/.test(xml) && !sources.some((s) => s.name === name)).map(([name, xml]) => ({ name, xml }));
      let handle;
      try {
        handle = native.compile([...sources, ...documentCharts], Object.entries(documents).map(([name, xml]) => ({ name, xml })));
      } catch (e) {
        for (const s of sources) this._sources.delete(s.name);
        throw e;
      }
      const image = new Image(handle);
      const add = () => {
        const segment = native.addImage(this._sys, handle);
        this._register(segment, image, []);
        for (let i = 0; i < list.length; i++) {
          const c = image._info.charts.find((x) => x.source === sources[i].name);
          if (c) this._charts.set(list[i], c.name);
        }
      };
      then(add);
    }

    /** Boot a <system> manifest (ta_compile_system, ta_host_boot): its actors, by name. */
    boot(manifest, options = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      const handle = native.compileSystem(String(manifest), Object.entries(options.documents || {}).map(([name, xml]) => ({ name, xml })));
      const image = new Image(handle);
      const out = {};
      const segment = native.addImage(this._sys, handle);
      this._register(segment, image, []);
      native.boot(this._sys, segment);
      for (const { name } of image.actors) {
        const session = native.actorByName(this._sys, name);
        if (session) out[name] = this._keep(session, { systemID: name });
      }
      native.pump(this._sys);
      return out;
    }

    /* ---- actors */

    /* the Actor of a session, kept while it runs */
    _keep(session, options = {}) {
      let actor = this._actors.get(session);
      if (!actor) {
        actor = new Actor(this, null, options);
        actor._session = session;
        actor._started = true;
        this._actors.set(session, actor);
      }
      return actor;
    }

    _spawn(actor) {
      if (native.later(this._sys, () => this._spawn(actor))) return; /* from an expression */
      const chart = actor.chart;
      if (!this._charts.has(chart)) this._compile([chart], {}, (add) => add());
      const at = native.chart(this._sys, this._charts.get(chart));
      if (!at) throw new Error(`no chart named "${this._charts.get(chart)}"`);
      const o = actor.options;
      actor._session = native.spawn(this._sys, at[0], at[1], o.systemID, o.input, o.traceparent);
      this._actors.set(actor._session, actor);
      native.pump(this._sys);
    }

    /** The actor spawned with `systemID` (ta_host_actor_by_name). */
    get(systemID) {
      if (this._stopped) return undefined;
      const session = native.actorByName(this._sys, String(systemID));
      return session ? this._actors.get(session) || this._keep(session, { systemID }) : undefined;
    }

    /** Every actor of the system, invoked ones included (ta_host_actors). */
    actors() {
      if (this._stopped) return [];
      return native.actors(this._sys).map((session) => {
        const known = this._actors.get(session);
        if (known) return known;
        const actor = new Actor(this, null, {});
        actor._session = session;
        actor._started = true;
        return actor;
      });
    }

    /** Send many events in one call (ta_host_send_many): [actor, event, { traceparent }?]
     * items; returns how many were taken. */
    sendMany(items) {
      const sessions = [], names = [], datas = [], traceparents = [];
      for (const [actor, ev, options] of items) {
        const event = typeof ev === 'string' ? { type: ev } : ev;
        if (!(actor instanceof Actor) || actor._status !== 'active' || !actor._session) continue;
        sessions.push(actor._session);
        names.push(String(event.type));
        datas.push(event);
        traceparents.push((options && options.traceparent) || undefined);
      }
      if (this._stopped || native.later(this._sys, () => this.sendMany(items))) return 0;
      return native.sendMany(this._sys, sessions, names, datas, traceparents);
    }

    /**
     * An event for an address (ta_host_send with every field of ta_event_in:
     * sendid, origin, origintype, raw, traceparent); an address of another
     * system goes through the router (ta_host_deliver).
     */
    send(address, event, options = {}) {
      if (typeof event === 'string') event = { type: event };
      const target = address instanceof Actor || address instanceof ActorRef ? address.address : String(address);
      const a = parseAddress(target);
      if (!a) throw new TypeError(`send: not an address: ${target}`);
      if (a.system !== this.id) return this.deliver({ target, name: event.type, data: event, ...options });
      if (this._stopped || native.later(this._sys, () => this.send(address, event, options))) return undefined;
      const session = a.kind === 'session' ? Number(a.session) : native.actorByName(this._sys, a.name);
      native.post(this._sys, session, { ...options, name: String(event.type), data: event });
      return undefined;
    }

    /** An envelope (docs/protocol.md) from host code to the router (ta_host_deliver, peer 0):
     * 'delivered', 'relayed', 'duplicate', 'refused' ('queued' inside a run). */
    deliver(envelope) {
      if (!envelope || typeof envelope.target !== 'string' || typeof envelope.name !== 'string') throw new TypeError('deliver({ target, name, ... })');
      if (this._stopped) return 'refused';
      if (native.later(this._sys, () => this.deliver(envelope))) return 'queued';
      return ROUTE_OUTCOMES[native.deliver(this._sys, 0, { origin: formatAddress(this.id, 'host'), ...envelope })] || 'refused';
    }

    /** A reference to the actor at `address`: an Actor of this system, else an ActorRef. */
    ref(address) {
      if (address instanceof Actor || address instanceof ActorRef) return address;
      const a = parseAddress(address);
      if (a && a.system === this.id && a.kind === 'session') {
        const known = this._actors.get(Number(a.session));
        if (known) return known;
      }
      return new ActorRef(this, String(address));
    }

    /* ---- what the core did (one call per pump) */

    _effects(effects, changed, traces, logs) {
      for (const e of effects || NONE) {
        try {
          this._effect(e);
        } catch (err) {
          platform.log(err);
        }
      }
      for (const session of changed || NONE) {
        const actor = this._actors.get(session);
        if (actor && actor._observers.size && actor._status === 'active') actor._notify();
      }
      if (traces || logs) this._traces(traces, logs);
    }

    _logger(session) {
      const actor = this._actors.get(session);
      return (actor && actor.options.logger) || this.options.logger || platform.log;
    }

    _effect(e) {
      switch (e.kind) {
        case 'log': {
          const logger = this._logger(e.root);
          if (e.label) logger(e.label, e.text);
          else logger(e.text);
          break;
        }
        case 'exception':
          this._logger(e.session)('error.execution', e.error);
          break;
        case 'done':
        case 'error': {
          const actor = this._actors.get(e.session);
          this._actors.delete(e.session);
          if (actor) actor._end(e);
          break;
        }
        case 'send':
          this._ioSend(e);
          break;
        case 'invoke':
          this._invoke(e);
          break;
        case 'uninvoke': {
          const inv = this._invocations.get(`${e.segment}:${e.token}`);
          if (inv) this._uninvoke(inv);
          break;
        }
        case 'wake':
          this._wake(e.session);
          break;
        case 'migrated': {
          const job = this._jobs.get(e.job);
          if (job) job.moved++;
          break;
        }
        case 'job': {
          const job = this._jobs.get(e.job);
          if (!job) break;
          this._jobs.delete(e.job);
          const pending = e.pending.map((s) => this._actors.get(s) || this.ref(formatAddress(this.id, s)));
          platform.defer(() => job.resolve({ migrated: e.migrated, failed: e.failed, pending }));
          break;
        }
        case 'freed':
          this._segments[e.segment] = undefined;
          break;
        case 'peer': {
          const conn = this._peers.get(e.peer);
          if (conn) conn.envelope(e);
          break;
        }
        case 'peerError': {
          const conn = this._peers.get(e.peer);
          if (conn) conn.post({ t: 'err', origin: e.origin, msgid: e.msgid, sendid: e.sendid, reason: REASONS[e.reason] || 'unreachable' });
          break;
        }
        default:
          break;
      }
    }

    /* ---- I/O processors and invokers */

    _post(session, fields) {
      if (!this._stopped) native.post(this._sys, session, fields);
    }

    /* TA_EFFECT_SEND for an I/O processor, or for an invoked service */
    _ioSend(e) {
      const io = this._io[e.ioProcessor];
      let inv = e.ioProcessor < 0 && e.token ? this._invocations.get(`${e.segment}:${e.token}`) : null;
      if (!inv && io && io.invoker && e.target.startsWith('#_')) inv = this._invocations.get(`${e.session}#${e.target.slice(2)}`);
      if (inv) {
        if (inv.invoker.receive) inv.invoker.receive(inv.invokeid, { name: e.event, data: e.data });
        return;
      }
      const fail = () => this._post(e.session, { flags: K.EVENT_IN_ERROR_COMMUNICATION, name: 'error.communication', sendid: e.sendid });
      if (!io || !io.processor) return fail();
      try {
        const sent = io.processor.send(
          { target: e.target, event: e.event, sendid: e.sendid, data: e.data, origin: formatAddress(this.id, e.session) },
          { system: this, signal: this._abort.signal, traceparent: e.traceparent },
        );
        if (sent && typeof sent.then === 'function') sent.then(undefined, fail);
      } catch {
        fail();
      }
    }

    /* TA_EFFECT_INVOKE: an <invoke type> the core does not run itself */
    _invoke(e) {
      const invoker = this._invokers[e.type];
      const inv = { session: e.session, segment: e.segment, token: e.token, invokeid: e.invokeid, invoker, closed: false, controller: new AbortController() };
      const post = (fields) => {
        if (inv.closed || this._stopped) return;
        this._post(inv.session, { ...fields, token: inv.token });
      };
      const fail = () => {
        post({ flags: K.EVENT_IN_ERROR_COMMUNICATION, name: 'error.communication' });
        this._uninvoke(inv);
      };
      if (!invoker || typeof invoker.start !== 'function') {
        this._post(e.session, { flags: K.EVENT_IN_ERROR_COMMUNICATION, name: 'error.communication', token: e.token });
        return;
      }
      this._invocations.set(`${e.segment}:${e.token}`, inv);
      this._invocations.set(`${e.session}#${e.invokeid}`, inv);
      const context = {
        system: this,
        signal: inv.controller.signal,
        traceparent: e.traceparent,
        send: (event, data) => post({ name: String(event), data, flags: K.EVENT_IN_FROM_INVOKE }),
        done: (data) => {
          post({ flags: K.EVENT_IN_INVOKE_DONE, name: `done.invoke.${inv.invokeid}`, data });
          this._uninvoke(inv);
        },
        fail,
      };
      try {
        const started = invoker.start({ invokeid: e.invokeid, type: e.type, src: e.src, params: e.params }, context);
        if (started && typeof started.then === 'function') started.then(undefined, fail);
      } catch {
        fail();
      }
    }

    _uninvoke(inv) {
      if (inv.closed) return;
      inv.closed = true;
      this._invocations.delete(`${inv.segment}:${inv.token}`);
      this._invocations.delete(`${inv.session}#${inv.invokeid}`);
      inv.controller.abort();
    }

    /* ---- hibernation (blobs go to options.hibernation.store) */

    /**
     * Hibernate actors that have been idle since the previous sweep
     * (ta_host_sweep, ta_host_hibernate), up to `max` slots scanned; returns
     * how many went to the store. An actor that cannot be stored stays.
     */
    sweep(max = 0) {
      if (!this._store) throw new Error('createSystem({ hibernation: { store } }) is needed to hibernate');
      if (this._stopped) return 0;
      let n = 0;
      for (const session of native.sweep(this._sys, max)) if (this._hibernate(session, false)) n++;
      return n;
    }

    /** Hibernate one actor now (throws, saying why, when it cannot be). */
    hibernate(actor) {
      if (!this._store) throw new Error('createSystem({ hibernation: { store } }) is needed to hibernate');
      if (this._stopped) return false;
      return this._hibernate(actor._session, true);
    }

    _hibernate(session, strict) {
      const actor = this._actors.get(session);
      const snapshot = actor ? actor._snapshot() : null;
      let blob;
      try {
        blob = native.hibernate(this._sys, session);
      } catch (e) {
        if (strict) throw e;
        return false;
      }
      if (actor) actor._cached = snapshot;
      this._hibernated.add(session);
      this._pending.set(session, blob);
      const key = String(session);
      const done = () => this._pending.get(session) === blob && this._pending.delete(session);
      const w = this._store.set(key, blob);
      if (w && typeof w.then === 'function') w.then(done, (e) => platform.log('hibernation store:', e));
      else done();
      return true;
    }

    /* TA_EFFECT_WAKE: load the blob (it has work, or a migration wants it);
     * the blobs loaded meanwhile go back in together, with one run */
    _wake(session) {
      if (this._waking.has(session) || !this._store) return;
      this._waking.add(session);
      const key = String(session);
      const finish = (blob) => {
        this._waking.delete(session);
        if (this._stopped) return;
        if (!blob) return platform.log(`tinyactors: cannot wake actor ${key} (no blob in the store)`);
        this._woken.push([session, blob]);
        if (this._woken.length === 1) this._wakeAll();
      };
      const blob = this._pending.get(session) || this._store.get(key);
      if (blob && typeof blob.then === 'function')
        blob.then(finish, (e) => {
          this._waking.delete(session);
          platform.log('hibernation store:', e);
        });
      else finish(blob);
    }

    _wakeAll() {
      for (const [session, blob] of this._woken.splice(0)) {
        /* awake before the run: subscribers hear of it with a fresh snapshot */
        this._hibernated.delete(session);
        this._pending.delete(session);
        const actor = this._actors.get(session);
        if (actor) actor._cached = null;
        try {
          native.wake(this._sys, session, blob);
        } catch (e) {
          platform.log(`tinyactors: ${e.message}`);
          continue;
        }
        const d = this._store.delete(String(session));
        if (d && typeof d.then === 'function') d.catch(() => {});
      }
      native.pump(this._sys);
    }

    /* ---- moving actors, versions, snapshots */

    /**
     * Move `actor` out (ta_host_export_actor): bytes that another system with
     * the same charts loaded takes in with adopt(). The actor stops here,
     * without a done event; its delayed events keep the time they had left.
     */
    export(actor) {
      if (!(actor instanceof Actor) || actor.system !== this) throw new TypeError('export() expects an actor of this system');
      const snapshot = actor._snapshot();
      const blob = native.exportActor(this._sys, actor._session);
      this._actors.delete(actor._session);
      actor._halt(snapshot);
      return blob;
    }

    /** Take in an actor another system exported (ta_host_adopt): the same session, states and variables. */
    adopt(blob) {
      const actor = this._keep(native.adopt(this._sys, blob));
      native.pump(this._sys);
      return actor;
    }

    /**
     * Live migration (ta_host_migrate): actors of older versions of a chart
     * move to its current version at their next stable point and receive
     * error.platform.upgrade. `target`: an actor, a chart or its name, or
     * nothing (every chart). Options: wake (load hibernated actors first),
     * timeoutMs. Returns { done } resolving with { migrated, failed, pending }.
     */
    migrate(target, options = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      const { wake = !!this._store, timeoutMs = 0 } = options;
      let name = '';
      let session = 0;
      if (target instanceof Actor) {
        const info = native.inspect(this._sys, target._session);
        name = info ? this._segments[info.segment]._info.charts[info.chart].name : '';
        session = target._session;
      } else if (target instanceof Chart) name = this._charts.get(target) || target.name || '';
      else if (target !== undefined && target !== null) name = String(target);
      const job = native.migrate(this._sys, name, session, !!wake, timeoutMs);
      const done = new Promise((resolve) => this._jobs.set(job, { resolve, moved: 0 }));
      native.pump(this._sys);
      return { done };
    }

    /**
     * The whole system as bytes (ta_host_snapshot, and the datamodel's
     * state): restore with createSystem({ restore, charts }). Needs a quiet
     * moment (no migration); hibernated actors stay in the store.
     */
    snapshot() {
      const s = native.snapshot(this._sys);
      const images = s.segments.map((image) => (image ? native.imageBytes(image) : null));
      return pack({ systemID: this.id, ecmascript: this._ecmascript, now: this._now() }, [s.host, s.datamodel, ...images]);
    }

    /** The host's counters and each chart's (ta_host_stats_get, ta_host_chart_stats). */
    stats() {
      return native.stats(this._sys);
    }

    /* ---- other systems: routing is the native router's (ta_host: peers, routes,
     * relaying, origin checks, policy, dedup — docs/protocol.md); transports
     * only move envelopes between it and the wire */

    _fromPeer(peer, msg) {
      if (this._stopped || !this._peers.has(peer)) return;
      if (msg.t === 'ev') {
        const str = (v) => (typeof v === 'string' ? v : '');
        native.deliver(this._sys, peer, {
          target: str(msg.target),
          name: str(msg.name),
          wire: msg.data === undefined ? undefined : JSON.stringify(msg.data),
          origin: str(msg.origin),
          origintype: str(msg.origintype),
          sendid: str(msg.sendid),
          msgid: str(msg.msgid),
          dedup: str(msg.dedup),
          hops: Number.isInteger(msg.hops) && msg.hops >= 0 ? msg.hops : 0,
          traceparent: str(msg.traceparent),
        });
      } else if (msg.t === 'err') {
        const reason = REASONS.indexOf(msg.reason);
        native.peerError(this._sys, peer, String(msg.origin || ''), String(msg.msgid || ''), String(msg.sendid || ''), reason > 0 ? reason : K.ROUTE_ERROR_UNREACHABLE);
        native.pump(this._sys);
      }
    }

    _attach(conn) {
      this._peers.set(conn.peer, conn);
    }

    _detach(conn) {
      if (this._peers.get(conn.peer) === conn) this._peers.delete(conn.peer);
    }

    _peerHello(conn, reach) {
      if (this._stopped) return;
      native.peerReach(this._sys, conn.peer, reach);
      native.peerUp(this._sys, conn.peer, true);
      native.pump(this._sys);
      this._announce();
    }

    _peerDown(conn, remove) {
      if (this._stopped) return;
      if (remove) {
        native.peerRemove(this._sys, conn.peer); /* queued envelopes fail: error.communication */
        this._detach(conn);
      } else native.peerUp(this._sys, conn.peer, false);
      native.pump(this._sys);
      this._announce();
    }

    /* the systems reachable through us, as announced to `conn` */
    _reachFor(conn) {
      const out = new Set();
      for (const c of new Set(this._peers.values())) {
        if (c === conn || !c.open || !c.remoteID) continue;
        out.add(c.remoteID);
        for (const r of c.reach) out.add(r);
      }
      out.delete(this.id);
      if (conn.remoteID) out.delete(conn.remoteID);
      const only = Array.isArray(conn.options.announce) ? new Set(conn.options.announce) : null;
      return [...out].filter((id) => !only || only.has(id)).sort();
    }

    _announce() {
      for (const c of new Set(this._peers.values())) if (c.announce && c.open) c.hello();
    }

    /**
     * Stop the whole system (ta_host_free): actors stop, links close, timers
     * end, invocations are aborted. The system cannot be used afterwards.
     */
    stop() {
      if (this._stopped) return;
      for (const c of [...this._links]) c.disconnect();
      if (this._metricsTimer !== null) platform.clearRepeat(this._metricsTimer);
      if (this._exporter && this._exporter.urls.metrics) this._exportMetrics(); /* what was counted up to now */
      if (this._traceSink || this._exporter) {
        const { traces, traceLogs } = native.traceEnd(this._sys); /* open spans end, incomplete */
        try {
          this._traces(traces, traceLogs);
        } catch (e) {
          platform.log(e);
        }
        if (this._exporter) this._exporter.flush();
      }
      for (const w of this._watches) if (w.timer !== null) platform.clearRepeat(w.timer);
      this._stopped = true;
      platform.clearTimer(this._timer);
      if (this._immediate !== null) platform.cancelDefer(this._immediate);
      this._clockTimers = [];
      this._abort.abort();
      for (const inv of new Set(this._invocations.values())) inv.controller.abort();
      this._invocations.clear();
      native.destroy(this._sys); /* from an effect handler: when the pump ends */
      for (const actor of [...this._actors.values()]) actor._halt(null);
      this._actors.clear();
      for (const [, j] of this._jobs) j.resolve({ migrated: 0, failed: 0, pending: [] });
      this._jobs.clear();
    }

    /* ---- metrics (docs/adr/0007): core's counts, pulled; the runs' durations here */

    /** Metrics as plain data: per chart, the pools and memory, the run queue, traces. */
    metrics() {
      if (this._stopped) throw new Error('the system was stopped');
      const time = this._clock();
      const bytes = native.metrics(this._sys, nativeMetrics(this._ownMetrics()), [], this._metricsStart, time);
      return { startTime: this._metricsStart, time, metrics: snapshotMetrics(native.decodeMetrics(bytes)) };
    }

    _ownMetrics() {
      const r = native.runs(this._sys);
      const runs = { attributes: {}, count: r.count, sum: r.sum, bounds: r.bounds, buckets: r.buckets, exemplars: [] };
      if (r.count) Object.assign(runs, { min: r.min, max: r.max });
      const own = [{ name: 'tinyactors.run.duration', unit: 's', kind: 'histogram', points: [runs] }];
      if (this._exporter) {
        const o = this._exporter.outcome;
        own.push({
          name: 'tinyactors.telemetry.bytes',
          unit: 'By',
          kind: 'sum',
          points: Object.keys(o).map((outcome) => ({ attributes: { 'tinyactors.telemetry.outcome': outcome }, value: o[outcome] })),
        });
      }
      return own;
    }

    _resource() {
      return { 'service.name': 'tinyactors', ...(this._telemetry.resource || {}), ...(this._traceOptions.resource || {}), 'tinyactors.system.id': this.id };
    }

    _exportMetrics() {
      if (!this._exporter) return;
      try {
        const own = this._ownMetrics();
        const app = this._telemetry.metrics ? this._telemetry.metrics() || [] : [];
        const bytes = native.metrics(this._sys, nativeMetrics([...own, ...app]), resourcePairs(this._resource()), this._metricsStart, this._clock());
        this._exporter.add('metrics', bytes);
      } catch (e) {
        platform.log(e);
      }
    }

    /* ---- tracing */

    /**
     * Hand traces to `sink` as OTLP protobuf: per run, a Uint8Array with an
     * ExportTraceServiceRequest and one with an ExportLogsServiceRequest (or
     * undefined). null removes it. Options: detail, kinds, values,
     * valueLimit, sample, parent ("cause" | "link"), resource, rules.
     */
    setTraceSink(sink, options = {}) {
      if (sink !== null && sink !== undefined && typeof sink !== 'function') throw new TypeError('setTraceSink(sink: (traces, logs) => void, options)');
      if (this._stopped) throw new Error('the system was stopped');
      this._traceSink = sink || null;
      this._traceOptions = { ...options };
      delete this._traceOptions.sink;
      delete this._traceOptions.seed;
      this._traceApply();
    }

    /** Change detail, kinds, sample and rules while traces flow (ta_host_trace_set); open spans are kept. */
    configureTraces({ detail, kinds, sample, rules } = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      const next = { ...this._traceOptions };
      if (detail !== undefined || kinds !== undefined) {
        next.detail = detail;
        next.kinds = kinds;
      }
      if (sample !== undefined) next.sample = sample;
      if (rules !== undefined) next.rules = rules;
      const previous = this._traceOptions;
      this._traceOptions = next;
      try {
        this._traceApply();
      } catch (e) {
        this._traceOptions = previous;
        this._traceApply();
        throw e;
      }
    }

    /**
     * Trace matching steps in full, sampled whatever the ratio, and what they
     * cause (docs/adr/0007): steps of these actors, sessions, actor names (a
     * trailing "*" a prefix), charts, or events. Ends after `for` ms (15
     * minutes; Infinity: until stop()). At most `perSecond` new traces a second.
     */
    watchTraces({ actors = [], sessions = [], names = [], charts = [], events = [], detail = 'full', values = false, for: ms = WATCH_MS, perSecond } = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      const watch = (match) => ({ ...match, at: ['start', 'take'], sample: true, detail, values, perSecond });
      const rules = [
        ...actors.map((a) => watch({ session: a instanceof Actor ? a._session : a })),
        ...sessions.map((session) => watch({ session })),
        ...names.map((actor) => watch({ actor })),
        ...charts.map((chart) => watch({ chart })),
        ...events.map((event) => watch({ event })),
      ];
      if (!rules.length) throw new TypeError('watchTraces: name actors, sessions, names, charts or events');
      const entry = { rules, timer: null };
      this._watches.push(entry);
      try {
        this._traceApply();
      } catch (e) {
        this._watches.splice(this._watches.indexOf(entry), 1);
        this._traceApply();
        throw e;
      }
      const handle = {
        stop: () => {
          const at = this._watches.indexOf(entry);
          if (at < 0) return;
          this._watches.splice(at, 1);
          if (entry.timer !== null) platform.clearRepeat(entry.timer);
          if (!this._stopped) this._traceApply();
        },
      };
      if (ms !== Infinity && ms > 0) entry.timer = platform.repeat(() => handle.stop(), ms);
      return handle;
    }

    /** Resolves once every trace export this system started has finished. */
    flushTraces() {
      return this._exporter ? this._exporter.settled() : Promise.resolve();
    }

    _traceApply() {
      const t = this._traceOptions;
      const exporter = this._exporter;
      const sample = t.sample !== undefined ? t.sample : this._telemetry.sample;
      let flags = 0;
      if (t.parent === 'link') flags |= K.OTLP_LINK_PARENTS;
      else if (t.parent !== undefined && t.parent !== 'cause') throw new TypeError('parent: "cause" or "link"');
      if (t.values) flags |= K.OTLP_VALUES;
      if (t.logs === false) flags |= K.OTLP_NO_LOGS;
      if (this._telemetry.spanMetrics) flags |= K.OTLP_SPAN_METRICS;
      /* classes: 0 unsampled (lifecycle only), 1 sampled, 2..7 what rules and watches ask for */
      const kinds = traceKinds(t);
      const classes = [kinds & K.TRACE_LIFECYCLE, kinds];
      const classOf = (mask) => {
        let at = classes.indexOf(mask, 2);
        if (at < 0) {
          if (classes.length === K.TRACE_DETAILS) throw new RangeError('traces: rules and watches ask for more than 6 different details');
          at = classes.push(mask) - 1;
        }
        return at;
      };
      const rules = [...this._watches.flatMap((w) => w.rules), ...(t.rules || [])].map((rule) => nativeRule(rule, classOf));
      if (classes.some((mask) => mask & K.TRACE_VALUES)) flags |= K.OTLP_VALUES;
      while (classes.length < K.TRACE_DETAILS) classes.push(0);
      /* with nowhere for records to go core writes none; contexts still flow (traceparents) */
      native.traceConfigure(this._sys, {
        enabled: !!(this._traceSink || (exporter && (exporter.urls.traces || exporter.urls.logs))),
        classes: classes.map((mask) => mask >>> 0),
        sampleParts: sampleParts(sample),
        sampleWhole: SAMPLE_WHOLE,
        rules,
        flags,
        valueLimit: t.valueLimit || 0,
        resource: Object.entries(this._resource()).map(([k, v]) => [String(k), String(v)]),
      });
    }

    _traces(traces, logs) {
      if (!traces && !logs) return;
      if (this._exporter) {
        this._exporter.add('traces', traces);
        this._exporter.add('logs', logs);
      }
      if (this._traceSink) this._traceSink(traces || new Uint8Array(0), logs);
    }
  }

  /** A new system (see System). */
  function createSystem(options) {
    return new System(options);
  }

  /* ---------------------------------------------------------------- actors */

  const STATUS = { done: 'done', error: 'error' };

  class Actor {
    constructor(system, chart, options = {}) {
      this.system = system;
      this.chart = chart;
      this.options = options;
      this._session = 0;
      this._started = false;
      this._status = 'active';
      this._output = undefined;
      this._error = undefined;
      this._final = undefined;
      this._context = undefined;
      this._cached = null; /* the snapshot while hibernated */
      this._observers = new Set();
      this._waiters = [];
    }

    /** Its session id (_sessionid), for its whole life. */
    get sessionID() {
      return this._session ? String(this._session) : undefined;
    }

    /** Its address (ta_address_format): tinyactors://<system>/#_scxml_<session>. */
    get address() {
      return this._session ? formatAddress(this.system.id, this._session) : undefined;
    }

    get hibernated() {
      return this.system._hibernated.has(this._session);
    }

    /** Spawn it (ta_host_spawn): its chart's current version, `input` overriding its <data>. */
    start() {
      if (this._started) return this;
      this._started = true;
      this.system._spawn(this);
      return this;
    }

    /** An event (ta_host_send): its name is event.type, its data the event. */
    send(event, options = {}) {
      if (typeof event === 'string') event = { type: event };
      if (!event || typeof event.type !== 'string') throw new TypeError('events are objects with a string "type"');
      const system = this.system;
      if (system._stopped || native.later(system._sys, () => this.send(event, options))) return;
      if (this._status !== 'active' || !this._session) return;
      native.send(system._sys, this._session, event.type, event, options.traceparent);
    }

    /** Stop it (ta_host_terminate). */
    stop() {
      const system = this.system;
      if (system._stopped || native.later(system._sys, () => this.stop())) return this;
      if (this._status !== 'active' || !this._session) return this;
      const snapshot = this._snapshot();
      system._actors.delete(this._session);
      this._halt(snapshot);
      native.terminate(system._sys, this._session);
      return this;
    }

    _snapshot() {
      const { _status: status, _output: output, _error: error } = this;
      if (status !== 'active') return { value: this._final === undefined ? {} : this._final, context: this._context, status, output, error };
      if (this._cached) return this._cached;
      const system = this.system;
      if (!this._session || system._stopped) return { value: {}, context: undefined, status, output, error };
      const context = native.context(system._sys, this._session);
      return { value: native.stateValue(system._sys, this._session), context: system._ecmascript === 'native' && context ? { ...context } : context, status, output, error };
    }

    /** { value, context, status, output, error }: computed when called. */
    getSnapshot() {
      return this._snapshot();
    }

    /** Called after each run in which its datamodel took an event, and when it ends. */
    subscribe(nextOrObserver, error, complete) {
      const o = typeof nextOrObserver === 'function' ? { next: nextOrObserver, error, complete } : nextOrObserver || {};
      this._observers.add(o);
      if (this._session) this.system._actors.set(this._session, this);
      return { unsubscribe: () => this._observers.delete(o) };
    }

    _notify() {
      const s = this._snapshot();
      for (const o of [...this._observers]) if (o.next) o.next(s);
    }

    /* TA_EFFECT_DONE / TA_EFFECT_ERROR */
    _end(e) {
      this._status = STATUS[e.kind];
      this._context = e.context && this.system._ecmascript === 'native' ? { ...e.context } : e.context;
      if (e.kind === 'done') {
        this._output = e.output;
        this._final = e.final;
      } else this._error = new Error(`actor fault ${e.code}`);
      const s = this._snapshot();
      for (const o of [...this._observers]) {
        if (e.kind === 'done') {
          if (o.next) o.next(s);
          if (o.complete) o.complete();
        } else if (o.error) o.error(this._error);
      }
      this._observers.clear();
      for (const w of this._waiters.splice(0)) w();
    }

    /* stopped from the outside (stop, export, the system's stop) */
    _halt(snapshot) {
      if (this._status !== 'active') return;
      this._status = 'stopped';
      if (snapshot) {
        this._final = snapshot.value;
        this._context = snapshot.context;
      }
      for (const o of [...this._observers]) if (o.complete) o.complete();
      this._observers.clear();
      for (const w of this._waiters.splice(0)) w();
    }

    toJSON() {
      return { $ref: this.address };
    }

    toString() {
      return this.address;
    }
  }

  /** A new actor of `chart` in `options.system` (start() spawns it): input, systemID, logger, traceparent. */
  function createActor(chart, options = {}) {
    if (!(chart instanceof Chart)) throw new TypeError('createActor(chart): a chart from fromSCXML() or scxml.chart()');
    if (!(options.system instanceof System)) throw new TypeError('createActor(chart, { system }): a system from createSystem()');
    return new Actor(options.system, chart, options);
  }

  /** Settles when the actor is done (with its output) or fails (TA_EFFECT_ERROR); a stopped actor rejects. */
  function toPromise(actor) {
    return new Promise((resolve, reject) => {
      const settle = () => {
        if (actor._status === 'done') resolve(actor._output);
        else reject(actor._error || new Error(`the actor ${actor._status === 'stopped' ? 'was stopped' : 'failed'}`));
      };
      if (actor._status !== 'active') settle();
      else actor._waiters.push(settle);
    });
  }

  /** An actor of another system, or one this system holds no Actor for: send() and its address. */
  class ActorRef {
    constructor(system, address) {
      this.system = system;
      this.address = address;
    }
    send(event, options) {
      return this.system.send(this.address, event, options);
    }
    toJSON() {
      return { $ref: this.address };
    }
    toString() {
      return this.address;
    }
  }

  /* ----------------------------------------------------------------- links
   * A link carries envelopes between two systems over a WebSocket-like
   * transport, speaking the link protocol v1 (docs/protocol.md, "Link
   * protocol"): hello {v, id, reach, caps, cred}, ev (an envelope), err,
   * ping/pong. Routing is the native router's: each connection is one router
   * peer (ta_host_peer_*); this code moves frames between it and the wire.
   * Envelopes come out of the addon as neutral JSON (ta_host_envelope_json). */
  const LINK_V = 1;
  const parseFrame = (text) => {
    try {
      const m = JSON.parse(text);
      return m && typeof m === 'object' && typeof m.t === 'string' ? m : null;
    } catch {
      return null;
    }
  };
  const systemIDValid = (id) => native.systemIDValid(id);
  const reachOf = (msg) => (Array.isArray(msg.reach) ? msg.reach.filter(systemIDValid) : []);

  /* what both sides of a link share: frames and envelopes */
  class PeerConn {
    constructor(system, options) {
      this.system = system;
      this.options = options;
      this.ws = null;
      this.open = false;
      this.peer = 0; /* the router's handle */
      this.remoteID = null;
      this.reach = [];
      this.announce = !!options.announce;
      this.announced = null;
      this.maxFrame = options.maxFrame || K.FRAME_MAX; /* what we accept, announced in our hello */
      this.peerMax = K.FRAME_MAX; /* what the peer accepts (its hello's caps.max_frame) */
    }
    negotiate(msg) {
      const caps = msg.caps && typeof msg.caps === 'object' ? msg.caps : {};
      this.peerMax = Number.isInteger(caps.max_frame) && caps.max_frame > 0 ? caps.max_frame : K.FRAME_MAX;
    }
    send(msg) {
      this.ws.send(JSON.stringify(msg));
    }
    hello() {
      const reach = this.announce ? this.system._reachFor(this) : [];
      const key = reach.join(' ');
      if (this.announced === key) return;
      this.announced = key;
      const msg = { t: 'hello', v: LINK_V, id: this.system.id, reach, caps: { profile: 'neutral', ordered: true, acks: false, max_frame: this.maxFrame } };
      if (this.options.credential !== undefined) msg.cred = this.options.credential;
      this.send(msg);
    }
    /* a TA_HOST_EFFECT_PEER effect: the envelope goes out on the wire */
    envelope(e) {
      const fail = (reason) => this.system._fromPeer(this.peer, { t: 'err', origin: e.origin, msgid: e.msgid, sendid: e.sendid, reason });
      if (e.malformed || !e.envelope) return fail('malformed'); /* data that is no neutral value */
      const text = `{"t":"ev",${e.envelope.slice(1)}`;
      if (text.length * 3 > this.peerMax && utf8.encode(text).length > this.peerMax) return fail('malformed');
      if (!this.post(null, text)) fail('unreachable');
    }
    post(msg, text = JSON.stringify(msg)) {
      if (!this.open || !this.ws) return false;
      this.ws.send(text);
      return true;
    }
    /* frames every connection handles alike; false: not one of them */
    common(ws, msg) {
      if (msg.t === 'hello') return false;
      if (msg.t === 'ping') ws.send(JSON.stringify({ t: 'pong', at: msg.at }));
      else if ((msg.t === 'ev' || msg.t === 'err') && this.peer && this.remoteID) this.system._fromPeer(this.peer, msg);
      return true; /* pong is the caller's; ack and unknown types are ignored */
    }
  }

  /* the client side: reconnects with a backoff while not closed */
  class LinkConn extends PeerConn {
    constructor(system, options) {
      super(system, options);
      this.alias = options.name || 'remote';
      if (!systemIDValid(this.alias)) throw new TypeError(`createLink: name "${this.alias}" is not a system id`);
      this.closed = false;
      this.pinger = null;
      this.retry = null;
      this.attempts = 0;
      this.error = null;
      this.opened = [];
      const flags = (options.trusted === false ? 0 : K.PEER_TRUSTED) | (options.maxQueue !== 0 ? K.PEER_QUEUE : 0);
      this.peer = native.peerAdd(system._sys, this.alias, [], flags, options.maxQueue || 1000, options.queueTTLMs || 0);
      if (!this.peer) throw new Error(`createLink: cannot add a peer "${this.alias}"`);
      native.peerUp(system._sys, this.peer, false); /* not connected yet: envelopes queue */
      system._attach(this);
      system._links.add(this);
    }
    connect() {
      this.retry = null;
      if (this.closed) return;
      let ws;
      try {
        ws = this.options.transport ? asTransport(this.options.transport(), this.maxFrame) : new WebSocket(this.options.url);
      } catch (e) {
        this.ws = null;
        platform.defer(() => this.dropped(null, String(e && e.message ? e.message : e)));
        return;
      }
      this.ws = ws;
      this.announced = null;
      ws.onopen = () => this.hello();
      ws.onmessage = (ev) => this.message(ws, String(ev.data));
      ws.onclose = () => this.dropped(ws, 'connection closed');
      ws.onerror = () => {}; /* followed by onclose */
    }
    message(ws, text) {
      if (ws !== this.ws) return;
      const msg = parseFrame(text);
      if (!msg || this.common(ws, msg)) {
        if (msg && msg.t === 'pong' && typeof msg.at === 'number') native.peerRoundTripTime(this.system._sys, this.peer, Math.max(0, Math.round(this.system._now() - msg.at)));
        return;
      }
      if (msg.v !== LINK_V || !systemIDValid(msg.id)) {
        ws.close();
        return;
      }
      const first = !this.open;
      this.remoteID = msg.id;
      this.reach = reachOf(msg);
      this.negotiate(msg);
      this.open = true;
      this.attempts = 0;
      this.error = null;
      this.system._peerHello(this, [msg.id, ...this.reach]);
      if (!first) return;
      for (const resolve of this.opened.splice(0)) resolve();
      const every = this.options.pingMs === undefined ? 5000 : this.options.pingMs;
      if (every > 0) {
        const ping = () => this.ws === ws && ws.send(JSON.stringify({ t: 'ping', at: this.system._now() }));
        ping();
        this.pinger = platform.repeat(ping, every);
      }
    }
    /* our name for the server (the link's alias) becomes its own id on the wire */
    envelope(e) {
      if (this.remoteID && this.alias !== this.remoteID && e.envelope) {
        const a = parseAddress(e.target);
        if (a && a.system === this.alias) {
          const env = JSON.parse(e.envelope);
          env.target = `tinyactors://${this.remoteID}/${a.local}`;
          return super.envelope({ ...e, envelope: JSON.stringify(env) });
        }
      }
      return super.envelope(e);
    }
    dropped(ws, error) {
      if (ws !== this.ws) return;
      this.ws = null;
      this.open = false;
      if (this.pinger !== null) platform.clearRepeat(this.pinger);
      this.pinger = null;
      if (this.closed) return;
      this.error = error;
      this.system._peerDown(this, false);
      this.attempts++;
      const ms = Math.min(30000, 250 * 2 ** Math.min(this.attempts - 1, 7));
      this.retry = this.system._at(this.system._now() + ms, () => this.connect());
    }
    disconnect() {
      if (this.closed) return;
      this.closed = true;
      this.open = false;
      if (this.pinger !== null) platform.clearRepeat(this.pinger);
      if (this.retry) this.retry();
      const ws = this.ws;
      this.ws = null;
      if (ws) ws.close();
      this.system._links.delete(this);
      if (!this.system._stopped) this.system._peerDown(this, true);
    }
  }

  const peerStatus = (system, conn, extra = {}) => {
    const s = conn.peer && !system._stopped ? native.peerStatus(system._sys, conn.peer) : null;
    return s ? { ...s, ...extra } : null;
  };

  /**
   * Link `system` to another system (a browser to its server): { url } (a
   * WebSocket) or { transport: () => a WebSocket-like object or a byte
   * stream }, `name` (our name for it), trusted, maxQueue, queueTTLMs,
   * announce, pingMs, credential, maxFrame. Returns { name, opened, get,
   * status, close }.
   */
  function createLink(system, options = {}) {
    if (!(system instanceof System)) throw new TypeError('createLink(system, options): system expected');
    if (!options.url && !options.transport) throw new TypeError('createLink: { url } or { transport } needed');
    const conn = new LinkConn(system, options);
    const opened = new Promise((resolve) => conn.opened.push(resolve));
    conn.connect();
    return {
      name: conn.alias,
      opened,
      /** A reference to `systemID` (an actor name) on the other side. */
      get: (systemID) => system.ref(formatAddress(conn.alias, systemID)),
      /** The router's view of the peer (ta_host_peer_status), and the last error. */
      status: () => peerStatus(system, conn, { error: conn.error, attempts: conn.attempts }),
      close: () => conn.disconnect(),
    };
  }

  /* exports: ['lobby', ...] or { lobby: 'JOIN LEAVE', ... } -> [name, events] */
  function exportsOf(x) {
    if (!x) return [];
    if (Array.isArray(x)) return x.map((n) => [String(n), '']);
    return Object.entries(x).map(([n, ev]) => [n, Array.isArray(ev) ? ev.join(' ') : String(ev || '')]);
  }

  /**
   * Accept links into `system`: `accept(onConnection)` registers a source of
   * open transports (WebSocket-like, or byte streams) and may return a
   * function that stops it. Untrusted peers (the default) may only send to
   * what `exports` lists (ta_host_export); trusted ones (`trusted: true`, or
   * `trust(hello)`) may send anything and relay. A peer that reconnects with
   * the same system id within `resumeMs` gets what was queued for it.
   */
  function serveLinks(system, options = {}) {
    if (typeof options.accept !== 'function') throw new TypeError('serveLinks(system, { accept }) needs an accept function here');
    const conns = new Set();
    const byID = new Map(); /* remote system id -> its connection (also while it is away) */
    const expiry = new Map(); /* remote system id -> cancels the end of its resumption window */
    const resumeMs = options.resumeMs === undefined ? 30000 : options.resumeMs;
    const rules = exportsOf(options.exports);
    const trusts = (hello) => (typeof options.trust === 'function' ? !!options.trust(hello) : !!options.trusted);
    const server = { closed: false };
    const forget = (c) => {
      const cancel = expiry.get(c.remoteID);
      if (cancel) cancel();
      expiry.delete(c.remoteID);
      if (byID.get(c.remoteID) === c) byID.delete(c.remoteID);
      system._peerDown(c, true);
    };
    const hello = (c, ws, msg) => {
      if (msg.v !== LINK_V || !systemIDValid(msg.id) || msg.id === system.id) return ws.close();
      c.reach = reachOf(msg);
      c.negotiate(msg);
      if (c.peer) return system._peerHello(c, c.reach); /* a repeated hello: new reach */
      c.remoteID = msg.id;
      const before = byID.get(msg.id);
      if (before) {
        /* the same system again: the newer connection wins and resumes its peer */
        const cancel = expiry.get(msg.id);
        if (cancel) cancel();
        expiry.delete(msg.id);
        if (before.ws && before.ws !== ws) {
          const old = before.ws;
          before.ws = null;
          before.open = false;
          conns.delete(before);
          old.close();
        }
        c.peer = before.peer;
      } else {
        const flags = (trusts(msg) ? K.PEER_TRUSTED : 0) | (resumeMs > 0 ? K.PEER_QUEUE : 0);
        c.peer = native.peerAdd(system._sys, msg.id, c.reach, flags, options.maxQueue || 1000, resumeMs);
        if (!c.peer) return ws.close();
        for (const [name, events] of rules) native.exportName(system._sys, c.peer, name, events);
      }
      byID.set(msg.id, c);
      system._attach(c);
      c.open = true;
      c.hello();
      system._peerHello(c, c.reach);
    };
    const onConnection = (transport) => {
      const ws = asTransport(transport, options.maxFrame || K.FRAME_MAX);
      if (server.closed) return ws.close();
      const c = new PeerConn(system, options);
      c.ws = ws;
      conns.add(c);
      ws.onmessage = (ev) => {
        if (c.ws !== ws) return;
        const msg = parseFrame(String(ev.data));
        if (msg && !c.common(ws, msg)) hello(c, ws, msg);
      };
      ws.onclose = () => {
        conns.delete(c);
        if (c.ws !== ws) return; /* replaced by a newer connection */
        c.ws = null;
        c.open = false;
        if (!c.peer || byID.get(c.remoteID) !== c) return;
        if (resumeMs > 0 && !server.closed) {
          system._peerDown(c, false); /* queue for it until it is back or resumeMs passed */
          const id = c.remoteID;
          expiry.set(id, system._at(system._now() + resumeMs, () => byID.get(id) === c && !c.ws && forget(c)));
        } else forget(c);
      };
    };
    const stop = options.accept(onConnection);
    return {
      connections: () => [...conns].filter((c) => c.open).map((c) => c.remoteID),
      status: (id) => (byID.get(id) ? peerStatus(system, byID.get(id)) : null),
      close: async () => {
        server.closed = true;
        for (const c of [...conns]) {
          const ws = c.ws;
          c.ws = null;
          c.open = false;
          if (ws) ws.close();
        }
        conns.clear();
        for (const c of [...byID.values()]) if (!system._stopped) forget(c);
        if (typeof stop === 'function') await stop();
      },
    };
  }

  /* A byte stream as a link transport (the WebSocket-like shape links use): a
   * Node-style duplex (a net.Socket) or a { readable, writable } pair (a child
   * process's stdio). Anything with send() is a transport already. */
  function asTransport(x, maxFrame) {
    return x && typeof x.send !== 'function' && (typeof x.write === 'function' || (x.writable && typeof x.writable.write === 'function'))
      ? streamTransport(x, maxFrame)
      : x;
  }

  function streamTransport(stream, maxFrame) {
    const pair = typeof stream.write !== 'function';
    const r = pair ? stream.readable : stream;
    const w = pair ? stream.writable : stream;
    const decoder = new FrameDecoder({ maxFrame });
    const t = { onopen: null, onmessage: null, onclose: null, onerror: null, readyState: 0 };
    let done = false;
    const data = (chunk) => {
      for (const f of decoder.push(chunk)) {
        if (done) return;
        if (typeof f === 'string') t.onmessage && t.onmessage({ data: f });
        else t.onerror && t.onerror(f); /* refused (skipped); the stream stays in step */
      }
    };
    const finish = () => {
      if (done) return;
      done = true;
      t.readyState = 3;
      r.off('data', data);
      queueMicrotask(() => t.onclose && t.onclose({}));
    };
    t.send = (text) => {
      if (!done) w.write(encodeFrame(text));
    };
    t.close = () => {
      if (done) return;
      try {
        w.end();
        if (pair && r.destroy) r.destroy();
      } catch {
        /* e.g. process.stdout cannot be ended */
      }
      finish();
    };
    r.on('data', data);
    for (const s of pair ? [r, w] : [r]) for (const ev of ['end', 'close', 'error']) s.on(ev, finish);
    if (r.destroyed || r.readableEnded) finish();
    else if (w.connecting) w.once('connect', () => !done && ((t.readyState = 1), t.onopen && t.onopen({})));
    else
      queueMicrotask(() => {
        if (done) return;
        t.readyState = 1;
        if (t.onopen) t.onopen({});
      });
    return t;
  }

  /* ------------------------------------------------------------ store links
   * A store link connects the system to a database of actors (docs/protocol.md,
   * "Store links": PostgreSQL's tinyactors extension) through SQL, with the
   * caller's client: `query(sql, params) -> Promise<rows>` and optionally
   * `listen(channel, onPayload) -> unlisten`. The database is one router peer:
   * envelopes for it go in with tinyactors.deliver(envelope) (in order,
   * retried until the database takes them: at-least-once, the database drops
   * repeats by msgid); envelopes from it are claimed from its outbox
   * (claim_for) when it NOTIFYs or every pollMs, handed to the router and acked. */
  class StoreConn extends PeerConn {
    constructor(system, options) {
      super(system, { ...options, announce: options.announce !== false });
      this.query = options.query;
      this.remoteID = options.id || null;
      this.worker = options.worker || `${system.id}/${randomID()}`;
      this.batch = options.batch || 100;
      this.out = []; /* frames for the database, in order */
      this.peerMax = Infinity; /* rows, not frames */
      this.pumping = false;
      this.draining = false;
      this.again = false;
      this.closed = false;
      this.error = null;
      this.idle = [];
      this.poller = null;
    }
    async start() {
      if (!this.remoteID) {
        const r = await this.query('SELECT tinyactors.system_id() AS id', []);
        this.remoteID = r[0].id;
      }
      if (!systemIDValid(this.remoteID)) throw new TypeError(`createStoreLink: "${this.remoteID}" is not a system id`);
      const flags = (this.options.trusted === false ? 0 : K.PEER_TRUSTED) | K.PEER_QUEUE;
      this.peer = native.peerAdd(this.system._sys, this.remoteID, [], flags, this.options.maxQueue || 10000, this.options.queueTTLMs || 0);
      if (!this.peer) throw new Error(`createStoreLink: cannot add a peer "${this.remoteID}"`);
      this.system._attach(this);
      this.system._links.add(this);
      this.open = true;
      this.hello(); /* tinyactors.hello(): what we reach is claimed by us */
      this.system._peerHello(this, []);
      if (this.options.listen) {
        this.unlisten = await this.options.listen('tinyactors_outbox', (payload) => {
          if (payload === this.system.id) this.drain();
        });
      }
      const every = this.options.pollMs === undefined ? 1000 : this.options.pollMs;
      if (every > 0) this.poller = platform.repeat(() => this.drain(), every);
      await this.drain();
    }
    send(msg) {
      this.out.push(msg);
      this.pump();
    }
    post(msg) {
      if (this.closed) return false;
      this.send(msg);
      return true;
    }
    envelope(e) {
      if (e.malformed || !e.envelope) return this.system._fromPeer(this.peer, { t: 'err', origin: e.origin, msgid: e.msgid, sendid: e.sendid, reason: 'malformed' });
      if (!this.post({ t: 'ev', envelope: e.envelope, origin: e.origin, msgid: e.msgid, sendid: e.sendid }))
        this.system._fromPeer(this.peer, { t: 'err', origin: e.origin, msgid: e.msgid, sendid: e.sendid, reason: 'unreachable' });
    }
    async pump() {
      if (this.pumping) return;
      this.pumping = true;
      try {
        while (this.out.length && !this.closed) {
          const msg = this.out[0];
          let r;
          try {
            r = await this.sql(msg);
          } catch (e) {
            this.failed(e); /* down: keep it, try again later */
            await new Promise((ok) => platform.setTimer(ok, 500));
            continue;
          }
          this.out.shift();
          this.recovered();
          if (msg.t === 'ev' && r !== 'ok' && r !== 'duplicate')
            this.system._fromPeer(this.peer, { t: 'err', origin: msg.origin, msgid: msg.msgid, sendid: msg.sendid, reason: REASONS.includes(r) ? r : 'unreachable' });
        }
      } finally {
        this.pumping = false;
        this.settled();
      }
    }
    async sql(msg) {
      if (msg.t === 'hello') {
        const r = await this.query('SELECT tinyactors.hello($1, $2::text[]) AS id', [msg.id, `{${msg.reach.join(',')}}`]);
        return r[0] && r[0].id;
      }
      if (msg.t === 'ev') {
        /* as this system: the database checks the origin against what we announced */
        const r = await this.query('SELECT tinyactors.deliver($1::jsonb, $2) AS r', [msg.envelope, this.system.id]);
        return r[0] && r[0].r;
      }
      if (msg.t === 'err') await this.query('SELECT tinyactors.deliver_error($1::jsonb)', [JSON.stringify({ origin: msg.origin, msgid: msg.msgid, sendid: msg.sendid, reason: msg.reason })]);
      return null;
    }
    /* claim what the database has for us (and the systems we announced) */
    async drain() {
      if (this.closed || !this.peer) return;
      if (this.draining) {
        this.again = true;
        return;
      }
      this.draining = true;
      try {
        do {
          this.again = false;
          const rows = await this.query('SELECT id, envelope FROM tinyactors.claim_for($1, $2, $3)', [this.system.id, this.batch, this.worker]);
          for (const row of rows) {
            const env = typeof row.envelope === 'string' ? JSON.parse(row.envelope) : row.envelope;
            this.system._fromPeer(this.peer, { ...env, t: 'ev' });
          }
          if (rows.length) await this.query('SELECT tinyactors.ack(id) FROM unnest($1::bigint[]) AS id', [`{${rows.map((r) => r.id).join(',')}}`]);
          if (rows.length === this.batch) this.again = true;
          this.recovered();
        } while (this.again && !this.closed);
      } catch (e) {
        this.failed(e);
      } finally {
        this.draining = false;
        this.settled();
      }
    }
    failed(e) {
      this.error = String(e && e.message ? e.message : e);
      if (this.open) {
        this.open = false;
        this.system._peerDown(this, false); /* the router queues meanwhile */
      }
    }
    recovered() {
      if (this.open || this.closed) return;
      this.open = true;
      this.error = null;
      this.announced = null;
      this.hello();
      this.system._peerHello(this, []);
    }
    settled() {
      if (!this.pumping && !this.draining && !this.out.length) for (const ok of this.idle.splice(0)) ok();
    }
    disconnect() {
      if (this.closed) return;
      this.closed = true;
      this.open = false;
      if (this.poller !== null) platform.clearRepeat(this.poller);
      if (this.unlisten) Promise.resolve(this.unlisten()).catch(() => {});
      this.system._links.delete(this);
      if (!this.system._stopped) this.system._peerDown(this, true);
      this.settled();
    }
  }

  /** Connect `system` to a database of actors (PostgreSQL + the tinyactors extension) through
   * your own client: `query(sql, params)` resolving to rows, optionally `listen(channel, onPayload)`. */
  function createStoreLink(system, options = {}) {
    if (!(system instanceof System)) throw new TypeError('createStoreLink(system, options): system expected');
    if (typeof options.query !== 'function') throw new TypeError('createStoreLink: { query(sql, params) } is needed');
    const conn = new StoreConn(system, options);
    const ready = conn.start();
    ready.catch(() => {});
    return {
      get id() {
        return conn.remoteID;
      },
      ready,
      /** Claim and deliver what the database has for us now. */
      drain: () => ready.then(() => conn.drain()),
      /** Resolves once everything handed to the link reached the database and nothing is being claimed. */
      flush: () =>
        ready.then(
          () =>
            new Promise((ok) => {
              conn.idle.push(ok);
              conn.settled();
            }),
        ),
      status: () => peerStatus(system, conn, { error: conn.error, pending: conn.out.length }),
      close: async () => conn.disconnect(),
    };
  }

  return {
    scxml: { chart },
    fromSCXML,
    Chart,
    createSystem,
    System,
    createActor,
    Actor,
    ActorRef,
    toPromise,
    loadImage,
    Image,
    createLink,
    serveLinks,
    createStoreLink,
    parseAddress,
    formatAddress,
    encodeFrame,
    FrameDecoder,
    parseTraceparent,
    formatTraceparent,
    stepSpanID,
    decodeTraces,
    metricsToOTLP,
    TRACE_KINDS,
  };
}

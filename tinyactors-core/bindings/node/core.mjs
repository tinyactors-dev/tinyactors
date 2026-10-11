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
  const TRACE_KINDS = native.traceKinds;
  const EXPORT_DELAY_MS = 250; /* bytes gather this long before a post */
  const EXPORT_BYTES = 1 << 20; /* or until this many wait */
  const EXPORT_MAX_BYTES = 16 << 20; /* waiting beyond this is dropped */
  const FAILURE_LOG_MS = 10000;
  const METRICS_INTERVAL_MS = 60000; /* OTEL_METRIC_EXPORT_INTERVAL's default */
  const METRIC_KINDS = { sum: K.OTLP_METRIC_SUM, gauge: K.OTLP_METRIC_GAUGE, histogram: K.OTLP_METRIC_HISTOGRAM };
  const METRIC_KIND_NAMES = Object.fromEntries(Object.entries(METRIC_KINDS).map(([k, v]) => [v, k]));
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
      const restored = options.restore ? native.snapshotContents(options.restore) : null;
      this.id = (restored && restored.systemID) || options.systemID || randomID();
      this._ecmascript = (restored && restored.ecmascript) || options.ecmascript || 'native';
      if (this._ecmascript !== 'native' && this._ecmascript !== 'quickjs') throw new TypeError('ecmascript: "native" or "quickjs"');
      this._charts = new Map(); /* Chart -> its name in this system */
      this._sources = new Map(); /* source name -> Chart */
      this._segments = []; /* by segment: its Image */
      this._actors = new Map(); /* session -> Actor the application holds */
      this._invocations = new Map(); /* `${segment}:${token}` and `${session}#${invokeid}` -> invocation */
      this._store = options.hibernation ? options.hibernation.store : null;
      if (this._store && typeof this._store.get !== 'function') throw new TypeError('hibernation: { store } with get, set and delete');
      this._links = new Set(); /* store links, closed with the system */
      this._transports = new Map(); /* link connection -> its transport */
      this._linkOpeners = new Map(); /* link -> () => a new transport */
      this._linkOpened = new Map(); /* link -> resolves its `opened` */
      this._linkServers = new Map(); /* accepted connection -> its server */
      this._linkJudges = new Map(); /* server -> trust(hello) */
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
        restored.images.forEach((image, segment) => image && this._register(segment, new Image(image), options.charts || []));
        config.now = restored.now;
        this._sys = native.restoreSystem(callbacks, config, restored.images, options.restore);
      } else {
        this._sys = native.createSystem(callbacks, config);
      }
      /* tracing: the platform's exporter settings (Node.js: OTEL_* variables), the application's over them */
      const telemetry = options.telemetry === false ? { disabled: true } : { ...(platform.telemetry || {}), ...(options.telemetry || {}) };
      this._telemetry = telemetry;
      this._exporter = !telemetry.disabled && (telemetry.endpoint || telemetry.tracesEndpoint || telemetry.metricsEndpoint) ? new Exporter(telemetry) : null;
      this._traceSink = null;
      this._traceOptions = {};
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
        case 'freed':
          this._segments[e.segment] = undefined;
          break;
        case 'link':
          this._linkEffect(e);
          break;
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
        blob = native.hibernate(this._sys, session); /* kept by the addon until stored */
      } catch (e) {
        if (strict) throw e;
        return false;
      }
      if (actor) actor._cached = snapshot;
      const key = String(session);
      const stored = () => {
        if (this._stopped || !native.stored(this._sys, session)) return;
        const d = this._store.delete(key); /* it woke from the addon's copy meanwhile */
        if (d && typeof d.then === 'function') d.catch(() => {});
      };
      const w = this._store.set(key, blob);
      if (w && typeof w.then === 'function') w.then(stored, (e) => platform.log('hibernation store:', e));
      else stored();
      return true;
    }

    /* TA_EFFECT_WAKE: load the blob (the addon asks once; one it still held it woke itself) */
    _wake(session) {
      const key = String(session);
      const wake = (blob) => {
        if (this._stopped) return;
        if (!blob) return platform.log(`tinyactors: cannot wake actor ${key} (no blob in the store)`);
        try {
          native.wake(this._sys, session, blob);
        } catch (e) {
          return platform.log(`tinyactors: ${e.message}`);
        }
        const d = this._store.delete(key);
        if (d && typeof d.then === 'function') d.catch(() => {});
      };
      const blob = this._store.get(key);
      if (blob && typeof blob.then === 'function') blob.then(wake, (e) => platform.log('hibernation store:', e));
      else wake(blob);
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
      if (target instanceof Actor) session = target._session;
      else if (target instanceof Chart) name = this._charts.get(target) || target.name || '';
      else if (target !== undefined && target !== null) name = String(target);
      const done = native
        .migrate(this._sys, name, session, !!wake, timeoutMs)
        .then((r) => ({ ...r, pending: r.pending.map((s) => this._actors.get(s) || this.ref(formatAddress(this.id, s))) }));
      native.pump(this._sys);
      return { done };
    }

    /**
     * The whole system as bytes (ta_host_snapshot_pack: the host, the datamodel's
     * state and the images): restore with createSystem({ restore, charts }). Needs a quiet
     * moment (no migration); hibernated actors stay in the store.
     */
    snapshot() {
      return native.snapshot(this._sys);
    }

    /** The host's counters and each chart's (ta_host_stats_get, ta_host_chart_stats). */
    stats() {
      return native.stats(this._sys);
    }

    /* ---- other systems: routing is the native router's and the link
     * protocol core's (src/link); transports only move text */

    /* what the links (src/link) ask of the transports */
    _linkEffect(e) {
      const t = this._transports.get(e.connection);
      if (e.op === 'send') {
        if (t) t.send(e.text);
      } else if (e.op === 'close') {
        this._transports.delete(e.connection);
        if (t) t.close();
      } else if (e.op === 'open') {
        const open = this._linkOpeners.get(e.connection);
        if (!open) return;
        let transport;
        try {
          transport = open();
        } catch (err) {
          const why = String(err && err.message ? err.message : err);
          platform.defer(() => !this._stopped && native.linkDropped(this._sys, e.connection, why));
          return;
        }
        this._linkWire(e.connection, transport);
        transport.onopen = () => this._transports.get(e.connection) === transport && !this._stopped && native.linkOpened(this._sys, e.connection);
      } else if (e.op === 'hello') {
        const trust = this._linkJudges.get(this._linkServers.get(e.connection));
        let verdict = 0;
        try {
          verdict = trust && trust(JSON.parse(e.text)) ? 2 : 1;
        } catch (err) {
          platform.log(err);
        }
        native.linkAdmit(this._sys, e.connection, verdict);
      }
    }

    /* a transport carries `connection`'s messages */
    _linkWire(connection, t) {
      this._transports.set(connection, t);
      t.onmessage = (ev) => {
        if (this._transports.get(connection) !== t || this._stopped) return;
        if (!native.linkReceived(this._sys, connection, String(ev.data))) return;
        const opened = this._linkOpened.get(connection);
        if (opened) {
          this._linkOpened.delete(connection);
          opened();
        }
      };
      t.onclose = () => {
        if (this._transports.get(connection) !== t) return;
        this._transports.delete(connection);
        this._linkServers.delete(connection);
        if (!this._stopped) native.linkDropped(this._sys, connection, 'connection closed');
      };
      t.onerror = () => {}; /* followed by onclose */
    }

    /**
     * Stop the whole system (ta_host_free): actors stop, links close, timers
     * end, invocations are aborted. The system cannot be used afterwards.
     */
    stop() {
      if (this._stopped) return;
      for (const c of [...this._links]) c.disconnect();
      for (const t of this._transports.values()) t.close(); /* the links go with the system */
      this._transports.clear();
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
    watchTraces({ actors = [], sessions = [], names = [], charts = [], events = [], ...options } = {}) {
      if (this._stopped) throw new Error('the system was stopped');
      const id = native.watch(this._sys, {
        ...options,
        sessions: [...actors.map((a) => (a instanceof Actor ? a._session : a)), ...sessions],
        names,
        charts: charts.map((c) => (c instanceof Chart ? c.name : c)),
        events,
      });
      return { stop: () => !this._stopped && native.unwatch(this._sys, id) };
    }

    /** Resolves once every trace export this system started has finished. */
    flushTraces() {
      return this._exporter ? this._exporter.settled() : Promise.resolve();
    }

    _traceApply() {
      const t = this._traceOptions;
      const exporter = this._exporter;
      /* with nowhere for records to go core writes none; contexts still flow (traceparents) */
      native.traceConfigure(this._sys, {
        ...t,
        enabled: !!(this._traceSink || (exporter && (exporter.urls.traces || exporter.urls.logs))),
        sample: t.sample !== undefined ? t.sample : this._telemetry.sample,
        rules: (t.rules || []).map((r) => (r && r.chart instanceof Chart ? { ...r, chart: r.chart.name } : r)),
        spanMetrics: !!this._telemetry.spanMetrics,
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
      return !!this._session && !this.system._stopped && native.hibernated(this.system._sys, this._session);
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
      if (this._cached) {
        if (this.hibernated) return this._cached;
        this._cached = null; /* awake again */
      }
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
   * transport (docs/protocol.md, "Link protocol"). The protocol is core's
   * (src/link: hellos, reach, pings, reconnects, resumption, exports); this
   * code opens transports and moves their text ("link" effects one way,
   * native.link* calls the other). */

  /** Link `system` to another system (a browser to its server): { url } (a
   * WebSocket) or { transport: () => a WebSocket-like object or a byte
   * stream }, `name` (our name for it), trusted, maxQueue, queueTTLMs,
   * announce, pingMs, credential, maxFrame. Returns { name, opened, get,
   * status, close }. */
  function createLink(system, options = {}) {
    if (!(system instanceof System)) throw new TypeError('createLink(system, options): system expected');
    if (!options.url && !options.transport) throw new TypeError('createLink: { url } or { transport } needed');
    const name = options.name || 'remote';
    if (!native.systemIDValid(name)) throw new TypeError(`createLink: name "${name}" is not a system id`);
    const maxFrame = options.maxFrame || K.FRAME_MAX;
    const link = native.linkConnect(system._sys, {
      name,
      trusted: options.trusted !== false,
      maxQueue: options.maxQueue === undefined ? 1000 : options.maxQueue,
      queueTTLMs: options.queueTTLMs || 0,
      announce: options.announce,
      pingMs: options.pingMs === undefined ? 5000 : options.pingMs,
      credential: options.credential === undefined ? undefined : JSON.stringify(options.credential),
      maxFrame,
    });
    const opened = new Promise((resolve) => system._linkOpened.set(link, resolve));
    system._linkOpeners.set(link, () => (options.transport ? asTransport(options.transport(), maxFrame) : new WebSocket(options.url)));
    native.pump(system._sys); /* asks for its transport */
    return {
      name,
      opened,
      /** A reference to `systemID` (an actor name) on the other side. */
      get: (systemID) => system.ref(formatAddress(name, systemID)),
      /** The router's view of the peer (ta_host_peer_status), and the last error. */
      status: () => (system._stopped ? null : native.linkStatus(system._sys, link)),
      close: () => {
        system._linkOpeners.delete(link);
        if (!system._stopped) native.linkClose(system._sys, link);
      },
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
    const maxFrame = options.maxFrame || K.FRAME_MAX;
    const server = native.linkServe(system._sys, {
      trusted: !!options.trusted,
      judge: typeof options.trust === 'function',
      announce: options.announce,
      exports: exportsOf(options.exports),
      resumeMs: options.resumeMs === undefined ? 30000 : options.resumeMs,
      maxQueue: options.maxQueue || 1000,
      maxFrame,
    });
    if (typeof options.trust === 'function') system._linkJudges.set(server, options.trust);
    let closed = false;
    const stop = options.accept((transport) => {
      const t = asTransport(transport, maxFrame);
      if (closed || system._stopped) return t.close();
      const connection = native.linkAccept(system._sys, server);
      if (!connection) return t.close();
      system._linkServers.set(connection, server);
      system._linkWire(connection, t);
    });
    const statuses = () => (system._stopped ? [] : native.linkConnections(system._sys, server).map((c) => native.linkStatus(system._sys, c)).filter(Boolean));
    return {
      connections: () => statuses().filter((s) => s.open).map((s) => s.remote),
      status: (id) => statuses().find((s) => s.remote === id) || null,
      close: async () => {
        closed = true;
        system._linkJudges.delete(server);
        if (!system._stopped) native.linkClose(system._sys, server);
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
   * `listen(channel, onPayload) -> unlisten`. It is a link (src/link) whose
   * transport is SQL: frames out become tinyactors.hello, tinyactors.deliver
   * (in order, retried until the database takes them: at-least-once, the
   * database drops repeats by msgid) and tinyactors.deliver_error; envelopes
   * the database has for us are claimed (claim_for) when it NOTIFYs or every
   * pollMs, come in as frames and are acked. The database's hello is its
   * system id. */
  function storeTransport(system, options, id) {
    const query = options.query;
    const batch = options.batch || 100;
    const worker = options.worker || `${system.id}/${randomID()}`;
    const t = { onopen: null, onmessage: null, onclose: null, onerror: null, open: false, error: null, out: [], idle: [] };
    let pumping = false, draining = false, again = false;
    const message = (m) => t.open && t.onmessage && t.onmessage({ data: JSON.stringify(m) });
    const lost = () => queueMicrotask(() => t.onclose && t.onclose({}));
    const down = (e) => {
      t.error = String(e && e.message ? e.message : e);
      if (!t.open) return;
      t.open = false; /* the link reconnects; a frame not taken is sent again first */
      lost();
    };
    t.settled = () => {
      if (!pumping && !draining && !t.out.length) for (const ok of t.idle.splice(0)) ok();
    };
    /* the link asks for a transport: the same one, again */
    t.connect = () => {
      t.open = true;
      queueMicrotask(() => {
        if (!t.open) return;
        if (t.onopen) t.onopen({});
        message({ t: 'hello', v: 1, id, reach: [], caps: { max_frame: 0x7fffffff } });
        t.error = null;
        t.pump();
        t.drain();
      });
      return t;
    };
    t.send = (text) => {
      t.out.push(JSON.parse(text));
      t.pump();
    };
    t.close = () => {
      if (!t.open) return;
      t.open = false;
      lost();
    };
    const sql = async (msg) => {
      if (msg.t === 'hello') {
        /* what we reach is claimed by us */
        const r = await query('SELECT tinyactors.hello($1, $2::text[]) AS id', [system.id, `{${msg.reach.join(',')}}`]);
        return r[0] && r[0].id;
      }
      if (msg.t === 'ev') {
        /* as this system: the database checks the origin against what we announced */
        const { t: _, ...envelope } = msg;
        const r = await query('SELECT tinyactors.deliver($1::jsonb, $2) AS r', [JSON.stringify(envelope), system.id]);
        return r[0] && r[0].r;
      }
      if (msg.t === 'err') await query('SELECT tinyactors.deliver_error($1::jsonb)', [JSON.stringify({ origin: msg.origin, msgid: msg.msgid, sendid: msg.sendid, reason: msg.reason })]);
      return null;
    };
    t.pump = async () => {
      if (pumping) return;
      pumping = true;
      try {
        while (t.out.length && t.open) {
          const msg = t.out[0];
          let r;
          try {
            r = await sql(msg);
          } catch (e) {
            down(e); /* kept: sent again when the link is back */
            break;
          }
          t.out.shift();
          if (msg.t === 'ev' && r !== 'ok' && r !== 'duplicate')
            message({ t: 'err', origin: msg.origin, msgid: msg.msgid, sendid: msg.sendid, reason: REASONS.includes(r) ? r : 'unreachable' });
        }
      } finally {
        pumping = false;
        t.settled();
      }
    };
    /* claim what the database has for us (and the systems we announced) */
    t.drain = async () => {
      if (!t.open) return;
      if (draining) {
        again = true;
        return;
      }
      draining = true;
      try {
        do {
          again = false;
          const rows = await query('SELECT id, envelope FROM tinyactors.claim_for($1, $2, $3)', [system.id, batch, worker]);
          for (const row of rows) message({ ...(typeof row.envelope === 'string' ? JSON.parse(row.envelope) : row.envelope), t: 'ev' });
          if (rows.length) await query('SELECT tinyactors.ack(id) FROM unnest($1::bigint[]) AS id', [`{${rows.map((r) => r.id).join(',')}}`]);
          if (rows.length === batch) again = true;
        } while (again && t.open);
      } catch (e) {
        down(e);
      } finally {
        draining = false;
        t.settled();
      }
    };
    return t;
  }

  /** Connect `system` to a database of actors (PostgreSQL + the tinyactors extension) through
   * your own client: `query(sql, params)` resolving to rows, optionally `listen(channel, onPayload)`. */
  function createStoreLink(system, options = {}) {
    if (!(system instanceof System)) throw new TypeError('createStoreLink(system, options): system expected');
    if (typeof options.query !== 'function') throw new TypeError('createStoreLink: { query(sql, params) } is needed');
    let id = options.id || null;
    let link = null, transport = null, poller = null, unlisten = null, closed = false;
    const close = async () => {
      if (closed) return;
      closed = true;
      system._links.delete(store);
      if (poller !== null) platform.clearRepeat(poller);
      if (link) link.close();
      if (unlisten) await Promise.resolve(unlisten()).catch(() => {});
    };
    const store = { disconnect: close };
    const ready = (async () => {
      if (!id) id = (await options.query('SELECT tinyactors.system_id() AS id', []))[0].id;
      if (!native.systemIDValid(id)) throw new TypeError(`createStoreLink: "${id}" is not a system id`);
      transport = storeTransport(system, options, id);
      link = createLink(system, {
        name: id,
        transport: () => transport.connect(),
        trusted: options.trusted !== false,
        maxQueue: options.maxQueue || 10000,
        queueTTLMs: options.queueTTLMs || 0,
        announce: options.announce !== false,
        pingMs: 0,
        maxFrame: 0x7fffffff,
      });
      system._links.add(store);
      if (options.listen) unlisten = await options.listen('tinyactors_outbox', (payload) => payload === system.id && transport.drain());
      const every = options.pollMs === undefined ? 1000 : options.pollMs;
      if (every > 0) poller = platform.repeat(() => transport.drain(), every);
      await link.opened;
      await transport.drain();
    })();
    ready.catch(() => {});
    return {
      get id() {
        return id;
      },
      ready,
      /** Claim and deliver what the database has for us now. */
      drain: () => ready.then(() => transport.drain()),
      /** Resolves once everything handed to the link reached the database and nothing is being claimed. */
      flush: () =>
        ready.then(
          () =>
            new Promise((ok) => {
              transport.idle.push(ok);
              transport.settled();
            }),
        ),
      status: () => {
        const s = link && link.status();
        return s ? { ...s, error: transport.error, pending: transport.out.length } : null;
      },
      close,
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

'use strict';
/*
 * torture-bench prelude: the scenario engine. Runs in the bench's script
 * context (quickjs, no I/O except __host.readText of scenario files).
 *
 * A scenario directory contains
 *   scenario.json   systems (charts + capacities), initial actors, scripts,
 *                   generators, mutations, schedule, expectations
 *   charts/         SCXML documents (or scripts that generate them)
 *   scripts/        run once at start (setup)
 *   generators/     produce events: schema-based (bench.schema) or
 *                   history-based (bench.history.replay / .markov)
 *   mutations/      change the running system; triggered by the schedule
 *                   or from the dashboard
 *   data/           JSON schemas, JSONL histories
 *
 * Scripts see one global, `bench` (documented in tools/torture/README.md).
 */
(function () {
  const H = __host;
  let scenario = null;
  const systems = new Map(); /* name -> { index, name, charts } */
  const groups = new Map(); /* group -> actor ids */
  const handlers = new Map(); /* event from charts (type="bench") -> fns */
  const ioprocs = new Map(); /* scenario-defined host I/O processors -> handler */
  const histograms = new Map();
  const counters = new Map();
  const mutations = new Map();
  const checks = [];
  const finishers = [];
  const results = { mutations: [] };
  const pending = new Set();
  let ending = false;
  function track(value) {
    if (!value || typeof value.then !== 'function') return;
    const p = Promise.resolve(value).catch((e) => H.log('error', String(e.stack || e)))
      .finally(() => pending.delete(p));
    pending.add(p);
    return p;
  }

  /* ---- deterministic randomness (mulberry32) */
  function rng(seed) {
    let a = seed >>> 0 || 0x9e3779b9;
    const next = () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    next.int = (lo, hi) => lo + Math.floor(next() * (hi - lo + 1));
    next.pick = (arr, weights) => {
      if (!weights) return arr[Math.floor(next() * arr.length)];
      const total = weights.reduce((s, w) => s + w, 0);
      let r = next() * total;
      for (let i = 0; i < arr.length; i++) if ((r -= weights[i]) < 0) return arr[i];
      return arr[arr.length - 1];
    };
    next.normal = (mean = 0, sd = 1) => {
      const u = 1 - next(), v = next();
      return mean + sd * Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
    };
    next.exp = (mean) => -Math.log(1 - next()) * mean;
    return next;
  }
  const random = rng(H.seed);

  /* ---- schema-based generation (JSON Schema subset) ----------------------------- */
  const WORDS = 'alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu'.split(' ');
  function genString(s, r) {
    if (s.format === 'uuid') {
      const h = () => Math.floor(r() * 16).toString(16);
      return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, h);
    }
    if (s.format === 'date-time') return new Date(1.7e12 + Math.floor(r() * 3e10)).toISOString();
    if (s.format === 'email') return `${r.pick(WORDS)}.${r.pick(WORDS)}@example.com`;
    const min = s.minLength ?? 3, max = s.maxLength ?? Math.max(min, 12);
    const len = r.int(min, max);
    let out = '';
    while (out.length < len) out += (out ? ' ' : '') + r.pick(WORDS);
    return out.slice(0, len);
  }
  function generate(schema, r = random, defs = schema.$defs || schema.definitions || {}) {
    let s = schema;
    if (s.$ref) s = defs[s.$ref.replace(/^#\/(\$defs|definitions)\//, '')] || {};
    if (s.const !== undefined) return s.const;
    if (s.enum) return r.pick(s.enum, s['x-weights']);
    if (s.oneOf || s.anyOf) return generate(r.pick(s.oneOf || s.anyOf), r, defs);
    const type = Array.isArray(s.type) ? r.pick(s.type) : s.type || (s.properties ? 'object' : 'string');
    switch (type) {
      case 'object': {
        const o = {};
        const req = new Set(s.required || Object.keys(s.properties || {}));
        for (const [k, sub] of Object.entries(s.properties || {}))
          if (req.has(k) || r() < (s['x-optional'] ?? 0.7)) o[k] = generate(sub, r, defs);
        return o;
      }
      case 'array': {
        const n = r.int(s.minItems ?? 0, s.maxItems ?? Math.max(s.minItems ?? 0, 5));
        return Array.from({ length: n }, () => generate(s.items || {}, r, defs));
      }
      case 'integer':
        return r.int(Math.ceil(s.minimum ?? 0), Math.floor(s.maximum ?? 1000));
      case 'number': {
        const lo = s.minimum ?? 0, hi = s.maximum ?? 1000;
        return Math.round((lo + r() * (hi - lo)) * 100) / 100;
      }
      case 'boolean':
        return r() < 0.5;
      case 'null':
        return null;
      default:
        return genString(s, r);
    }
  }

  /* ---- history-based generation ------------------------------------------------- */
  function loadHistory(path) {
    return H.readText(path)
      .split('\n')
      .filter((l) => l.trim())
      .map((l) => JSON.parse(l))
      .sort((a, b) => a.t - b.t);
  }
  function perturb(v, r, amount) {
    if (typeof v === 'number') return Number.isInteger(v) ? Math.round(v * (1 + (r() * 2 - 1) * amount)) : v * (1 + (r() * 2 - 1) * amount);
    if (Array.isArray(v)) return v.map((x) => perturb(x, r, amount));
    if (v && typeof v === 'object') {
      const o = {};
      for (const [k, x] of Object.entries(v)) o[k] = perturb(x, r, amount);
      return o;
    }
    return v;
  }
  /** Learn an order-k Markov chain of event names (per `key`), inter-arrival
   * times and payload samples per event name from a recorded history. */
  function markov(history, { order = 1, key = 'key', r = random, jitter = 0.1 } = {}) {
    const trans = new Map(), gaps = [], payloads = new Map(), last = new Map();
    const starts = [];
    let prevT = null;
    for (const e of history) {
      if (prevT !== null) gaps.push(e.t - prevT);
      prevT = e.t;
      const k = e[key] ?? '_';
      const ctx = last.get(k) || [];
      const state = ctx.join('|');
      if (!ctx.length) starts.push(e.event);
      else {
        if (!trans.has(state)) trans.set(state, new Map());
        const m = trans.get(state);
        m.set(e.event, (m.get(e.event) || 0) + 1);
      }
      if (!payloads.has(e.event)) payloads.set(e.event, []);
      if (e.data !== undefined) payloads.get(e.event).push(e.data);
      last.set(k, [...ctx, e.event].slice(-order));
    }
    const streams = new Map();
    return {
      gaps,
      meanGap: gaps.length ? gaps.reduce((a, b) => a + b, 0) / gaps.length : 1,
      /** next event for stream `id`: { event, data, dt } */
      next(id = '_') {
        const ctx = streams.get(id) || [];
        const m = ctx.length ? trans.get(ctx.join('|')) : null;
        let ev;
        if (m && m.size) ev = r.pick([...m.keys()], [...m.values()]);
        else ev = r.pick(starts.length ? starts : history.map((h) => h.event));
        streams.set(id, [...ctx, ev].slice(-order));
        if (!m || !m.size) streams.set(id, [ev]);
        const samples = payloads.get(ev) || [];
        const data = samples.length ? perturb(r.pick(samples), r, jitter) : undefined;
        return { event: ev, data, dt: gaps.length ? r.pick(gaps) : 1 };
      },
    };
  }
  /** Replay a recorded history: fn(entry) at the recorded times (scaled). */
  function replay(history, fn, { speed = 1, loop = true } = {}) {
    if (!history.length) return null;
    const t0 = history[0].t, span = history[history.length - 1].t - t0 + 1e-3;
    let i = 0, base = H.now();
    const timer = bench.every(5, () => {
      const now = (H.now() - base) * speed / 1000;
      while (i < history.length && history[i].t - t0 <= now) fn(history[i++]);
      if (i >= history.length && loop) {
        i = 0;
        base += (span * 1000) / speed;
      }
    });
    return timer;
  }

  /* ---- histograms (exported as p50/p95/p99/max per sample) */
  class Histogram {
    constructor(name) {
      this.name = name;
      this.values = [];
      this.count = 0;
    }
    record(v) {
      this.count++;
      if (this.values.length < 50000) this.values.push(v);
    }
    flush() {
      if (!this.values.length) return;
      const v = this.values.sort((a, b) => a - b);
      const q = (p) => v[Math.min(v.length - 1, Math.floor(p * v.length))];
      H.metric(`${this.name}.p50`, q(0.5));
      H.metric(`${this.name}.p95`, q(0.95));
      H.metric(`${this.name}.p99`, q(0.99));
      H.metric(`${this.name}.max`, v[v.length - 1]);
      this.last = { p50: q(0.5), p99: q(0.99), max: v[v.length - 1] };
      this.values = [];
    }
  }

  /* ---- the bench API ------------------------------------------------------------- */
  const scaled = (n) => Math.max(1, Math.round(n * H.scale));
  const toJSON = (d) => (d === undefined ? undefined : JSON.stringify(d));

  /** --param key=value; numbers are parsed */
  const params = {};
  for (const [k, v] of Object.entries(H.params || {})) params[k] = v !== '' && !isNaN(Number(v)) ? Number(v) : v;
  const reportRows = [];
  const metricInfo = {};

  const bench = {
    params,
    /** This machine: { memTotal, memAvailable, memoryLimit (cgroup, 0 = none), cpus, rss } in bytes, at start; systemMaxActors = the runtime's hard per-system limit. */
    machine: H.machine,
    /** Resident memory of the bench process now (bytes). */
    rss: () => H.rss(),
    /** A row of the run's result table (summary.results, shown on the dashboard). */
    result(row) {
      reportRows.push({ t: H.now() / 1000, ...row });
      H.log('result', Object.entries(row).map(([k, v]) => `${k}=${typeof v === 'number' ? Math.round(v * 100) / 100 : v}`).join(' '));
    },
    /** Title/unit/format ('num' | 'ms' | 'pct' | 'bytes') of the dashboard chart for metrics "<prefix>.*". */
    describeMetric(prefix, info) {
      metricInfo[prefix] = info;
    },
    scale: H.scale,
    seed: H.seed,
    random,
    rng,
    scaled,
    get scenario() {
      return scenario;
    },
    now: () => H.now(),
    log: (...a) => H.log('info', a.map((x) => (typeof x === 'string' ? x : JSON.stringify(x))).join(' ')),
    warn: (...a) => H.log('warn', a.join(' ')),
    readText: (p) => H.readText(p),
    readJSON: (p) => JSON.parse(H.readText(p)),
    metric: (name, v) => H.metric(name, v),
    histogram(name) {
      if (!histograms.has(name)) histograms.set(name, new Histogram(name));
      return histograms.get(name);
    },
    count(name, n = 1) {
      counters.set(name, (counters.get(name) || 0) + n);
      return counters.get(name);
    },
    counter: (name) => counters.get(name) || 0,

    /** Create a system from SCXML charts: paths, or { name, script } that returns SCXML text. */
    addSystem(name, def = {}) {
      const charts = (def.charts || []).map((c) => {
        if (typeof c === 'string') return { name: c.split('/').pop(), text: H.readText(c) };
        if (c.script) {
          const make = new Function('bench', 'params', H.readText(c.script));
          return { name: c.name, text: String(make(bench, c.params || {})) };
        }
        return { name: c.name, text: c.text };
      });
      const config = { ioprocessors: (scenario && scenario.ioprocessors) || [], ...(def.config || {}) };
      /* --param tracing=disabled|records|otlp, traceDetail=summary|decisions|full,
       * traceSample=0..1: every system (comparisons) */
      if (bench.params.tracing) config.tracing = String(bench.params.tracing);
      if (bench.params.traceDetail) config.traceDetail = String(bench.params.traceDetail);
      if (bench.params.traceSample !== undefined) config.traceSample = Number(bench.params.traceSample);
      const index = H.createSystem(name, charts, config);
      systems.set(name, { index, name, charts: charts.map((c) => c.name) });
      return name;
    },
    removeSystem(name) {
      const s = systems.get(name);
      if (!s) return false;
      H.destroySystem(s.index);
      systems.delete(name);
      for (const [g, ids] of groups) groups.set(g, ids.filter((id) => Math.floor(id / 2 ** 48) !== s.index));
      return true;
    },
    systems: () => [...systems.keys()],
    /** Hibernate (unload to the bench's blob file) actors of `system` that did not run since the
     *  previous sweep: a CLOCK sweep over up to `scan` slots, at most `max` actors. Woken
     *  automatically when they get work (TA_EFFECT_WAKE). Returns the number unloaded. */
    hibernate(system, { max = 1e9, scan = 1e6 } = {}) {
      const s = systems.get(system);
      return s ? H.hibernate(s.index, max, scan) : 0;
    },
    stats(name) {
      const s = systems.get(name);
      return s ? H.stats(s.index) : undefined;
    },
    /** Hot reload (CGI style): a new version of `system` with these charts replaced (paths
     *  or { name, text }; the rest as before). New actors start in it; running actors stay on
     *  their version (or move with migrate()); versions without actors are freed. Addresses
     *  keep working across versions. Returns the version number (2, 3, ...). */
    reload(system, charts = []) {
      const s = systems.get(system);
      if (!s) throw new Error(`no system "${system}"`);
      const list = charts.map((c) => (typeof c === 'string' ? { name: c.split('/').pop(), text: H.readText(c) } : c));
      const v = H.reload(s.index, list);
      H.log('info', `${system}: version ${v} loaded`);
      return v;
    },
    /** Live migration: move running actors of older versions of `system` to the newest one
     *  (states by id, data kept, error.platform.upgrade with the previous bindings, same
     *  address). A job the bench loop advances in ~2 ms steps (actors walked in chunks):
     *  busy actors are retried at later steps, hibernated ones too unless `wake` loads them
     *  first; `timeoutMs` ends the job with the stragglers left where they are.
     *  onDone({ migrated, failed, pending }) runs when it ends. Returns the job's status. */
    migrate(system, { wake = false, timeoutMs, onDone } = {}) {
      const s = systems.get(system);
      if (!s) throw new Error(`no system "${system}"`);
      return H.migrate(s.index, { wake, timeoutMs, onDone });
    },
    /** Status of the migration job of `system`: { active, migrated, failed, pending, walking }. */
    migration(system) {
      const s = systems.get(system);
      return s ? H.migration(s.index) : undefined;
    },
    /** The versions of `system` alive now: [{ name, version, actors, current }]. */
    versions(system) {
      const s = systems.get(system);
      return s ? H.versions(s.index) : [];
    },
    /** Modification time (ms) of a scenario file, 0 if missing. */
    mtime: (path) => H.mtime(path),
    /** CGI style: reload `system` whenever one of `paths` changes (polled every `ms`).
     *  `load()` returns the charts to reload (default: the changed paths); `migrate` moves
     *  running actors right away; `then(version)` runs after a reload. Returns the timer. */
    watch(system, paths, { ms = 1000, load, migrate = false, then } = {}) {
      const seen = new Map(paths.map((p) => [p, H.mtime(p)]));
      return H.setTimer(ms, ms, () => {
        const changed = paths.filter((p) => H.mtime(p) !== seen.get(p));
        if (!changed.length) return;
        for (const p of changed) seen.set(p, H.mtime(p));
        try {
          const v = bench.reload(system, load ? load(changed) : changed);
          if (migrate) bench.migrate(system);
          if (then) then(v);
        } catch (e) {
          H.log('error', `${system}: reload failed: ${e.message || e}`);
        }
      });
    },
    /** The run's planned duration in seconds. */
    duration: () => (H.duration > 0 ? H.duration : (scenario && scenario.duration) || 30),

    /** Spawn actors; returns their ids. opts: { count, group, data, name } — data may be a function (i) => data. */
    spawn(system, chart, opts = {}) {
      const s = systems.get(system);
      if (!s) throw new Error(`no system "${system}"`);
      const count = opts.count ?? 1;
      const ids = [];
      for (let i = 0; i < count; i++) {
        const data = typeof opts.data === 'function' ? opts.data(i) : opts.data;
        const name = opts.name ? String(opts.name).replace('{i}', i) : undefined;
        ids.push(H.spawn(s.index, chart, name, toJSON(data)));
      }
      if (opts.group) {
        const g = groups.get(opts.group) || [];
        for (const id of ids) g.push(id);
        groups.set(opts.group, g);
      }
      return ids;
    },
    group(name) {
      return groups.get(name) || [];
    },
    /** Drop actors that are gone from a group (after terminate/finish). */
    prune(name) {
      const g = (groups.get(name) || []).filter((id) => H.alive(id));
      groups.set(name, g);
      return g;
    },
    send: (id, event, data) => H.send(id, event, toJSON(data)),
    broadcast: (ids, event, data) => H.sendMany(ids, event, toJSON(data)),
    terminate: (id) => H.terminate(id),
    alive: (id) => H.alive(id),
    /** SCXML address of an actor, usable as <send target> from any system. */
    address: (id) => H.address(id),

    every(ms, fn) {
      let busy = false;
      return H.setTimer(ms, ms, () => {
        if (ending || busy) return;
        const p = track(fn());
        if (p) { busy = true; p.finally(() => { busy = false; }); }
      });
    },
    after(ms, fn) {
      return H.setTimer(ms, 0, () => track(fn()));
    },
    cancel: (t) => H.clearTimer(t),
    /** Call fn(n) every `tick` ms with the number of events due at `perSecond` (may change via the returned handle). */
    rate(perSecond, fn, tick = 10) {
      const h = { perSecond, acc: 0 };
      h.timer = H.setTimer(tick, tick, () => {
        h.acc += (h.perSecond * tick) / 1000;
        const n = Math.floor(h.acc);
        if (n > 0) {
          h.acc -= n;
          fn(n);
        }
      });
      return h;
    },
    /** A host I/O processor named in scenario.json "ioprocessors": charts use
     * <send type="<type>" ...>; fn({ event, target, data, sendid, from }) runs for
     * each send. Reply with bench.send(msg.from, ...), or bench.failSend(msg) for
     * error.communication. */
    ioprocessor(type, fn) {
      ioprocs.set(type, fn);
    },
    failSend: (msg) => H.failSend(msg.from, msg.sendid),
    /** Events sent by charts with <send type="bench" event="...">: fn(data, fromActorID). */
    on(event, fn) {
      if (!handlers.has(event)) handlers.set(event, []);
      handlers.get(event).push(fn);
    },

    schema: { generate, load: (p) => JSON.parse(H.readText(p)) },
    history: { load: loadHistory, markov, replay },

    run(path) {
      return H.evalFile(path);
    },
    mutation(id, def) {
      mutations.set(id, typeof def === 'function' ? { id, title: id, fn: def } : { id, ...def });
    },
    mutate(id) {
      const m = mutations.get(id);
      if (!m) throw new Error(`no mutation "${id}"`);
      H.log('mutation', m.title || id);
      results.mutations.push({ id, t: H.now() / 1000 });
      if (m.fn) m.fn(bench);
      else H.evalFile(m.script);
    },
    stop: (msg) => H.stop(msg || 'stopped'),
    /** Called before the summary is evaluated (e.g. to record unfinished measurements). */
    onFinish(fn) {
      finishers.push(fn);
    },
    /** A pass/fail check evaluated on the final summary: fn(summary) -> boolean. */
    check(description, fn) {
      checks.push({ description, fn });
    },
  };
  globalThis.bench = bench;

  /* ---- host hooks ----------------------------------------------------------------- */
  globalThis.__start = function () {
    scenario = JSON.parse(H.readText('scenario.json'));
    for (const [name, def] of Object.entries(scenario.systems || {})) bench.addSystem(name, def);
    for (const s of scenario.spawn || [])
      bench.spawn(s.system, s.chart, { count: s.fixed ? s.count : scaled(s.count ?? 1), group: s.group, data: s.data, name: s.name });
    for (const [id, m] of Object.entries(scenario.mutations || {})) bench.mutation(id, m);
    for (const f of scenario.scripts || []) bench.run(f);
    for (const f of scenario.generators || []) bench.run(f);
    for (const e of scenario.schedule || []) bench.after(e.at * 1000, () => bench.mutate(e.mutation));
    H.log('info', `scenario "${scenario.title || scenario.name || 'untitled'}" started (scale ${H.scale})`);
  };
  globalThis.__scenarioDuration = () => (scenario && scenario.duration) || 30;
  globalThis.__scenarioInfo = () =>
    JSON.stringify({
      name: scenario && scenario.name,
      title: scenario && (scenario.title || scenario.name),
      description: scenario && scenario.description,
      scale: H.scale,
      params,
      metrics: metricInfo,
      systems: [...systems.keys()],
      mutations: [...mutations.values()].map((m) => ({ id: m.id, title: m.title || m.id, description: m.description || '' })),
      charts: scenario && scenario.charts,
    });
  globalThis.__hostSend = (type, msg) => {
    const fn = ioprocs.get(type);
    if (fn) fn(msg);
    else H.failSend(msg.from, msg.sendid); /* nobody carries this type */
  };
  globalThis.__fromChart = (event, data, from) => {
    for (const fn of handlers.get(event) || []) fn(data, from);
  };
  globalThis.__beforeSample = () => {
    for (const h of histograms.values()) h.flush();
    for (const [k, v] of counters) H.metric(k, v);
  };
  globalThis.__command = (text) => {
    let cmd;
    try {
      cmd = JSON.parse(text);
    } catch (e) {
      return undefined;
    }
    if (cmd.cmd === 'mutate') {
      bench.mutate(cmd.id);
      return 'ok';
    }
    if (cmd.cmd === 'pause' || cmd.cmd === 'resume' || cmd.cmd === 'stop') return cmd.cmd;
    return undefined;
  };

  /* Expectations: "path op value", path into the summary (e.g. "systems.ping.deliveredTotal > 1000"). */
  function lookup(obj, path) {
    return path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);
  }
  globalThis.__finish = async () => {
    ending = true;
    await Promise.all([...pending]);
    const summary = { type: 'summary', scenario: scenario && scenario.name, duration: H.now() / 1000, scale: H.scale, systems: {}, totals: { deliveredTotal: 0, actors: 0, remoteOut: 0, remoteFailed: 0, faults: 0 }, metrics: {}, counters: {}, mutations: results.mutations };
    for (const [name, s] of systems) {
      const st = H.stats(s.index);
      if (!st) continue;
      st.deliveredTotal = st.delivered;
      st.deliveredPerSecond = st.delivered / summary.duration;
      st.bytesPerActor = st.actors ? st.regionBytes / st.actors : 0;
      st.versions = H.versions(s.index);
      summary.systems[name] = st;
      summary.totals.deliveredTotal += st.delivered;
      summary.totals.actors += st.actors;
      summary.totals.remoteOut += st.remoteOut;
      summary.totals.remoteFailed += st.remoteFailed;
      summary.totals.faults += st.faults;
    }
    summary.totals.deliveredPerSecond = summary.totals.deliveredTotal / summary.duration;
    for (const h of histograms.values()) {
      h.flush();
      if (h.last) summary.metrics[h.name] = { ...h.last, count: h.count };
    }
    for (const [k, v] of counters) summary.counters[k] = v;
    for (const fn of finishers) await fn(summary);
    summary.results = reportRows;
    const failures = [];
    for (const e of (scenario && scenario.expect) || []) {
      const m = /^\s*([\w.$-]+)\s*(>=|<=|==|!=|>|<)\s*(-?[\d.e+]+)\s*$/.exec(e);
      if (!m) {
        failures.push(`bad expectation: ${e}`);
        continue;
      }
      const v = lookup(summary, m[1]);
      const want = Number(m[3]);
      const ok = typeof v === 'number' && { '>': v > want, '<': v < want, '>=': v >= want, '<=': v <= want, '==': v === want, '!=': v !== want }[m[2]];
      if (!ok) failures.push(`${e} (got ${v})`);
    }
    for (const c of checks) {
      let ok = false;
      try {
        ok = !!(await c.fn(summary));
      } catch (e) {
        failures.push(`${c.description} (threw ${e})`);
        continue;
      }
      if (!ok) failures.push(c.description);
    }
    summary.expect = [...((scenario && scenario.expect) || []), ...checks.map((c) => c.description)];
    summary.failures = failures;
    summary.ok = failures.length === 0;
    return JSON.stringify(summary);
  };
})();

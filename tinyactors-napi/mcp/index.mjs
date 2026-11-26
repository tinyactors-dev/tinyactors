// @tinyactors/mcp: a Model Context Protocol server over running tinyactors
// hosts, for an agent that inspects a system, sends to it, reads its
// traces and rehearses and deploys new versions of its charts.
//
// An opt-in package beside @tinyactors/node: JavaScript over that
// package's public API and the MCP SDK (@modelcontextprotocol/sdk),
// nothing else. Two halves:
//
// - tap(): wraps a host's options when the host is made (a handler is fixed
//   then), keeping a copy of what the agent reads later: the trace spans,
//   the <log> lines, the migrations' results, the running services. Every
//   handler the application gave goes on being called.
// - createMCP(systems): the tools, over each host and its tap. Live tools
//   act on the host now; snapshot tools take a snapshot once and answer
//   from its bytes (every actor at one moment, the trace history the
//   snapshot keeps), and from a copy restored from it for variables and
//   rehearsals. A copy never touches the live host or the outside world:
//   its I/O processors, services and transport are stand-ins.
import { createServer as createHTTPServer } from 'node:http';
import { randomUUID } from 'node:crypto';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/streamableHttp.js';
import { z } from 'zod';
import { compileCharts, decodeTraces, Host, SimulatedClock } from '@tinyactors/node';
import { decodePack, stateName } from './lib/snapshot.mjs';

// ---- tap: what the host's handlers see, kept for the agent -----------------------------------

/**
 * Wraps a host's options so the MCP server can read what its handlers see.
 * Call `options(given)` with the options for `new Host(charts, options)`.
 */
export function tap({ spans = 5000, logs = 1000, migrations = 50 } = {}) {
  const state = {
    given: undefined,
    spans: [],
    logs: [],
    migrations: [],
    migrationWaiters: [],
    services: new Map(), // session (bigint) -> [Invocation]
    limits: { spans, logs, migrations },
  };
  const keep = (list, item, limit) => {
    list.push(item);
    if (list.length > limit) list.splice(0, list.length - limit);
  };
  return {
    state,
    /** The options for `new Host` (or `Host.restore`), with each handler wrapped. */
    options(given = {}) {
      state.given = given;
      const options = { ...given };
      options.telemetry = (signal, data) => {
        if (signal === 'traces') {
          try {
            for (const span of decodeTraces(data)) keep(state.spans, span, state.limits.spans);
          } catch {
            /* not ours to fail the exporter */
          }
        }
        return given.telemetry?.(signal, data);
      };
      options.log = (entry) => {
        keep(state.logs, { session: entry.session, label: entry.label, text: entry.text }, state.limits.logs);
        return given.log?.(entry);
      };
      options.migrationDone = (result) => {
        const copy = { migration: result.migration, migrated: result.migrated, failed: [...result.failed], pending: [...result.pending] };
        keep(state.migrations, copy, state.limits.migrations);
        for (const waiter of state.migrationWaiters.splice(0)) waiter(copy);
        return given.migrationDone?.(result);
      };
      if (given.services) {
        options.services = Object.fromEntries(Object.entries(given.services).map(([type, start]) => [type, (invocation) => {
          const list = state.services.get(invocation.session) ?? [];
          list.push(invocation);
          state.services.set(invocation.session, list.filter((i) => i.running));
          return start(invocation);
        }]));
      }
      return options;
    },
  };
}

// ---- values for a model ------------------------------------------------------------------------

const plain = (value) => JSON.parse(JSON.stringify(value, (_, v) => {
  if (typeof v === 'bigint') return v <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(v) : v.toString();
  if (v instanceof Uint8Array) return `<${v.length} bytes>`;
  return v;
}));
const ok = (value, note) => {
  const structured = plain(value);
  const text = JSON.stringify(structured, null, 2);
  return {
    content: [{ type: 'text', text: note ? `${note}\n${text}` : text }],
    structuredContent: Array.isArray(structured) ? { items: structured } : structured,
  };
};
const refused = (message) => ({ isError: true, content: [{ type: 'text', text: message }], structuredContent: { error: message } });

/** A migration's result for a model: failures grouped by reason, a few sessions each. */
export function migrationSummary(result) {
  if (!result?.failed) return result;
  const failed = {};
  for (const f of result.failed) {
    const group = (failed[f.reason] ??= { count: 0, sample: [] });
    group.count++;
    if (group.sample.length < 5) group.sample.push(f.session);
  }
  return { migration: result.migration, migrated: result.migrated, failed, pending: result.pending.length };
}

// ---- the server ----------------------------------------------------------------------------------

/**
 * The MCP tools over `systems`: `{ [name]: { host, tap?, sources? } }`, where
 * `tap` is the tap whose options made the host (needed for traces, logs,
 * services, migrations and every copy) and `sources` maps a chart's name to
 * its SCXML (the image holds no source). Options: `allowDeploy` (deploy,
 * migrate, end an invocation), `allowTerminate`, `snapshotKeep` (8).
 * `server()` makes one McpServer (one per connection); `listen()` serves
 * Streamable HTTP.
 */
export function createMCP(systems, options = {}) {
  const shared = { snapshots: new Map(), drafts: new Map(), nextSnapshot: 1, nextDraft: 1 };

  const system = (name) => {
    const s = systems[name];
    if (!s) throw new Error(`no system ${name}; known: ${Object.keys(systems).join(', ')}`);
    return s;
  };
  const tapOf = (s, what) => {
    if (!s.tap) throw new Error(`${what} needs the host to be made with tap().options(...)`);
    return s.tap.state;
  };
  const actorOf = (s, which) => s.host.actor(/^\d+$/.test(String(which)) ? BigInt(which) : String(which));

  // a copy of a snapshot: the same system, config and datamodel, stand-ins for the world
  function copyOf(snap, { record, services = {} } = {}) {
    const s = system(snap.system);
    const given = tapOf(s, 'a copy of a snapshot').given;
    const standIns = {};
    if (given.ioProcessors) {
      standIns.ioProcessors = Object.fromEntries(Object.entries(given.ioProcessors).map(([type, p]) => [type, {
        location: p.location,
        receive(send) { record?.sends.push(`${type} ${send.event}`); },
      }]));
    }
    if (given.services) {
      standIns.services = Object.fromEntries(Object.keys(given.services).map((type) => [type, (invocation) => {
        record?.invocations.push(`${type}${invocation.restored ? ' (restored)' : ''}`);
        const answer = services[type] ?? 'silent';
        if (answer !== 'silent') {
          queueMicrotask(() => {
            try {
              if (answer === 'done') invocation.done();
              else invocation.sendBack(answer === 'error' ? 'error.communication' : answer);
            } catch { /* it ended meanwhile */ }
          });
        }
        return {};
      }]));
    }
    const copyOptions = {
      system: given.system, config: given.config, datamodel: given.datamodel, quickjs: given.quickjs,
      ...standIns,
      transport: (send) => { record?.sends.push(`transport ${send.event}`); },
      clock: new SimulatedClock(),
    };
    return { copy: Host.restore(snap.bytes, [], { ...copyOptions, ...(record?.handlers ?? {}) }), clock: copyOptions.clock };
  }

  const packOf = (id) => {
    const snap = shared.snapshots.get(id);
    if (!snap) throw new Error(`no snapshot ${id}; take one with the snapshot tool`);
    snap.pack ??= decodePack(snap.bytes);
    return snap;
  };
  const live = (pack) => pack.segments.filter((segment) => segment.live);
  const statesOf = (segment, actor) => (actor.configuration ?? []).map((i) => segment.charts[actor.chart]?.states[i]?.id ?? `#${i}`);
  const leavesOf = (segment, actor) => {
    const chart = segment.charts[actor.chart];
    if (!chart || !actor.configuration) return [];
    const active = new Set(actor.configuration);
    return actor.configuration.filter((i) => !(chart.states[i].children ?? []).some((c) => active.has(c))).map((i) => chart.states[i].id);
  };
  const rowOf = (segment, actor) => ({
    session: actor.session, name: actor.name, chart: actor.chartName, segment: segment.index,
    status: stateName(actor.state), states: leavesOf(segment, actor), mailbox: actor.mailboxLength,
    queued: (actor.queued ?? []).map((e) => e?.eventName ?? null),
    delayed: (actor.delayed ?? []).map((t) => ({ event: t.event?.eventName ?? null, sendid: t.event?.strings?.sendid ?? null })),
  });
  const countStates = (pack, chart) => {
    const counts = {};
    for (const segment of live(pack)) {
      for (const actor of segment.actors) {
        if (chart && actor.chartName !== chart) continue;
        const key = stateName(actor.state) === 'hibernated' ? '(hibernated)' : leavesOf(segment, actor).join('+');
        counts[key] = (counts[key] ?? 0) + 1;
      }
    }
    return counts;
  };

  function server() {
    const mcp = new McpServer({ name: 'tinyactors', version: '0.0.0' });
    const tool = (name, description, inputSchema, annotations, run) =>
      mcp.registerTool(name, { description, inputSchema, annotations }, async (args) => {
        try {
          return await run(args);
        } catch (error) {
          return refused(`${error.name}: ${error.message}`);
        }
      });
    const read = { readOnlyHint: true };
    const destructive = { destructiveHint: true };

    // live: the host now
    tool('systems', 'The systems served: each with its datamodel, its charts, and how many actors it holds now.', {}, read, async () => {
      const out = [];
      for (const [name, s] of Object.entries(systems)) {
        const stats = await s.host.hostStats();
        out.push({ system: name, charts: stats.charts.map((c) => c.name), actors: stats.host.actors, hibernated: stats.host.hibernated, segments: stats.host.segments, nowMs: s.host.now() });
      }
      return ok(out);
    });
    tool('host_stats', 'The host\'s counters now: actors, hibernated, pools, and per chart its turns, errors and faults.', { system: z.string() }, read,
      async ({ system: name }) => ok(await system(name).host.hostStats()));
    tool('actor', 'One actor now: its state, mailbox, delayed sends, invocations and variables. A hibernated actor has no state or variables here: use a snapshot.', { system: z.string(), actor: z.string().describe('a session id or the name it was spawned with') }, read, async ({ system: name, actor }) => {
      const a = actorOf(system(name), actor);
      const out = { session: a.id, get: await a.get() };
      for (const [key, call] of [['state', () => a.state()], ['context', () => a.context()]]) {
        try { out[key] = await call(); } catch (error) { out[key] = { refused: error.message }; }
      }
      return ok(out);
    });
    tool('send', 'Send an event to an actor. It returns once the event is in the actor\'s mailbox; read the effect with actor or a snapshot.', { system: z.string(), actor: z.string(), event: z.string(), data: z.any().optional() }, { openWorldHint: true }, async ({ system: name, actor, event, data }) => {
      const a = actorOf(system(name), actor);
      a.send(event, data);
      return ok({ sent: event, to: a.id });
    });
    tool('ask', 'Send an event and wait for the actor\'s reply to _event.origin (a <send targetexpr="_event.origin">).', { system: z.string(), actor: z.string(), event: z.string(), data: z.any().optional(), timeoutMs: z.number().default(2000) }, { openWorldHint: true }, async ({ system: name, actor, event, data, timeoutMs }) => {
      const [reply, replyData] = await actorOf(system(name), actor).ask(event, data, { signal: AbortSignal.timeout(timeoutMs) });
      return ok({ reply, data: replyData });
    });
    tool('trace_watch', 'Trace matching actors at more detail for a while: rules of {chart, actor (a name, * suffix), event, session, detail: summary|decisions|full, values}. Read the spans with traces.', { system: z.string(), rules: z.array(z.record(z.any())), seconds: z.number().default(60) }, {},
      async ({ system: name, rules, seconds }) => ok(await system(name).host.traceWatch({ rules, seconds })));
    tool('trace_unwatch', 'End a watch early.', { system: z.string(), id: z.number() }, {},
      async ({ system: name, id }) => ok(await system(name).host.traceUnwatch({ id })));
    tool('traces', 'The recent spans the host exported (its telemetry handler), newest last: filter by session or a span name prefix. Needs telemetry in the host\'s config.', { system: z.string(), session: z.number().optional(), name: z.string().optional(), limit: z.number().default(50) }, read, async ({ system: name, session, name: prefix, limit }) => {
      let spans = tapOf(system(name), 'traces').spans;
      if (session != null) spans = spans.filter((s) => Number(s.attributes?.['scxml.session_id'] ?? -1) === session);
      if (prefix) spans = spans.filter((s) => s.name.startsWith(prefix));
      return ok({ total: spans.length, spans: spans.slice(-limit) });
    });
    tool('logs', 'The recent <log> lines, newest last, for one session or all.', { system: z.string(), session: z.number().optional(), limit: z.number().default(50) }, read, async ({ system: name, session, limit }) => {
      let lines = tapOf(system(name), 'logs').logs;
      if (session != null) lines = lines.filter((l) => Number(l.session) === session);
      return ok({ total: lines.length, logs: lines.slice(-limit) });
    });
    tool('services', 'The services (external invocations) that run, by invoking session: type, src, invoke id.', { system: z.string(), session: z.number().optional() }, read, async ({ system: name, session }) => {
      const out = [];
      for (const [s, list] of tapOf(system(name), 'services').services) {
        if (session != null && Number(s) !== session) continue;
        for (const i of list) if (i.running) out.push({ session: s, invokeid: i.invokeid, type: i.type, src: i.src });
      }
      return ok(out);
    });
    tool('end_service', 'End a running service as its service would: done (done.invoke for the actor) or error (error.communication). For a service that will never answer. Destructive.', { system: z.string(), session: z.number(), invokeid: z.string(), as: z.enum(['done', 'error']) }, destructive, async ({ system: name, session, invokeid, as }) => {
      if (!options.allowDeploy) return refused('end_service is off for this server (allowDeploy)');
      const list = tapOf(system(name), 'end_service').services.get(BigInt(session)) ?? [];
      const invocation = list.find((i) => i.running && i.invokeid === invokeid);
      if (!invocation) return refused(`no running service ${invokeid} of session ${session}`);
      if (as === 'done') invocation.done();
      else invocation.sendBack('error.communication');
      return ok({ ended: invokeid, session, as });
    });
    tool('chart_source', 'The SCXML of the current version of a chart, as the application gave it (an image holds no source).', { system: z.string(), chart: z.string() }, read, async ({ system: name, chart }) => {
      const text = system(name).sources?.[chart];
      return text ? { content: [{ type: 'text', text }] } : refused(`no source for chart ${chart}`);
    });
    tool('compile', 'Compile a draft chart; nothing runs. Returns its diagnostics and a draft id for rehearse and deploy.', { name: z.string().describe('a file name, such as order.scxml'), text: z.string() }, read, async ({ name, text }) => {
      let compiled;
      try {
        compiled = compileCharts([{ name, text }]);
      } catch (error) {
        return { ...refused(`does not compile: ${error.message}`), structuredContent: { error: error.message, diagnostics: plain(error.diagnostics ?? []) } };
      }
      const id = `d${shared.nextDraft++}`;
      shared.drafts.set(id, { name, text, image: compiled.image, diagnostics: compiled.diagnostics });
      compiled.close();
      return ok({ draft: id, diagnostics: shared.drafts.get(id).diagnostics });
    });
    tool('deploy', 'Add a draft\'s image to the live host: new spawns run it at once. With migrate, move the running actors of the chart too, and wait for the result. Destructive: rehearse first.', { system: z.string(), draft: z.string(), chart: z.string(), migrate: z.boolean().default(false), wake: z.boolean().default(false), timeoutMs: z.number().default(30000) }, destructive, async ({ system: name, draft, chart, migrate, wake, timeoutMs }) => {
      if (!options.allowDeploy) return refused('deploy is off for this server (allowDeploy)');
      const s = system(name);
      const d = shared.drafts.get(draft);
      if (!d) return refused(`no draft ${draft}`);
      const segment = s.host.addImage(d.image);
      if (s.sources) s.sources[chart] = d.text;
      if (!migrate) return ok({ segment });
      const waiters = tapOf(s, 'deploy with migrate').migrationWaiters;
      const result = new Promise((resolve) => waiters.push(resolve));
      const migration = s.host.migrate({ chart, wake });
      const done = await Promise.race([result, new Promise((resolve) => setTimeout(() => resolve(undefined), timeoutMs))]);
      return ok({ segment, migration, result: done ? migrationSummary(done) : 'still running' });
    });
    tool('terminate', 'End an actor. Destructive.', { system: z.string(), actor: z.string() }, destructive, async ({ system: name, actor }) => {
      if (!options.allowTerminate) return refused('terminate is off for this server (allowTerminate)');
      const a = actorOf(system(name), actor);
      a.terminate();
      return ok({ terminated: a.id });
    });

    // snapshots: every actor at one moment, read without the live host
    tool('snapshot', 'Take a snapshot of a system: every actor, its states, mailbox and delayed sends, the variables, and the trace history, at one moment. The snapshot_* tools and rehearse read it without touching the live host.', { system: z.string() }, read, async ({ system: name }) => {
      const s = system(name);
      let bytes;
      for (let attempt = 0; ; attempt++) {
        try {
          bytes = s.host.snapshot();
          break;
        } catch (error) {
          if (attempt >= 40) throw error; // a reply pending or a migration running: after the next turn
          await new Promise((resolve) => setTimeout(resolve, 25));
        }
      }
      const id = `s${shared.nextSnapshot++}`;
      shared.snapshots.set(id, { id, system: name, bytes, hostTimeMs: s.host.now() });
      while (shared.snapshots.size > (options.snapshotKeep ?? 8)) shared.snapshots.delete(shared.snapshots.keys().next().value);
      const { pack } = packOf(id);
      return ok({ snapshot: id, system: name, bytes: bytes.length, actors: live(pack).reduce((n, segment) => n + segment.actors.length, 0), hostTimeMs: shared.snapshots.get(id).hostTimeMs });
    });
    tool('snapshot_summary', 'What a snapshot holds: its versions (segments), per chart how many actors rest in each state, and how much trace history it kept.', { snapshot: z.string() }, read, async ({ snapshot }) => {
      const { pack, system: name, hostTimeMs } = packOf(snapshot);
      const segments = pack.segments.map((segment) => (!segment.live ? { segment: segment.index, live: false } : {
        segment: segment.index, live: true, charts: (segment.charts ?? []).map((c) => c.name), actors: segment.actors.length,
      }));
      const charts = {};
      for (const segment of live(pack)) for (const actor of segment.actors) (charts[actor.chartName ?? '?'] ??= true);
      const byChart = Object.fromEntries(Object.keys(charts).map((chart) => [chart, countStates(pack, chart)]));
      const t = pack.traces;
      return ok({ system: name, hostTimeMs, segments, states: byChart, traces: t ? { spans: t.spans.length, ...t.retained } : 'tracing off' });
    });
    tool('snapshot_actors', 'The actors of a snapshot, filtered by chart, a state they are in (any active state id), status (idle, runnable, hibernated, ...) or name prefix. Paged.', {
      snapshot: z.string(), chart: z.string().optional(), state: z.string().optional(), status: z.string().optional(), name: z.string().optional(), offset: z.number().default(0), limit: z.number().default(25),
    }, read, async (q) => {
      const { pack } = packOf(q.snapshot);
      const rows = [];
      for (const segment of live(pack)) {
        for (const actor of segment.actors) {
          if (q.chart && actor.chartName !== q.chart) continue;
          if (q.status && stateName(actor.state) !== q.status) continue;
          if (q.state && !statesOf(segment, actor).includes(q.state)) continue;
          if (q.name && !(actor.name ?? '').startsWith(q.name)) continue;
          rows.push(rowOf(segment, actor));
        }
      }
      rows.sort((a, b) => Number(a.session) - Number(b.session));
      return ok({ total: rows.length, offset: q.offset, actors: rows.slice(q.offset, q.offset + q.limit) });
    });
    tool('snapshot_variables', 'The variables of the actors of a snapshot (one session, or those in a chart and state), read from a copy restored from it. A hibernated actor\'s are in the application\'s store.', { snapshot: z.string(), session: z.number().optional(), chart: z.string().optional(), state: z.string().optional(), limit: z.number().default(50) }, read, async ({ snapshot, session, chart, state, limit }) => {
      const snap = packOf(snapshot);
      const sessions = [];
      for (const segment of live(snap.pack)) {
        for (const actor of segment.actors) {
          if (session != null && Number(actor.session) !== session) continue;
          if (chart && actor.chartName !== chart) continue;
          if (state && !statesOf(segment, actor).includes(state)) continue;
          if (stateName(actor.state) !== 'hibernated' && sessions.length < limit) sessions.push(BigInt(actor.session));
        }
      }
      const { copy } = copyOf(snap);
      try {
        // posted together, before the copy's first turn runs anything: the snapshot's values
        const asked = sessions.map((s) => [s, copy.actorContext({ session: s }).then((r) => r.variables, (e) => ({ refused: e.message }))]);
        const out = [];
        for (const [s, variables] of asked) out.push({ session: s, variables: await variables });
        return ok({ actors: out });
      } finally {
        copy.close();
      }
    });
    tool('snapshot_chart', 'A chart as a snapshot holds it: its states with how many actors rest in each, its transitions (events, conditions, targets) and its data.', { snapshot: z.string(), chart: z.string() }, read, async ({ snapshot, chart }) => {
      const { pack } = packOf(snapshot);
      const segment = live(pack).filter((s) => (s.charts ?? []).some((c) => c.name === chart)).at(-1);
      if (!segment) return refused(`no live segment holds chart ${chart}`);
      const c = segment.charts.find((x) => x.name === chart);
      const counts = new Map();
      for (const actor of segment.actors) if (actor.chartName === chart) for (const i of actor.configuration ?? []) counts.set(i, (counts.get(i) ?? 0) + 1);
      return ok({
        segment: segment.index, datamodel: c.datamodel, data: c.data.map((d) => d.id),
        states: c.states.map((s, i) => ({ id: s.id, kind: s.kind, parent: s.parent ? c.states[s.parent].id : null, actors: counts.get(i) ?? 0 })).filter((s, i) => c.states[i].parent !== null),
        transitions: c.transitions.filter((t) => t.type !== 'initial').map((t) => ({ from: c.states[t.source]?.id, events: t.events, cond: t.cond || undefined, to: t.targets.map((i) => c.states[i].id) })),
      });
    });
    tool('snapshot_traces', 'The spans of the trace history a snapshot kept (the moments before it was taken), newest last: filter by session or a span name prefix.', { snapshot: z.string(), session: z.number().optional(), name: z.string().optional(), limit: z.number().default(40) }, read, async ({ snapshot, session, name, limit }) => {
      const { pack } = packOf(snapshot);
      if (!pack.traces) return refused('this snapshot holds no trace history (tracing off)');
      let spans = pack.traces.spans;
      if (session != null) spans = spans.filter((s) => Number(s.attributes?.['scxml.session_id'] ?? -1) === session);
      if (name) spans = spans.filter((s) => s.name.startsWith(name));
      return ok({ total: spans.length, retained: pack.traces.retained, spans: spans.slice(-limit).map(({ trace, span, parent, name: n, start, end, attributes, events }) => ({ trace, span, parent, name: n, start, end, attributes, events })) });
    });
    tool('rehearse', 'Rehearse a deploy on a copy of a snapshot: restore it with stand-ins for the I/O processors, services and transport, add a draft, migrate a chart to it, then run the copy for advanceMs on a simulated clock. Reports the migration, the states before and after, and what the copy sent and invoked. The live host is not touched.', {
      snapshot: z.string(), draft: z.string(), chart: z.string(), advanceMs: z.number().default(0),
      services: z.record(z.string()).optional().describe('how each stand-in service answers, by type: silent (never, the default), done, error (error.communication), or the name of an event it sends back'),
    }, read, async ({ snapshot, draft, chart, advanceMs, services = {} }) => {
      const snap = packOf(snapshot);
      const d = shared.drafts.get(draft);
      if (!d) return refused(`no draft ${draft}`);
      let result;
      const record = { sends: [], invocations: [], logs: [], errors: [] };
      record.handlers = {
        log: (e) => record.logs.push(`${e.session} ${e.label}: ${e.text}`),
        migrationDone: (r) => { result = r; },
        onError: (actor, error) => record.errors.push({ session: actor.id, error: error.message }),
        onHandlerError: (error) => record.errors.push({ handler: error.message }),
      };
      const { copy, clock } = copyOf(snap, { record, services });
      try {
        const before = countStates(snap.pack, chart);
        copy.addImage(d.image);
        const migration = copy.migrate({ chart });
        await copy.idle();
        for (let elapsed = 0; elapsed < advanceMs; elapsed += 250) {
          clock.increment(Math.min(250, advanceMs - elapsed));
          await copy.idle();
        }
        const after = countStates(decodePack(copy.snapshot()), chart);
        const tally = (list) => list.reduce((counts, item) => ({ ...counts, [item]: (counts[item] ?? 0) + 1 }), {});
        return ok({ migration, result: migrationSummary(result), before, after, sends: tally(record.sends), invocations: tally(record.invocations), logs: record.logs.slice(0, 20), errors: record.errors.slice(0, 20) });
      } finally {
        copy.close();
      }
    });
    return mcp;
  }

  /**
   * Serves the tools over Streamable HTTP at `http://host:port/mcp`, one
   * McpServer per MCP session. `token`: a bearer token every request must
   * carry. Returns `{ url, close() }`.
   */
  async function listen({ port = 0, host = '127.0.0.1', path = '/mcp', token } = {}) {
    const sessions = new Map();
    const http = createHTTPServer(async (request, response) => {
      if (new URL(request.url, 'http://x').pathname !== path) {
        response.statusCode = 404;
        response.end();
        return;
      }
      if (token && request.headers.authorization !== `Bearer ${token}`) {
        response.statusCode = 401;
        response.end();
        return;
      }
      const id = request.headers['mcp-session-id'];
      let transport = id && sessions.get(id);
      if (!transport) {
        transport = new StreamableHTTPServerTransport({ sessionIdGenerator: randomUUID, onsessioninitialized: (sid) => sessions.set(sid, transport) });
        transport.onclose = () => sessions.delete(transport.sessionId);
        await server().connect(transport);
      }
      await transport.handleRequest(request, response);
    });
    await new Promise((resolve) => http.listen(port, host, resolve));
    const address = http.address();
    return {
      url: `http://${host}:${address.port}${path}`,
      close: () => new Promise((resolve) => {
        for (const transport of sessions.values()) transport.close();
        http.close(resolve);
      }),
    };
  }

  return { server, listen };
}

// @tinyactors/mcp: a Model Context Protocol server over running tinyactors
// hosts, for an agent that inspects a system, sends to it, reads its
// traces and rehearses and deploys new versions of its charts.
//
// An opt-in package beside @tinyactors/node: JavaScript over that
// package's public API and the MCP SDK (@modelcontextprotocol/sdk),
// nothing else. Every language's server is the same three parts
// (docs/adr/0021 in the repository root; tinyactors-core/mcp/README.md):
//
// - the catalog, core's tinyactors-core/mcp/tools.json, which
//   @tinyactors/node ships as `@tinyactors/node/mcp-tools.json`: every
//   tool's name, description, input schema, annotations, the option that
//   gates it, and what runs it;
// - the forwarding: a `snapshot` tool is a method of core's snapshot
//   reader (snapshot.request), a `host` tool core's requests through the
//   typed methods (HOST_METHODS);
// - the binding operations (BINDING): what only this language can do:
//   the tap, which wraps a host's options when the host is made (a
//   handler is fixed then) and keeps what the agent reads later, a copy of
//   a snapshot restored with stand-ins, compile and deploy, send, ask,
//   terminate and the services.
import { createServer as createHTTPServer } from 'node:http';
import { randomUUID } from 'node:crypto';
import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/streamableHttp.js';
import { CallToolRequestSchema, ListToolsRequestSchema } from '@modelcontextprotocol/sdk/types.js';
import { compileCharts, decodeTraces, Host, readSnapshot, SimulatedClock } from '@tinyactors/node';
import catalog from '@tinyactors/node/mcp-tools.json' with { type: 'json' };

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

// An integer past 2^53 is its decimal text; bytes are their length.
const plain = (value) => JSON.parse(JSON.stringify(value ?? null, (_, v) => {
  if (typeof v === 'bigint') return v <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(v) : v.toString();
  if (v instanceof Uint8Array) return `<${v.length} bytes>`;
  return v;
}));
const ok = (value, note) => {
  const structured = plain(value);
  const text = JSON.stringify(structured, null, 2);
  return {
    content: [{ type: 'text', text: note ? `${note}\n${text}` : text }],
    structuredContent: Array.isArray(structured) || structured === null || typeof structured !== 'object' ? { items: structured } : structured,
  };
};
const refused = (message, structured = {}) => ({ isError: true, content: [{ type: 'text', text: message }], structuredContent: { error: message, ...structured } });

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

// ---- the catalog's arguments ----------------------------------------------------------------------

// The catalog's schemas are a small subset of JSON Schema: an object of
// properties, each typed string, number, integer, boolean, array or object
// (or untyped: any value), or an enum, with defaults and required names.
const TYPES = {
  string: (v) => typeof v === 'string',
  number: (v) => typeof v === 'number' && Number.isFinite(v),
  integer: (v) => Number.isSafeInteger(v),
  boolean: (v) => typeof v === 'boolean',
  array: Array.isArray,
  object: (v) => v !== null && typeof v === 'object' && !Array.isArray(v),
};

/** The tool's arguments checked against its schema, its defaults filled in; throws a TypeError. */
export function argumentsOf(tool, given = {}) {
  const { properties = {}, required = [] } = tool.inputSchema;
  const args = {};
  for (const key of Object.keys(given)) {
    if (!(key in properties)) throw new TypeError(`${tool.name} takes no argument ${key}`);
  }
  for (const [key, schema] of Object.entries(properties)) {
    const value = given[key] ?? schema.default;
    if (value === undefined) {
      if (required.includes(key)) throw new TypeError(`${tool.name} needs ${key}`);
      continue;
    }
    if (schema.type && !TYPES[schema.type](value)) throw new TypeError(`${tool.name}: ${key} is a ${schema.type}`);
    if (schema.enum && !schema.enum.includes(value)) throw new TypeError(`${tool.name}: ${key} is one of ${schema.enum.join(', ')}`);
    args[key] = value;
  }
  return args;
}

// ---- forwarding: core's requests through the typed methods -----------------------------------------

// A method of docs/requests.md, by its name, to the typed method of the
// host that posts it (no raw post: ADR 0012, decision 3).
const HOST_METHODS = {
  'host.stats': (host, params) => host.hostStats(params),
  'trace.watch': (host, params) => host.traceWatch(params),
  'trace.unwatch': (host, params) => host.traceUnwatch(params),
  'actor.get': (host, params) => host.actorGet(params),
  'actor.state': (host, params) => host.actorState(params),
  'actor.context': (host, params) => host.actorContext(params),
};

const without = (object, ...keys) => Object.fromEntries(Object.entries(object).filter(([key]) => !keys.includes(key)));

// ---- the server ----------------------------------------------------------------------------------

/**
 * The MCP tools over `systems`: `{ [name]: { host, tap?, sources? } }`, where
 * `tap` is the tap whose options made the host (needed for traces, logs,
 * services, migrations and every copy) and `sources` maps a chart's name to
 * its SCXML (the image holds no source). Options: `allowDeploy` (deploy,
 * migrate, end an invocation), `allowTerminate`, `snapshotKeep` (8).
 * `server()` makes one MCP server (one per connection); `listen()` serves
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
  // An actor named by a session id or the name it was spawned with.
  const actorOf = (s, which) => s.host.actor(/^\d+$/.test(String(which)) ? BigInt(which) : String(which));
  const snapshotOf = (id) => {
    const snap = shared.snapshots.get(id);
    if (!snap) throw new Error(`no snapshot ${id}; take one with the snapshot tool`);
    return snap;
  };

  // A copy of a snapshot: the same system, config and datamodel, stand-ins
  // for the world (tinyactors-core/mcp/README.md, "Copies and stand-ins").
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

  // The resting configurations of a chart's actors: snapshot.summary's states.
  const restOf = (reader, chart) => reader.request('snapshot.summary').states[chart] ?? {};

  // The binding operations: what the catalog marks {"binding": name}.
  const BINDING = {
    async systems() {
      const out = [];
      for (const [name, s] of Object.entries(systems)) {
        const stats = await s.host.hostStats();
        out.push({ system: name, charts: stats.charts.map((c) => c.name), actors: stats.host.actors, hibernated: stats.host.hibernated, segments: stats.host.segments, nowMs: s.host.now() });
      }
      return ok(out);
    },
    async send({ system: name, actor, event, data }) {
      const a = actorOf(system(name), actor);
      a.send(event, data);
      return ok({ sent: event, to: a.id });
    },
    async ask({ system: name, actor, event, data, timeoutMs }) {
      const [reply, replyData] = await actorOf(system(name), actor).ask(event, data, { signal: AbortSignal.timeout(timeoutMs) });
      return ok({ reply, data: replyData });
    },
    async traces({ system: name, session, name: prefix, limit }) {
      let spans = tapOf(system(name), 'traces').spans;
      if (session != null) spans = spans.filter((s) => Number(s.attributes?.['scxml.session_id'] ?? -1) === session);
      if (prefix) spans = spans.filter((s) => s.name.startsWith(prefix));
      return ok({ total: spans.length, spans: spans.slice(-limit) });
    },
    async logs({ system: name, session, limit }) {
      let lines = tapOf(system(name), 'logs').logs;
      if (session != null) lines = lines.filter((l) => Number(l.session) === session);
      return ok({ total: lines.length, logs: lines.slice(-limit) });
    },
    async services({ system: name, session }) {
      const out = [];
      for (const [s, list] of tapOf(system(name), 'services').services) {
        if (session != null && Number(s) !== session) continue;
        for (const i of list) if (i.running) out.push({ session: s, invokeid: i.invokeid, type: i.type, src: i.src });
      }
      return ok(out);
    },
    async end_service({ system: name, session, invokeid, as }) {
      const list = tapOf(system(name), 'end_service').services.get(BigInt(session)) ?? [];
      const invocation = list.find((i) => i.running && i.invokeid === invokeid);
      if (!invocation) return refused(`no running service ${invokeid} of session ${session}`);
      if (as === 'done') invocation.done();
      else invocation.sendBack('error.communication');
      return ok({ ended: invokeid, session, as });
    },
    async chart_source({ system: name, chart }) {
      const text = system(name).sources?.[chart];
      return text ? { content: [{ type: 'text', text }] } : refused(`no source for chart ${chart}`);
    },
    async compile({ name, text }) {
      let compiled;
      try {
        compiled = compileCharts([{ name, text }]);
      } catch (error) {
        return refused(`does not compile: ${error.message}`, { diagnostics: plain(error.diagnostics ?? []) });
      }
      const id = `d${shared.nextDraft++}`;
      shared.drafts.set(id, { name, text, image: compiled.image, diagnostics: compiled.diagnostics });
      compiled.close();
      return ok({ draft: id, diagnostics: shared.drafts.get(id).diagnostics });
    },
    async deploy({ system: name, draft, chart, migrate, wake, timeoutMs }) {
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
    },
    async terminate({ system: name, actor }) {
      const a = actorOf(system(name), actor);
      a.terminate();
      return ok({ terminated: a.id });
    },
    async snapshot({ system: name }) {
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
      const reader = readSnapshot(bytes);
      shared.snapshots.set(id, { id, system: name, bytes, reader, hostTimeMs: s.host.now() });
      while (shared.snapshots.size > (options.snapshotKeep ?? 8)) shared.snapshots.delete(shared.snapshots.keys().next().value);
      const actors = reader.request('snapshot.summary').segments.reduce((n, segment) => n + (segment.actors ?? 0), 0);
      return ok({ snapshot: id, system: name, bytes: bytes.length, actors, hostTimeMs: shared.snapshots.get(id).hostTimeMs });
    },
    async snapshot_variables({ snapshot, session, chart, state, limit }) {
      const snap = snapshotOf(snapshot);
      const sessions = [];
      for (let offset = 0; sessions.length < limit;) {
        const page = snap.reader.request('snapshot.actors', { ...(chart ? { chart } : {}), ...(state ? { state } : {}), offset, limit: 512 });
        for (const row of page.actors) {
          if (session != null && Number(row.session) !== session) continue;
          if (row.status !== 'hibernated' && sessions.length < limit) sessions.push(BigInt(row.session));
        }
        offset += page.actors.length;
        if (!page.actors.length || offset >= page.total) break;
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
    },
    async rehearse({ snapshot, draft, chart, advanceMs, services = {} }) {
      const snap = snapshotOf(snapshot);
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
        const before = restOf(snap.reader, chart);
        copy.addImage(d.image);
        const migration = copy.migrate({ chart });
        await copy.idle();
        for (let elapsed = 0; elapsed < advanceMs; elapsed += 250) {
          clock.increment(Math.min(250, advanceMs - elapsed));
          await copy.idle();
        }
        const after = restOf(readSnapshot(copy.snapshot()), chart);
        const tally = (list) => list.reduce((counts, item) => ({ ...counts, [item]: (counts[item] ?? 0) + 1 }), {});
        return ok({ migration, result: migrationSummary(result), before, after, sends: tally(record.sends), invocations: tally(record.invocations), logs: record.logs.slice(0, 20), errors: record.errors.slice(0, 20) });
      } finally {
        copy.close();
      }
    },
  };

  // One tool's call: its arguments checked, its gate, then what runs it.
  async function call(tool, given) {
    const args = argumentsOf(tool, given);
    if (tool.gate && !options[tool.gate]) return refused(`${tool.name} is off for this server (${tool.gate})`);
    const { run } = tool;
    if (run.snapshot) return ok(snapshotOf(args.snapshot).reader.request(run.snapshot, without(args, 'snapshot')));
    if (run.host) {
      const s = system(args.system);
      const params = without(args, 'system', 'actor');
      if (args.actor !== undefined) params.session = actorOf(s, args.actor).id;
      if (typeof run.host === 'string') return ok(await HOST_METHODS[run.host](s.host, params));
      const out = {};
      for (const [key, method] of Object.entries(run.host)) {
        try {
          out[key] = await HOST_METHODS[method](s.host, params);
        } catch (error) {
          out[key] = { refused: error.message };
        }
      }
      return ok(out);
    }
    return BINDING[run.binding](args);
  }

  const tools = new Map(catalog.tools.map((tool) => [tool.name, tool]));

  function server() {
    const mcp = new Server({ name: catalog.name, version: '0.0.0' }, { capabilities: { tools: {} } });
    mcp.setRequestHandler(ListToolsRequestSchema, async () => ({
      tools: catalog.tools.map(({ name, description, inputSchema, annotations }) => ({ name, description, inputSchema, annotations })),
    }));
    mcp.setRequestHandler(CallToolRequestSchema, async (request) => {
      const tool = tools.get(request.params.name);
      if (!tool) return refused(`no tool ${request.params.name}`);
      try {
        return await call(tool, request.params.arguments ?? {});
      } catch (error) {
        return refused(`${error.name}: ${error.message}`, error.reason ? { reason: error.reason, path: error.path } : {});
      }
    });
    return mcp;
  }

  /**
   * Serves the tools over Streamable HTTP at `http://host:port/mcp`, one
   * MCP server per MCP session. `token`: a bearer token every request must
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

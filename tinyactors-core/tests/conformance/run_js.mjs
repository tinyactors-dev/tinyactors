#!/usr/bin/env node
/**
 * Host conformance driver for the JavaScript bindings (see README.md here):
 * runs every scenario through the public API — createSystem({ clock }) on
 * simulated time, fromSCXML, sendMany, hibernate with a memory store,
 * snapshot/restore, reload/migrate — and checks the expectations.
 *
 *   node tests/conformance/run_js.mjs [--binding node|web] [dir] [--only name] [-v]
 */
import { readFileSync, readdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { Duplex } from 'node:stream';

const args = process.argv.slice(2);
const flag = (name, def) => {
  const i = args.indexOf(name);
  return i < 0 ? def : args[i + 1];
};
const which = flag('--binding', 'node');
const only = flag('--only', null);
const verbose = args.includes('-v');
const here = path.dirname(new URL(import.meta.url).pathname);
const dir = args.find((a, i) => !a.startsWith('-') && !['--binding', '--only'].includes(args[i - 1])) || here;
const root = path.resolve(here, '../..');
const ta = which === 'web' ? await import(path.join(root, 'bindings/web/node.mjs')) : createRequire(import.meta.url)(path.join(root, 'bindings/node'));

/* atomic states of a state value: 'a', { p: { l: 'l2', r: 'r1' } }, { q: {} } (an atomic region) */
const leaves = (v) =>
  typeof v === 'string' ? [v] : Object.entries(v || {}).flatMap(([k, sub]) => (typeof sub === 'string' || (sub && Object.keys(sub).length) ? leaves(sub) : [k]));
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

async function run(sc) {
  const docs = Object.fromEntries(Object.entries(sc.documents).map(([k, v]) => [k, Array.isArray(v) ? v.join('\n') : v]));
  let now = 0;
  const logs = [];
  const store = ta.createMemoryStore();
  const options = { id: 'conformance', clock: () => now, logger: (...a) => logs.push(String(a.length > 1 ? a[1] : a[0])), hibernation: { store, sweepMs: 0 } };
  let system = ta.createSystem(options);
  const actors = new Map(); /* name -> Actor (kept after it finished) */
  const snapshots = {};
  const logicOf = (doc, extra = []) => ta.fromSCXML(docs[doc], { documents: Object.fromEntries(extra.map((d) => [d, docs[d]])) });
  const actor = (name) => {
    const a = actors.get(name);
    if (!a) throw new Error(`no actor "${name}"`);
    return a;
  };
  const subst = (v) => {
    if (typeof v === 'string' && v.startsWith('@')) return `#_scxml_${actor(v.slice(1)).address.split('#_scxml_')[1]}`;
    if (Array.isArray(v)) return v.map(subst);
    if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, subst(x)]));
    return v;
  };
  const settle = async () => {
    for (let i = 0; i < 50; i++) {
      system.flush();
      await new Promise((r) => setImmediate(r)); /* store promises, migration jobs */
      if (system.flush()) return;
    }
  };
  const failures = [];
  let step = 0;
  for (const s of sc.steps) {
    step++;
    const where = `step ${step} ${JSON.stringify(s).slice(0, 80)}`;
    try {
      if (s.spawn) {
        actors.set(s.as, ta.createActor(logicOf(s.spawn, s.documents), { system, systemID: s.as, input: s.input }).start());
      } else if (s.send) actor(s.send).send({ type: s.event, data: subst(s.data) });
      else if (s.sendMany) system.sendMany(s.sendMany.map((i) => [actor(i.to), { type: i.event, data: subst(i.data) }]));
      else if (s.advance !== undefined) now += s.advance;
      else if (s.hibernate) {
        await settle();
        if (system.hibernate(actor(s.hibernate)) !== 1) throw new Error(`${s.hibernate} could not be hibernated`);
      } else if (s.snapshot) snapshots[s.snapshot] = system.snapshot();
      else if (s.restore) {
        system.stop();
        system = ta.createSystem({ ...options, restore: snapshots[s.restore] });
        for (const n of [...actors.keys()]) system.get(n) ? actors.set(n, system.get(n)) : actors.delete(n);
      } else if (s.reload) system.reload(logicOf(s.reload, s.documents));
      else if (s.migrate) {
        const m = system.migrate(undefined, s.migrate);
        await settle();
        await Promise.race([m.done, new Promise((_, no) => setTimeout(() => no(new Error('migration did not end')), 2000))]);
      } else if (s.expect) {
        const e = s.expect;
        const got = logs.splice(0);
        if (e.logs && !same(got, e.logs)) failures.push(`${where}: logs ${JSON.stringify(got)}, expected ${JSON.stringify(e.logs)}`);
        if (e.logsUnordered && !same([...got].sort(), [...e.logsUnordered].sort())) failures.push(`${where}: logs ${JSON.stringify(got)}, expected (any order) ${JSON.stringify(e.logsUnordered)}`);
        for (const [n, want] of Object.entries(e.states || {})) {
          const st = leaves(actor(n).getSnapshot().value).sort();
          if (!same(st, [...want].sort())) failures.push(`${where}: ${n} in ${JSON.stringify(st)}, expected ${JSON.stringify(want)}`);
        }
        for (const [n, want] of Object.entries(e.done || {})) {
          const snap = actor(n).getSnapshot();
          if (snap.status !== 'done') failures.push(`${where}: ${n} is ${snap.status}, expected done`);
          else if (want !== true && !same(snap.output, want)) failures.push(`${where}: ${n} output ${JSON.stringify(snap.output)}, expected ${JSON.stringify(want)}`);
        }
        for (const n of e.hibernated || []) if (!actor(n).hibernated) failures.push(`${where}: ${n} is not hibernated`);
        for (const n of e.awake || []) if (actor(n).hibernated) failures.push(`${where}: ${n} is hibernated`);
      } else throw new Error('unknown step');
      if (!s.expect) await settle();
    } catch (err) {
      failures.push(`${where}: ${err.message}`);
      break;
    }
    if (verbose) console.log(`  ${where}`);
  }
  system.stop();
  return failures;
}

let failed = 0;

/* protocol vectors (docs/protocol.md, Conformance): the same files the C
 * implementation runs (tests/unit/test_address.c, ...) */
const VECTORS = new Set(['addresses.json', 'values.json', 'span_ids.json', 'traceparent.json']);
function addresses() {
  const v = JSON.parse(readFileSync(path.join(here, 'addresses.json'), 'utf8'));
  const failures = [];
  for (const c of v.valid) {
    const a = ta.parseAddress(c.text);
    if (!a) { failures.push(`should parse: ${c.text}`); continue; }
    if (a.system !== c.system || a.kind !== c.kind) failures.push(`${c.text}: ${a.system} ${a.kind}`);
    if (c.kind === 'session' && a.session !== BigInt(c.session)) failures.push(`${c.text}: session ${a.session}`);
    if (c.kind === 'session' && ta.formatAddress(a.system, a.session) !== c.text) failures.push(`${c.text}: formats back differently`);
    if (c.kind === 'name' && a.name !== c.name) failures.push(`${c.text}: name ${a.name}`);
  }
  for (const c of v.invalid) if (ta.parseAddress(c.text)) failures.push(`should not parse (${c.why}): ${c.text}`);
  for (const c of v.format) {
    const got = ta.formatAddress(c.system, c.session !== undefined ? BigInt(c.session) : c.name);
    if (got !== c.text) failures.push(`format ${JSON.stringify(c)}: ${got}`);
  }
  return failures;
}
if (!only) {
  const failures = addresses();
  console.log(`${failures.length ? 'FAIL' : 'ok  '} ${which.padEnd(4)} addresses.json (protocol vectors)`);
  for (const x of failures) console.log(`       ${x}`);
  failed += failures.length ? 1 : 0;
}

/* neutral values (docs/protocol.md, Values): the vectors the C datamodel runs too */
function values() {
  const v = JSON.parse(readFileSync(path.join(here, 'values.json'), 'utf8'));
  const failures = [];
  const sys = ta.createSystem({ id: 'vectors' });
  for (const c of v.roundtrip) {
    const got = JSON.stringify(ta.encodeWire(ta.decodeWire(c.wire, sys)));
    const want = JSON.stringify(c.back !== undefined ? c.back : c.wire);
    if (got !== want) failures.push(`roundtrip "${c.name}": ${got}, expected ${want}`);
  }
  for (const c of v.encode) {
    const got = JSON.stringify(ta.encodeWire(new Function(`return (${c.ecmascript});`)()));
    if (got !== JSON.stringify(c.wire)) failures.push(`encode "${c.name}": ${got}, expected ${JSON.stringify(c.wire)}`);
  }
  for (const c of v.invalid) {
    let threw = false;
    try {
      ta.encodeWire(new Function(`return (${c.ecmascript});`)());
    } catch {
      threw = true;
    }
    if (!threw) failures.push(`invalid "${c.name}": encoded`);
  }
  sys.stop();
  return failures;
}

/* trace values (docs/adr/0006 in the repository root): the vectors
 * tests/unit/test_trace.c runs; the binding's functions call the C code */
function traceValues() {
  const failures = [];
  for (const k of JSON.parse(readFileSync(path.join(here, 'span_ids.json'), 'utf8')).keys) {
    const got = ta.stepSpanID(k.session, BigInt(k.macrostep), k.microstep);
    if (got !== k.span) failures.push(`span ${k.session}/${k.macrostep}/${k.microstep}: ${got}, expected ${k.span}`);
  }
  const v = JSON.parse(readFileSync(path.join(here, 'traceparent.json'), 'utf8'));
  for (const c of v.valid) {
    const p = ta.parseTraceparent(c.text);
    if (!p || p.traceID !== c.trace || p.parentSpanID !== c.parent || p.flags !== c.flags) {
      failures.push(`should parse: ${JSON.stringify(c.text)}: ${JSON.stringify(p)}`);
      continue;
    }
    const back = ta.formatTraceparent({ traceID: p.traceID, spanID: p.parentSpanID, flags: p.flags });
    if (back !== (c.format || c.text)) failures.push(`${JSON.stringify(c.text)} formats back as ${back}`);
  }
  for (const c of v.invalid) if (ta.parseTraceparent(c.text)) failures.push(`should not parse (${c.why}): ${JSON.stringify(c.text)}`);
  return failures;
}

/* link transcripts (docs/protocol.md, Link protocol): this binding's system
 * serves links; the driver plays the peer on a fake transport, feeding `in`
 * frames and matching `out` frames (a subset of fields; "~regex" strings).
 * Each transcript runs twice: on a text transport (one JSON text per message,
 * like a WebSocket) and on a byte stream (length-prefixed frames, framed by
 * hand here). On the stream, consecutive `in` frames go out together, cut into
 * chunks of odd sizes: frames split anywhere, several in one chunk. */
function matches(want, got) {
  if (typeof want === 'string' && want.startsWith('~')) return typeof got === 'string' && new RegExp(want.slice(1)).test(got);
  if (want && typeof want === 'object') {
    if (!got || typeof got !== 'object') return false;
    if (Array.isArray(want)) return Array.isArray(got) && want.length === got.length && want.every((w, i) => matches(w, got[i]));
    return Object.keys(want).every((k) => matches(want[k], got[k]));
  }
  return want === got;
}
const CUTS = [1, 2, 7, 3, 64, 5, 1000];
const frameOf = (msg) => {
  const body = Buffer.from(JSON.stringify(msg), 'utf8');
  const head = Buffer.alloc(4);
  head.writeUInt32BE(body.length);
  return Buffer.concat([head, body]);
};
/* a byte-stream connection: { in(msg), flush(), close() }; frames the system writes go to `out` */
function byteStream(accept, out) {
  let rx = Buffer.alloc(0);
  let pending = [];
  const d = new Duplex({
    read() {},
    write(chunk, _enc, done) {
      rx = Buffer.concat([rx, chunk]);
      while (rx.length >= 4 && rx.length >= 4 + rx.readUInt32BE(0)) {
        const n = rx.readUInt32BE(0);
        out.push(JSON.parse(rx.subarray(4, 4 + n).toString('utf8')));
        rx = rx.subarray(4 + n);
      }
      done();
    },
  });
  accept(d);
  return {
    in: (msg) => pending.push(frameOf(msg)),
    flush() {
      const bytes = Buffer.concat(pending);
      pending = [];
      for (let at = 0, i = 0; at < bytes.length; i++) {
        const n = CUTS[i % CUTS.length];
        d.push(bytes.subarray(at, at + n));
        at += n;
      }
    },
    close: () => d.push(null),
  };
}
/* a text connection (one JSON text per message, WebSocket-like) */
function textStream(accept, out) {
  const ws = { send: (t) => out.push(JSON.parse(t)), close() {}, onmessage: null, onclose: null };
  accept(ws);
  return { in: (msg) => ws.onmessage({ data: JSON.stringify(msg) }), flush() {}, close: () => ws.onclose() };
}
async function transcript(sc, framed) {
  const docs = Object.fromEntries(Object.entries(sc.documents).map(([k, v]) => [k, v.join('\n')]));
  let now = 0;
  const logs = [];
  const system = ta.createSystem({ id: sc.system, clock: () => now, logger: (...a) => logs.push(String(a.length > 1 ? a[1] : a[0])) });
  for (const s of sc.spawn) ta.createActor(ta.fromSCXML(docs[s.doc]), { system, systemID: s.as }).start();
  let accept = null;
  const served = ta.serveLinks(system, { ...sc.serve, accept: (on) => void (accept = on) });
  const out = [];
  let conn = null;
  const connect = () => {
    conn = (framed ? byteStream : textStream)(accept, out);
  };
  const settle = async () => {
    for (let i = 0; i < 20; i++) {
      system.flush();
      await new Promise((r) => setImmediate(r));
    }
  };
  connect();
  const failures = [];
  let step = 0;
  for (const s of sc.transcript) {
    step++;
    const where = `step ${step} ${JSON.stringify(s).slice(0, 70)}`;
    if (s.in) {
      conn.in(s.in);
      if (!framed) await settle();
      continue;
    }
    conn.flush();
    await settle();
    if (s.out) {
      await settle();
      const got = out.shift();
      if (!got) failures.push(`${where}: nothing was sent`);
      else if (!matches(s.out, got)) failures.push(`${where}: sent ${JSON.stringify(got)}`);
    } else if (s.close) conn.close();
    else if (s.connect) connect();
    else if (s.advance !== undefined) now += s.advance;
    else if (s.expect) {
      await settle();
      const got = logs.splice(0);
      if (s.expect.logs && !matches(s.expect.logs, got)) failures.push(`${where}: logs ${JSON.stringify(got)}`);
      if (s.expect.silent && out.length) failures.push(`${where}: also sent ${JSON.stringify(out)}`);
    } else failures.push(`${where}: unknown step`);
    await settle();
  }
  conn.flush();
  await settle();
  await served.close();
  system.stop();
  return failures;
}

if (!only) {
  const failures = values();
  console.log(`${failures.length ? 'FAIL' : 'ok  '} ${which.padEnd(4)} values.json (protocol vectors)`);
  for (const x of failures) console.log(`       ${x}`);
  failed += failures.length ? 1 : 0;
  const traceFailures = traceValues();
  console.log(`${traceFailures.length ? 'FAIL' : 'ok  '} ${which.padEnd(4)} span_ids.json, traceparent.json (trace vectors)`);
  for (const x of traceFailures) console.log(`       ${x}`);
  failed += traceFailures.length ? 1 : 0;
  const ldir = path.join(here, 'links/');
  for (const f of readdirSync(ldir).filter((f) => f.endsWith('.json')).sort()) {
    const sc = JSON.parse(readFileSync(path.join(ldir, f), 'utf8'));
    for (const framed of [false, true]) {
      const fails = await transcript(sc, framed);
      console.log(`${fails.length ? 'FAIL' : 'ok  '} ${which.padEnd(4)} ${framed ? 'framed:' : ''}links/${f}: ${sc.name}`);
      for (const x of fails) console.log(`       ${x}`);
      failed += fails.length ? 1 : 0;
    }
  }
}

const files = readdirSync(dir).filter((f) => f.endsWith('.json') && !VECTORS.has(f)).sort();
for (const f of files) {
  const sc = JSON.parse(readFileSync(path.join(dir, f), 'utf8'));
  if (only && sc.name !== only) continue;
  const failures = await run(sc);
  console.log(`${failures.length ? 'FAIL' : 'ok  '} ${which.padEnd(4)} ${sc.name}`);
  for (const x of failures) console.log(`       ${x}`);
  failed += failures.length ? 1 : 0;
}
process.exit(failed ? 1 : 0);

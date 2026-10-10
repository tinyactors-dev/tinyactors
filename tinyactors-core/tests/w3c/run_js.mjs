#!/usr/bin/env node
/**
 * W3C SCXML IRP runner for the JavaScript bindings: every converted test runs
 * as `fromSCXML(document, { documents })` in its own system, with the C
 * ECMAScript datamodel (createSystem({ ecmascript: 'quickjs' })), and is
 * judged by the "Outcome" log the W3C stylesheet places in the pass/fail
 * final states — the same judgment as tests/w3c/run_w3c.c.
 *
 *   node tests/w3c/run_js.mjs <converted-dir> [--binding node|web] [--require-all]
 *                             [--only ID] [--timeout MS]
 *
 * Tests run concurrently on real time (delays are short in the suite).
 */
import { readFileSync, readdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(name);
  return i < 0 ? def : args[i + 1];
};
const dir = args[0];
if (!dir || dir.startsWith('--')) {
  console.error('usage: run_js.mjs <converted-dir> [--binding node|web] [--require-all] [--only ID]');
  process.exit(2);
}
const which = opt('--binding', 'node');
const only = opt('--only', null);
const timeoutMs = Number(opt('--timeout', 8000));
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const ta =
  which === 'web'
    ? await import(path.join(root, 'bindings/web/node.mjs'))
    : createRequire(import.meta.url)(path.join(root, 'bindings/node'));

const tests = readFileSync(path.join(dir, 'index.txt'), 'utf8')
  .split('\n')
  .filter(Boolean)
  .map((l) => {
    const [id, conf, rel, deps] = l.split(/\s+/);
    return { id, conf, rel, deps: deps && deps !== '-' ? deps.split(',') : [] };
  })
  .filter((t) => !only || t.id === only);

/* A BasicHTTP Event I/O Processor (SCXML Appendix D.2) on a loopback: what a
 * session sends to its location comes back to it as an HTTP POST would
 * (form fields, _scxmleventname, the raw message) — the application's part,
 * as the C runner's host plays it (tests/w3c/run_w3c.c). */
const BASIC_HTTP = 'http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor';
const LOCATION = 'http://localhost:8080/scxml';
const basicHTTP = {
  location: LOCATION,
  /* the outcome comes before the sender goes on: an unreachable target is
   * error.communication on its internal queue (577), the POST arrives before
   * the session's next send (201) */
  sync: true,
  send(m, { system }) {
    if (m.target !== LOCATION) throw new Error('unreachable');
    const text = (v) => (typeof v === 'string' ? v : JSON.stringify(v));
    const fields = m.data && typeof m.data === 'object' ? Object.entries(m.data).map(([k, v]) => [k, text(v)]) : [];
    if (m.event) fields.unshift(['_scxmleventname', m.event]);
    const body = typeof m.data === 'string' ? encodeURIComponent(m.data) : fields.map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`).join('&');
    const raw = `POST /scxml HTTP/1.1\r\nHost: localhost:8080\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: ${body.length}\r\n\r\n${body}`;
    const named = fields.find(([k]) => k === '_scxmleventname');
    const data = Object.fromEntries(fields.filter(([k]) => k !== '_scxmleventname'));
    system.send(m.origin, { type: named ? named[1] : 'HTTP.POST', ...data }, { origin: LOCATION, origintype: BASIC_HTTP, raw });
  },
};

function run(t) {
  return new Promise((resolve) => {
    let logic;
    try {
      /* the dependency documents and every other file of the test (e.g. <data src> text) */
      const tdir = path.join(dir, path.dirname(t.rel));
      const names = new Set([...t.deps.map((d) => path.basename(d)), ...readdirSync(tdir).filter((f) => f !== path.basename(t.rel))]);
      const documents = Object.fromEntries([...names].map((f) => [f, readFileSync(path.join(tdir, f), 'utf8')]));
      logic = ta.fromSCXML(readFileSync(path.join(dir, t.rel), 'utf8'), { documents });
    } catch (e) {
      return resolve({ result: 'COMPILE', detail: e.message.split('\n').slice(0, 3).join(' ') });
    }
    const system = ta.createSystem({ ecmascript: 'quickjs', ioProcessors: { [BASIC_HTTP]: basicHTTP } });
    const log = [];
    let settled = false;
    const finish = (result, detail) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try {
        system.stop();
      } catch {
        /* already stopped */
      }
      resolve({ result, detail, log });
    };
    const timer = setTimeout(() => finish('TIMEOUT'), timeoutMs);
    const logger = (label, message) => {
      if (only) console.error(`    log ${label}: ${message}`);
      log.push(`${label}: ${message}`);
      if (label === 'Outcome') finish(message === 'pass' ? 'pass' : 'FAIL');
    };
    try {
      ta.createActor(logic, { system, logger }).start();
    } catch (e) {
      finish('COMPILE', e.message);
    }
  });
}

const counts = { mandatory: {}, optional: {} };
const failures = [];
const results = await Promise.all(
  tests.map(async (t) => ({ t, r: await run(t) })),
);
for (const { t, r } of results.sort((a, b) => Number(a.t.id) - Number(b.t.id))) {
  const c = counts[t.conf] || (counts[t.conf] = {});
  c[r.result] = (c[r.result] || 0) + 1;
  if (r.result !== 'pass') failures.push(`${t.id} ${t.conf} ${r.result}${r.detail ? ` (${r.detail})` : ''}`);
}
const line = (k) => `${k}: ${['pass', 'FAIL', 'COMPILE', 'TIMEOUT'].map((r) => `${counts[k][r] || 0} ${r === 'pass' ? 'pass' : r.toLowerCase()}`).join(', ')}`;
console.log(`W3C IRP via the ${which} binding (the C ECMAScript datamodel)`);
console.log(line('mandatory'));
console.log(line('optional'));
for (const f of failures) console.log(`  ${f}`);
process.exit(args.includes('--require-all') && failures.length ? 1 : 0);

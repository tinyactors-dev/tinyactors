// Starts torture-bench with its dashboard server on an ephemeral port and
// checks the live protocol with Node's built-in WebSocket client and fetch:
// static files, hello/history/sample/log messages, a mutation command, the
// stop command, and that every custom element has its <template>; the layout
// (no horizontal overflow, aligned charts) in headless Chrome when one is found.
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
import test from 'node:test';
import { createServer } from 'node:net';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');
const bench = process.env.TORTURE_BENCH || path.join(root, 'build/dev/bin/torture-bench');

function startBench(scenario, args = [], env = {}) {
  const where = scenario ? [path.isAbsolute(scenario) ? scenario : path.join(here, '..', 'scenarios', scenario)] : ['--scenarios', path.join(here, '..', 'scenarios')];
  const p = spawn(bench, [...where, '--port', '0', '--quiet', '--scale', '0.05', ...args], { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
  let stdout = '';
  p.stdout.on('data', (d) => (stdout += d));
  const exited = new Promise((resolve) => p.on('exit', (code) => resolve({ code, stdout })));
  const port = new Promise((resolve, reject) => {
    let err = '';
    p.stderr.on('data', (d) => {
      err += d;
      const m = /dashboard on http:\/\/127\.0\.0\.1:(\d+)\//.exec(err);
      if (m) resolve(Number(m[1]));
    });
    p.on('exit', () => reject(new Error(`bench exited early:\n${err}`)));
  });
  return { p, port, exited };
}

function client(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`);
  const messages = [];
  const waiters = [];
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    messages.push(m);
    for (const w of [...waiters]) if (w.pred(m)) {
      waiters.splice(waiters.indexOf(w), 1);
      w.resolve(m);
    }
  });
  const next = (pred, ms = 8000) =>
    new Promise((resolve, reject) => {
      const hit = messages.find(pred);
      if (hit) return resolve(hit);
      const w = { pred, resolve };
      waiters.push(w);
      setTimeout(() => reject(new Error('timed out waiting for a message')), ms);
    });
  const opened = new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve);
    ws.addEventListener('error', reject);
  });
  return { ws, messages, next, opened };
}

test('dashboard files are served with the right types', async () => {
  const b = startBench('throughput', ['--duration', '20']);
  try {
    const port = await b.port;
    const base = `http://127.0.0.1:${port}`;
    for (const [file, type] of [['/', 'text/html'], ['/chamber.html', 'text/html'], ['/app.js', 'text/javascript'], ['/home.js', 'text/javascript'], ['/service.js', 'text/javascript'], ['/tokens.css', 'text/css'], ['/components.css', 'text/css'], ['/page.css', 'text/css']]) {
      const r = await fetch(base + file);
      assert.equal(r.status, 200, file);
      assert.ok(r.headers.get('content-type').startsWith(type), `${file}: ${r.headers.get('content-type')}`);
    }
    assert.equal((await fetch(`${base}/nope.js`)).status, 404);
    assert.equal((await fetch(`${base}/../etc/passwd`)).status, 404 /* normalized by fetch */);
    // every custom element has a <template id> in the page that defines it
    for (const [page, script, min] of [['/chamber.html', '/app.js', 8], ['/', '/home.js', 4]]) {
      const html = await (await fetch(base + page)).text();
      const js = (await (await fetch(base + script)).text()) + (await (await fetch(`${base}/service.js`)).text());
      const defined = [...js.matchAll(/customElements\.define\('([a-z-]+)'/g)].map((m) => m[1]);
      assert.ok(defined.length >= min, `custom elements in ${script}`);
      for (const name of defined) assert.match(html, new RegExp(`<template id="${name}">`), `${name} in ${page}`);
    }
    // one round trip after the page: it names its whole module graph and the
    // components' stylesheet up front (behind a slow proxy every extra level of
    // the chain is a visible delay), and no template links a stylesheet of its
    // own (a shadow root's <link> renders its content unstyled until it loads)
    for (const [page, entry] of [['/chamber.html', 'app.js'], ['/', 'home.js']]) {
      const html = await (await fetch(base + page)).text();
      const named = new Set([...html.matchAll(/<(?:script type="module" src|link rel="(?:modulepreload|preload)" href)="([^"]+)"/g)].map((m) => m[1]));
      const graph = new Set(), todo = [entry];
      while (todo.length) {
        const f = todo.pop();
        if (graph.has(f)) continue;
        graph.add(f);
        const src = await (await fetch(`${base}/${f}`)).text();
        for (const m of src.matchAll(/^import (?:[^'"]* from )?'\.\/([^']+)'/gm)) todo.push(m[1]);
        for (const m of src.matchAll(/fetch\('([^']+\.css)'\)/g)) todo.push(m[1]);
      }
      assert.ok(graph.has('components.css') && graph.size >= 4, `${page}: ${[...graph]}`);
      for (const f of graph) assert.ok(named.has(f), `${page} does not preload ${f}`);
      assert.doesNotMatch(html, /<template[^]*<link rel="stylesheet"/, `${page}: a template links a stylesheet`);
    }
    // design tokens are used, not ad-hoc values
    assert.match(readFileSync(path.join(root, 'web/torture/components.css'), 'utf8'), /var\(--space-5\)/);
  } finally {
    b.p.kill('SIGTERM');
    await b.exited;
  }
});

test('live protocol: hello, history, samples, logs, mutations, stop', async () => {
  const b = startBench('federation', ['--duration', '30']);
  const port = await b.port;
  await new Promise((r) => setTimeout(r, 1200)); // some history first
  const c = client(port);
  await c.opened;
  const hello = await c.next((m) => m.type === 'hello');
  assert.equal(hello.scenario.name, 'federation');
  assert.deepEqual(hello.scenario.systems.sort(), ['billing', 'gateway', 'orders']);
  assert.ok(hello.scenario.mutations.some((m) => m.id === 'kill-billing'));
  const history = await c.next((m) => m.type === 'history');
  assert.ok(history.samples.length >= 2, `history has ${history.samples.length} samples`);
  assert.ok(history.logs.some((l) => /federation:/.test(l.message)));
  const live = await c.next((m) => m.type === 'sample' && m.t > history.samples.at(-1).t);
  assert.ok(live.systems.find((s) => s.name === 'gateway').actors > 0);
  // a mutation from the dashboard: billing disappears from the samples
  c.ws.send(JSON.stringify({ cmd: 'mutate', id: 'kill-billing' }));
  await c.next((m) => m.type === 'log' && m.level === 'mutation' && /Kill billing/.test(m.message));
  await c.next((m) => m.type === 'sample' && !m.systems.some((s) => s.name === 'billing'));
  // pause / resume the systems
  c.ws.send(JSON.stringify({ cmd: 'pause' }));
  await c.next((m) => m.type === 'sample' && m.paused === true);
  c.ws.send(JSON.stringify({ cmd: 'resume' }));
  await c.next((m) => m.type === 'sample' && m.paused === false && m.t > live.t);
  // stop: the run ends with a summary
  c.ws.send(JSON.stringify({ cmd: 'stop' }));
  const summary = await c.next((m) => m.type === 'summary', 10000);
  assert.equal(summary.scenario, 'federation');
  assert.ok(summary.totals.remoteOut > 0);
  assert.ok(summary.mutations.some((m) => m.id === 'kill-billing'));
  // the server keeps serving after the scenario finished
  const after = await fetch(`http://127.0.0.1:${port}/api/scenarios`);
  assert.equal((await after.json()).state, 'finished');
  c.ws.close();
  b.p.kill('SIGTERM');
  await b.exited;
});

test('home page: scenario catalog, and opening a scenario switches the bench', async () => {
  const b = startBench(null, ['--duration', '30']);
  try {
    const port = await b.port;
    const base = `http://127.0.0.1:${port}`;
    const api = await (await fetch(`${base}/api/scenarios`)).json();
    assert.equal(api.state, 'idle');
    assert.equal(api.current, null);
    const ids = api.scenarios.map((s) => s.id);
    for (const id of ['campaign', 'capacity', 'chaos', 'crowd', 'federation', 'throughput']) assert.ok(ids.includes(id), id);
    for (const s of api.scenarios) assert.ok(s.scenario.title && (s.scenario.summary || s.scenario.description), `${s.id} has a title and summary`);
    const home = await (await fetch(`${base}/`)).text();
    assert.match(home, /<template id="tb-scenario-card">/);
    // a chamber page opens "crowd"; then another opens "throughput"
    const c = client(port);
    await c.opened;
    const idle = await c.next((m) => m.type === 'hello');
    assert.equal(idle.state, 'idle');
    c.ws.send(JSON.stringify({ cmd: 'open', scenario: 'crowd' }));
    const crowd = await c.next((m) => m.type === 'hello' && m.id === 'crowd');
    assert.equal(crowd.state, 'running');
    await c.next((m) => m.type === 'sample' && m.systems.some((s) => s.name === 'crowd'));
    c.ws.send(JSON.stringify({ cmd: 'open', scenario: '../etc' }));
    c.ws.send(JSON.stringify({ cmd: 'open', scenario: 'throughput' }));
    await c.next((m) => m.type === 'hello' && m.id === 'throughput');
    await c.next((m) => m.type === 'sample' && m.systems.some((s) => s.name === 'storm'));
    assert.equal((await (await fetch(`${base}/api/scenarios`)).json()).current, 'throughput');
    c.ws.close();
  } finally {
    b.p.kill('SIGTERM');
    await b.exited;
  }
});

test('services: PostgreSQL status over HTTP and the WebSocket', async () => {
  const b = startBench(null);
  try {
    const port = await b.port;
    const STATES = ['unknown', 'unavailable', 'starting', 'missing', 'available', 'failed'];
    const services = await (await fetch(`http://127.0.0.1:${port}/api/services`)).json();
    assert.ok(STATES.includes(services.postgres.state), services.postgres.state);
    assert.equal(services.postgres.address, '127.0.0.1:54320');
    assert.equal(services.postgres.start, 'mise run pg:start');
    const catalog = await (await fetch(`http://127.0.0.1:${port}/api/scenarios`)).json();
    assert.ok(STATES.includes(catalog.services.postgres.state));
    const pg = catalog.scenarios.find((e) => e.id === 'postgres');
    assert.deepEqual(pg.scenario.services, ['postgres']);
    // pushed on connect, and probed live (a known state within a few seconds)
    const c = client(port);
    await c.opened;
    const first = await c.next((m) => m.type === 'service');
    assert.equal(first.name, 'postgres');
    const probed = await c.next((m) => m.type === 'service' && m.state !== 'unknown', 5000).catch(() => first);
    assert.ok(STATES.includes(probed.state));
    // unknown services are ignored (no crash, no state change)
    c.ws.send(JSON.stringify({ cmd: 'start-service', name: 'nope' }));
    assert.equal((await fetch(`http://127.0.0.1:${port}/api/services`)).status, 200);
    c.ws.close();
  } finally {
    b.p.kill('SIGTERM');
    await b.exited;
  }
});

test('a busy scenario opened from the catalog streams at once', async () => {
  // chaos keeps its system busy every turn; the bench must still serve the
  // network after a switch, when the scenario clock restarts at 0
  const b = startBench(null, ['--duration', '30']);
  try {
    const port = await b.port;
    await new Promise((r) => setTimeout(r, 4000)); // idle first: the bench clock runs ahead
    const c = client(port);
    await c.opened;
    await c.next((m) => m.type === 'hello');
    const t0 = Date.now();
    c.ws.send(JSON.stringify({ cmd: 'open', scenario: 'chaos' }));
    await c.next((m) => m.type === 'sample' && m.systems.some((s) => s.name === 'chaos'), 10000);
    const ms = Date.now() - t0;
    assert.ok(ms < 2500, `first chaos sample after ${ms} ms`);
    const t1 = Date.now();
    assert.equal((await fetch(`http://127.0.0.1:${port}/api/services`)).status, 200);
    assert.ok(Date.now() - t1 < 1000, 'HTTP answers while chaos runs');
    c.ws.close();
  } finally {
    b.p.kill('SIGTERM');
    await b.exited;
  }
});

test('PostgreSQL Promises: FIFO, errors, async finish/checks, cancellation on switch', { timeout: 15000 }, async () => {
  const dir = mkdtempSync(path.join(os.tmpdir(), 'tb-async-'));
  writeFileSync(path.join(dir, 'scenario.json'), JSON.stringify({ name: 'async', scripts: ['setup.js'] }));
  writeFileSync(path.join(dir, 'setup.js'), `
    let rows;
    bench.after(0, async () => {
      const a = await bench.pg.query('one');
      if (a[0].value !== 'one') throw Error('wrong first result');
      rows = await Promise.all([bench.pg.query('two'), bench.pg.query('three')]);
      try { await bench.pg.query('bad'); throw Error('no SQL error'); }
      catch (e) { if (e.message !== 'deliberate SQL error') throw e; }
      await bench.pg.query('after');
    });
    bench.onFinish(async () => {
      const r = await bench.pg.query('finish');
      bench.result({ value: r[0].value });
    });
    bench.check('FIFO results', () => rows[0][0].value === 'two' && rows[1][0].value === 'three');
    bench.check('async SQL check', async () => (await bench.pg.query('check'))[0].value === 'check');
    bench.check('intentional async false', async () => false);
  `);
  const frame = (type, body) => {
    const h = Buffer.alloc(5); h[0] = type.charCodeAt(0); h.writeUInt32BE(body.length + 4, 1);
    return Buffer.concat([h, body]);
  };
  const ready = frame('Z', Buffer.from('I'));
  const result = (value) => {
    const column = Buffer.concat([Buffer.from([0, 1]), Buffer.from('value\0'), Buffer.alloc(18)]);
    const data = Buffer.alloc(6); data.writeUInt16BE(1); data.writeUInt32BE(Buffer.byteLength(value), 2);
    return Buffer.concat([frame('T', column), frame('D', Buffer.concat([data, Buffer.from(value)])), ready]);
  };
  const sockets = new Set(), queries = [];
  const server = createServer((s) => {
    sockets.add(s); s.on('close', () => sockets.delete(s)); s.on('error', () => {});
    let input = Buffer.alloc(0), startup = true;
    s.on('data', (data) => {
      input = Buffer.concat([input, data]);
      while (input.length >= (startup ? 4 : 5)) {
        const length = input.readUInt32BE(startup ? 0 : 1) + (startup ? 0 : 1);
        if (input.length < length) break;
        const msg = input.subarray(0, length); input = input.subarray(length);
        if (startup) { startup = false; s.write(Buffer.concat([frame('R', Buffer.alloc(4)), ready])); continue; }
        const sql = msg.subarray(5, -1).toString();
        queries.push(sql);
        if (sql === 'hang') continue;
        const bytes = sql === 'bad' ? Buffer.concat([frame('E', Buffer.from('Mdeliberate SQL error\0\0')), ready]) : result(sql);
        // Deliberately split headers, field lengths and rows across loop turns.
        let i = 0;
        const timer = setInterval(() => {
          if (s.destroyed || i >= bytes.length) return clearInterval(timer);
          s.write(bytes.subarray(i, i += 3));
        }, 2);
      }
    });
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const env = { BENCH_POSTGRES_PORT: String(server.address().port) };
  let b;
  try {
    b = startBench(dir, ['--headless', '--duration', '0.05'], env);
    // headless has no port announcement
    b.port.catch(() => {});
    const { code, stdout } = await b.exited;
    assert.equal(code, 1, 'an async false check must fail, not pass as a truthy Promise');
    const summary = JSON.parse(stdout.trim().split('\n').at(-1));
    assert.deepEqual(summary.failures, ['intentional async false']);
    assert.equal(summary.results[0].value, 'finish');
    assert.deepEqual(queries, ['one', 'two', 'three', 'bad', 'after', 'finish', 'check']);
    writeFileSync(path.join(dir, 'setup.js'), `bench.after(0, async () => { await bench.pg.query('hang'); bench.log('stale completion'); });`);
    b = startBench(dir, ['--duration', '30', '--scenarios', path.join(here, '..', 'scenarios')], env);
    const port = await b.port;
    const c = client(port); await c.opened;
    for (let i = 0; !queries.includes('hang') && i < 100; i++) await new Promise((r) => setTimeout(r, 10));
    assert.ok(queries.includes('hang'));
    const start = performance.now();
    c.ws.send(JSON.stringify({ cmd: 'open', scenario: 'throughput' }));
    await c.next((m) => m.type === 'hello' && m.id === 'throughput');
    assert.ok(performance.now() - start < 500, 'switch cancels an unanswered query without waiting');
    await c.next((m) => m.type === 'sample' && m.systems.some((s) => s.name === 'storm'));
    assert.ok(!c.messages.some((m) => m.message === 'stale completion'));
    c.ws.close();
  } finally {
    b?.p.kill('SIGTERM');
    if (b) await b.exited;
    for (const s of sockets) s.destroy();
    await new Promise((r) => server.close(r));
    rmSync(dir, { recursive: true, force: true });
  }
});

// Fixed-cadence requests, not request/await/request: a stall must count every
// request it delays (no coordinated omission). An invalid open requests hello
// without switching scenarios, so this also works against the old server.
async function responsiveness(port, seconds = 5) {
  const c = client(port);
  await c.opened;
  await c.next((m) => m.type === 'hello');
  const http = [], ws = [], pending = [], requests = [];
  c.ws.addEventListener('message', (e) => {
    if (JSON.parse(e.data).type === 'hello' && pending.length) ws.push(performance.now() - pending.shift());
  });
  const interval = setInterval(() => {
    const start = performance.now();
    pending.push(start);
    c.ws.send(JSON.stringify({ cmd: 'open', scenario: '../invalid' }));
    requests.push(fetch(`http://127.0.0.1:${port}/api/services`, { signal: AbortSignal.timeout(15000) })
      .then(async (r) => { assert.equal(r.status, 200); await r.text(); http.push(performance.now() - start); }));
  }, 20);
  try {
    await new Promise((r) => setTimeout(r, seconds * 1000));
    clearInterval(interval);
    await Promise.all(requests);
    for (let i = 0; pending.length && i < 1500; i++) await new Promise((r) => setTimeout(r, 10));
    assert.equal(pending.length, 0, 'all WebSocket round trips completed');
    const stats = (a) => {
      a.sort((x, y) => x - y);
      assert.ok(a.length >= 150, `${a.length} round trips`);
      return { p99: +a[Math.floor(a.length * .99)].toFixed(2), max: +a.at(-1).toFixed(2), n: a.length };
    };
    return { http: stats(http), ws: stats(ws) };
  } finally {
    clearInterval(interval);
    c.ws.close();
  }
}

for (const workload of ['chaos', 'blackhole', 'postgres-millions']) {
  test(`responsiveness: ${workload}`, { timeout: 45000 }, async (t) => {
    const sockets = new Set();
    const hole = workload === 'blackhole' ? createServer((s) => {
      sockets.add(s);
      s.on('close', () => sockets.delete(s));
    }) : null;
    if (hole) await new Promise((r) => hole.listen(0, '127.0.0.1', r));
    const args = ['--duration', '30'];
    if (workload === 'postgres-millions') args.push('--param', 'reset=1', '--param', 'hot=100000', '--param', 'database=tinyactors_torture_test');
    const b = startBench(null, args, hole ? { BENCH_POSTGRES_PORT: String(hole.address().port) } : {});
    let c;
    try {
      const port = await b.port;
      c = client(port);
      await c.opened;
      await c.next((m) => m.type === 'hello');
      if (workload === 'postgres-millions') {
        const service = await c.next((m) => m.type === 'service' && m.state !== 'unknown');
        if (service.state !== 'available') return t.skip(`PostgreSQL ${service.state}`);
      }
      // Idle first also catches accidentally mixing the scenario and loop clocks.
      await new Promise((r) => setTimeout(r, 300));
      if (!hole) c.ws.send(JSON.stringify({ cmd: 'open', scenario: workload }));
      const result = await responsiveness(port);
      console.log(`responsiveness ${workload}: ${JSON.stringify(result)}`);
      for (const [transport, stats] of Object.entries(result))
        assert.ok(stats.p99 < 50, `${workload} ${transport} p99 ${stats.p99} ms >= 50 ms`);
      if (workload === 'postgres-millions')
        await c.next((m) => m.type === 'log' && /100,?000 session actors stored/.test(m.message), 15000);
    } finally {
      c?.ws.close();
      b.p.kill('SIGTERM');
      await b.exited;
      for (const s of sockets) s.destroy();
      if (hole) await new Promise((r) => hole.close(r));
    }
  });
}

/* ---- layout, measured in headless Chrome over the DevTools protocol (skipped without Chrome) */

function findChrome() {
  const candidates = [process.env.CHROME];
  const ab = path.join(os.homedir(), '.agent-browser/browsers');
  if (existsSync(ab)) for (const d of readdirSync(ab).sort().reverse()) candidates.push(path.join(ab, d, 'chrome'));
  for (const dir of (process.env.PATH || '').split(':'))
    for (const name of ['chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable']) candidates.push(path.join(dir, name));
  return candidates.find((c) => c && existsSync(c));
}

async function startChrome(exe) {
  const profile = mkdtempSync(path.join(os.tmpdir(), 'tb-chrome-'));
  const p = spawn(exe, ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--hide-scrollbars', `--user-data-dir=${profile}`, '--remote-debugging-port=0', 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
  const exited = new Promise((resolve) => p.on('exit', resolve));
  const endpoint = await new Promise((resolve, reject) => {
    let err = '';
    p.stderr.on('data', (d) => {
      err += d;
      const m = /DevTools listening on (ws:\/\/\S+)/.exec(err);
      if (m) resolve(m[1]);
    });
    p.on('exit', () => reject(new Error(`chrome exited early:\n${err}`)));
  });
  const close = async () => {
    p.kill('SIGTERM');
    await exited;
    rmSync(profile, { recursive: true, force: true });
  };
  return { endpoint, close };
}

/** A page target over a flat DevTools session; evaluate(expr) returns the value. */
async function cdpPage(endpoint) {
  const ws = new WebSocket(endpoint);
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve);
    ws.addEventListener('error', reject);
  });
  let id = 0;
  const pending = new Map();
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) reject(new Error(JSON.stringify(m.error)));
      else resolve(m.result);
    }
  });
  const send = (method, params = {}, sessionID) =>
    new Promise((resolve, reject) => {
      pending.set(++id, { resolve, reject });
      ws.send(JSON.stringify({ id, method, params, sessionID }));
    });
  const { targetID } = await send('Target.createTarget', { url: 'about:blank' });
  const { sessionID } = await send('Target.attachToTarget', { targetID, flatten: true });
  const page = (method, params) => send(method, params, sessionID);
  const evaluate = async (expression) => {
    const r = await page('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  };
  return { page, evaluate, close: () => ws.close() };
}

// Runs in the page: what overflows, what is squeezed, what is misaligned.
const MEASURE = `(() => {
  const app = document.querySelector('tb-app') || document.querySelector('tb-home');
  const root = app.shadowRoot;
  const vw = document.documentElement.clientWidth;
  const title = root.querySelector('.scenario-title').getBoundingClientRect();
  // chart cards that share a grid row have the same height
  const rows = new Map();
  for (const c of root.querySelectorAll('tb-chart:not([hidden])')) {
    const r = c.shadowRoot.querySelector('.chart').getBoundingClientRect();
    const k = Math.round(c.getBoundingClientRect().top);
    rows.set(k, [...(rows.get(k) || []), Math.round(r.height)]);
  }
  const uneven = [...rows.values()].filter((h) => Math.max(...h) - Math.min(...h) > 1);
  const cards = [...root.querySelectorAll('tb-scenario-card')].map((c) => c.getBoundingClientRect().right);
  const page = root.querySelector('.page').getBoundingClientRect();
  return { vw, scrollWidth: document.documentElement.scrollWidth, titleWidth: title.width, uneven,
    cardsOverflow: cards.filter((r) => r > page.right - 16).length, tables: root.querySelectorAll('.results-scroll table').length };
})()`;

test('dashboard layout: no horizontal overflow, readable title, aligned charts (headless Chrome)', async (t) => {
  const exe = findChrome();
  if (!exe) return t.skip('no Chrome/Chromium found (set CHROME=...)');
  const b = startBench('reload', ['--duration', '4']);
  const chrome = await startChrome(exe);
  const cdp = await cdpPage(chrome.endpoint);
  try {
    const base = `http://127.0.0.1:${await b.port}`;
    // the finished state has the widest content: the result table
    await cdp.page('Page.navigate', { url: `${base}/chamber.html` });
    for (let i = 0; i < 100 && !(await cdp.evaluate(`!!document.querySelector('tb-app')?.finished`)); i++) await new Promise((r) => setTimeout(r, 100));
    assert.ok(await cdp.evaluate(`document.querySelector('tb-app').finished`), 'the run finished');
    for (const [width, height] of [[390, 844], [1280, 720], [1920, 1080]]) {
      await cdp.page('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
      await new Promise((r) => setTimeout(r, 300));
      const m = await cdp.evaluate(MEASURE);
      assert.equal(m.scrollWidth, m.vw, `chamber at ${width}px: page scrolls sideways (${m.scrollWidth}px)`);
      assert.ok(m.titleWidth >= 150, `chamber at ${width}px: the title is ${m.titleWidth}px wide`);
      assert.deepEqual(m.uneven, [], `chamber at ${width}px: chart cards of one row differ in height`);
      assert.equal(m.tables, 1, 'the result table sits in its scroll container');
    }
    await cdp.page('Page.navigate', { url: `${base}/` });
    for (let i = 0; i < 50 && !(await cdp.evaluate(`document.querySelector('tb-home')?.shadowRoot?.querySelectorAll('tb-scenario-card').length`)); i++) await new Promise((r) => setTimeout(r, 100));
    for (const [width, height] of [[360, 740], [1280, 720]]) {
      await cdp.page('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
      await new Promise((r) => setTimeout(r, 300));
      const m = await cdp.evaluate(MEASURE);
      assert.equal(m.scrollWidth, m.vw, `home at ${width}px: page scrolls sideways`);
      assert.equal(m.cardsOverflow, 0, `home at ${width}px: scenario cards stick out of the page`);
    }
  } finally {
    cdp.close();
    await chrome.close();
    b.p.kill('SIGTERM');
    await b.exited;
  }
});

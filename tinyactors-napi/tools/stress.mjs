// Hibernation under load: many actors, few resident, the rest in SQLite
// (docs/plan.xml, p2-hibernate; translated from
// tinyactors-cpython/tools/stress.py).
//
//     node tools/stress.mjs [--actors N] [--resident R] [--events E] [--datamodel native|quickjs]
//                           [--sample S] [--seed K]
//
// The application's side of hibernation (bindings.md section 4; README,
// "Hibernation"), as a program would write it:
//
// 1. populate: spawn N actors of a counter chart; whenever more than R are
//    resident, the oldest go to the store (host.hibernate, one SQLite row
//    each);
// 2. traffic: E events to actors drawn at random; an event for a
//    hibernated actor makes a WAKE, whose handler reads the blob and wakes
//    it (host.wake); after every batch the CLOCK sweep (host.sweep) picks
//    the cold actors, then the least recently used, and they are
//    hibernated again until R are resident;
// 3. verify: a sample of actors (all of them when N is small) reports its
//    count, which must be the events it was sent; after every eviction at
//    most R actors are resident (hostStats).
//
// The store is node:sqlite under node and bun:sqlite under bun, each
// runtime's own SQLite. Exits with status 1 when a check fails. Prints the
// times, the rates, the store's size, the process's peak memory and the
// host's region per actor.

import { mkdtempSync, rmSync, statSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { compileCharts, Host, TinyactorsError } from '@tinyactors/node';

// The counter of tinyactors-cpython/tools/charts/counter.ecmascript.scxml.
const COUNTER = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="counter" initial="s">
  <datamodel>
    <data id="n" expr="0" />
    <data id="last" expr="null" />
  </datamodel>
  <state id="s">
    <transition event="inc">
      <assign location="n" expr="n + 1" />
      <assign location="last" expr="_event.data" />
    </transition>
    <transition event="report">
      <log label="count" expr="_sessionid + ' ' + n" />
    </transition>
  </state>
</scxml>
`;

const BATCH = 256;

function options(argv) {
  const given = { actors: 2_000_000, resident: 1000, events: 200_000, sample: 10_000, datamodel: 'native', seed: 1 };
  for (let i = 0; i < argv.length; i += 2) {
    const name = argv[i].replace(/^--/, '');
    if (!(name in given) || i + 1 >= argv.length) throw new Error(`unknown option ${argv[i]}`);
    given[name] = name === 'datamodel' ? argv[i + 1] : Number(argv[i + 1]);
  }
  if (!['native', 'quickjs'].includes(given.datamodel)) throw new Error('--datamodel is native or quickjs');
  return given;
}

// A seeded generator (mulberry32), so a run is the same run again.
function random(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// The hibernated actors' blobs: one SQLite row per session, written in
// transactions of 4096.
class Store {
  static async open(file) {
    if (globalThis.Bun) {
      const { Database } = await import('bun:sqlite');
      const db = new Database(file);
      return new Store({ exec: (sql) => db.run(sql), prepare: (sql) => db.prepare(sql) });
    }
    const { DatabaseSync } = await import('node:sqlite');
    const db = new DatabaseSync(file);
    return new Store({ exec: (sql) => db.exec(sql), prepare: (sql) => db.prepare(sql) });
  }

  constructor(db) {
    this.db = db;
    db.exec('PRAGMA journal_mode=WAL');
    db.exec('PRAGMA synchronous=OFF');
    db.exec('CREATE TABLE blobs (session INTEGER PRIMARY KEY, blob BLOB NOT NULL)');
    this.insert = db.prepare('INSERT OR REPLACE INTO blobs VALUES (?, ?)');
    this.select = db.prepare('SELECT blob FROM blobs WHERE session = ?');
    this.pending = [];
    this.puts = 0;
    this.gets = 0;
  }

  put(session, blob) {
    this.pending.push([session, blob]);
    this.puts++;
    if (this.pending.length >= 4096) this.flush();
  }

  flush() {
    if (this.pending.length === 0) return;
    this.db.exec('BEGIN');
    for (const [session, blob] of this.pending) this.insert.run(session, blob);
    this.db.exec('COMMIT');
    this.pending.length = 0;
  }

  get(session) {
    this.flush(); // a wake can come before a put settled: read your own writes
    this.gets++;
    return this.select.get(session).blob;
  }
}

async function main() {
  const o = options(process.argv.slice(2));
  const draw = random(o.seed);
  const failures = [];
  const directory = mkdtempSync(path.join(os.tmpdir(), 'tinyactors-stress-'));
  const file = path.join(directory, 'actors.db');
  const store = await Store.open(file);
  const resident = new Map(); // sessions in memory, least recently used first
  let wakes = 0;
  const counts = new Map();

  const wake = (host, session) => {
    host.wake(session, store.get(session));
    resident.set(session, true);
    wakes++;
  };
  const log = (host, entry) => {
    const [session, count] = entry.text.split(' ');
    counts.set(BigInt(session), Number(count));
  };

  const hibernate = (host, session) => {
    try {
      store.put(session, host.hibernate(session));
    } catch (error) {
      if (!(error instanceof TinyactorsError) || error.status !== 'TA_ERROR_STATE') throw error; // a bug: the run fails
      return false; // not at a stable point now: it stays resident
    }
    resident.delete(session);
    return true;
  };

  // Down to R resident: the CLOCK sweep's cold actors first, then the least
  // recently used (an actor the sweep skips ran since the last one: just
  // woken, say).
  const evict = (host) => {
    let hibernated = 0;
    while (resident.size > o.resident) {
      const cold = host.sweep(1 << 20, resident.size - o.resident);
      let went = 0;
      for (const session of cold) went += hibernate(host, session) ? 1 : 0;
      hibernated += went;
      if (!went) break; // nothing cold, or nothing at rest: the LRU order below
    }
    let over = resident.size - o.resident;
    for (const session of resident.keys()) {
      if (over-- <= 0) break;
      hibernated += hibernate(host, session) ? 1 : 0;
    }
    return hibernated;
  };

  let peak = 0;
  const stats = async (host) => {
    let got;
    host.hostStats(undefined, (h, result) => (got = result.host));
    await host.idle();
    return got;
  };
  const checkResident = async (host) => {
    const got = await stats(host);
    const live = Number(got.actors) - Number(got.hibernated);
    peak = Math.max(peak, live);
    if (live > o.resident) failures.push(`${live} actors resident, more than ${o.resident}`);
    return got;
  };

  // Capacity planning, as Python's: an actor slot for every actor, instance
  // and string memory for the resident ones only, tracing off (nobody
  // exports the traces here). Left to grow, a segment doubles every pool
  // together, instance memory derived from maxActors: size the pools.
  const room = o.resident + 2 * BATCH;
  const config = {
    systemID: 'stress',
    system: { maxActors: o.actors + 4 * BATCH, maxEvents: 4096, instanceBytes: 128 * room, stringBytes: 16384 + 16 * room, tracing: false },
  };
  const host = new Host(config, { datamodel: o.datamodel, wake, log });
  const compiled = compileCharts([{ name: 'counter.scxml', text: COUNTER }]);
  host.addImage(compiled.image);
  compiled.close();
  const chart = host.chart('counter');

  // 1. populate, R resident at most
  let started = performance.now();
  const sessions = new BigUint64Array(o.actors);
  for (let i = 0; i < o.actors; i++) {
    const session = host.spawn(chart);
    sessions[i] = session;
    resident.set(session, true);
    if (resident.size >= o.resident + BATCH) {
      await host.idle();
      for (const old of [...resident.keys()].slice(0, BATCH)) hibernate(host, old);
      await checkResident(host);
    }
    if (i && i % 500_000 === 0) console.log(`  populate: ${i} actors, ${((performance.now() - started) / 1000).toFixed(1)} s`);
  }
  await host.idle();
  evict(host);
  store.flush();
  const populated = (performance.now() - started) / 1000;
  let got = await checkResident(host);
  console.log(`populate: ${o.actors} actors in ${populated.toFixed(1)} s (${Math.round(o.actors / populated)} spawned and hibernated a second), ` +
    `${Number(got.actors) - Number(got.hibernated)} resident, ${got.hibernated} hibernated`);

  // 2. traffic: random actors, woken from the store, swept back
  started = performance.now();
  const sent = new Uint32Array(o.actors);
  let swept = 0;
  for (let first = 0; first < o.events; first += BATCH) {
    const items = [];
    for (let k = 0; k < Math.min(BATCH, o.events - first); k++) {
      const index = Math.floor(draw() * o.actors);
      sent[index]++;
      items.push({ target: sessions[index], event: 'inc', data: { k: index } });
      if (resident.has(sessions[index])) { // most recently used now
        resident.delete(sessions[index]);
        resident.set(sessions[index], true);
      }
    }
    if (host.sendMany(items) !== items.length) failures.push('sendMany refused events');
    await host.idle(); // WAKE: read from the store, woken, the events run
    swept += evict(host);
    await checkResident(host);
  }
  store.flush();
  const traffic = (performance.now() - started) / 1000;
  await checkResident(host);
  console.log(`traffic: ${o.events} events in ${traffic.toFixed(1)} s (${Math.round(o.events / traffic)} a second): ` +
    `${wakes} woken from the store, ${swept} swept back`);

  // 3. verify a sample
  started = performance.now();
  let sample;
  if (o.actors <= o.sample) sample = Array.from({ length: o.actors }, (_, i) => i);
  else {
    const chosen = new Set();
    while (chosen.size < o.sample) chosen.add(Math.floor(draw() * o.actors));
    sample = [...chosen];
  }
  for (let chunk = 0; chunk < sample.length; chunk += BATCH) {
    const items = sample.slice(chunk, chunk + BATCH).map((index) => ({ target: sessions[index], event: 'report' }));
    host.sendMany(items);
    await host.idle();
    evict(host);
    await checkResident(host);
  }
  const wrong = sample.filter((index) => counts.get(sessions[index]) !== sent[index]);
  if (wrong.length)
    failures.push(`${wrong.length} of ${sample.length} actors counted wrong, e.g. actor ${wrong[0]}: ` +
      `${counts.get(sessions[wrong[0]])}, sent ${sent[wrong[0]]}`);
  got = await checkResident(host);
  const region = Number(got.regionBytes);
  const verified = (performance.now() - started) / 1000;
  host.close();
  store.flush();
  const size = statSync(file).size;
  // maxRSS: kilobytes (node, and bun's process.resourceUsage)
  const peakRSS = process.resourceUsage().maxRSS * 1024;
  console.log(`verify: ${sample.length} actors' counts in ${verified.toFixed(1)} s, ${wrong.length} wrong`);
  console.log(`store: ${store.puts} puts, ${store.gets} gets, ${(size / 1e6).toFixed(1)} MB (${Math.round(size / o.actors)} B an actor)`);
  console.log(`memory: peak resident ${Math.round(peakRSS / 1e6)} MB (${Math.round(peakRSS / o.actors)} B an actor); ` +
    `region ${(region / 1e6).toFixed(1)} MB (${Math.round(region / o.actors)} B an actor); ` +
    `at most ${peak} actors resident after an eviction`);
  rmSync(directory, { recursive: true, force: true });
  for (const failure of failures) console.log(`FAIL ${failure}`);
  process.exitCode = failures.length ? 1 : 0;
}

await main();

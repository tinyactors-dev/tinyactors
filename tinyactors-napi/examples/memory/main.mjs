// What an actor, a message and a delayed send cost on this runtime.
//
//   node main.mjs [--counts N,N,...] [--datamodel native|quickjs] [--no-tracing]
//                 [--chart file.scxml] [--data JSON] [--csv file.csv]
//
// Each measurement runs in a process of its own. It sizes the host's pools
// for the count up front, takes the memory, makes N of the thing, and takes
// the memory again. It prints the difference per thing, in whole bytes:
//
// - memory: what the process holds more (its resident set), the host
//   included: plan with this number;
// - heap: of it, the datamodel's heap (variables, data): the JavaScript
//   engine's on the native datamodel, quickjs's own on quickjs;
// - core reserves: the core's records for each thing (from host.hostStats),
//   which the host reserves when it sizes its pools; they count in memory
//   once used;
// - blob: a hibernated actor's bytes in your store.
//
// --counts measures each count in turn (default 100000), and --csv writes
// every row to a file, one line each, for a chart. --chart measures an
// actor of your chart (its first <scxml>). --data is the data each message
// and each delayed send carries.
import { spawnSync } from 'node:child_process';
import { appendFileSync, existsSync, readFileSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';

const CHART = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="counter" initial="idle">
  <datamodel>
    <data id="count" expr="0"/>
  </datamodel>
  <state id="idle">
    <transition event="message">
      <assign location="count" expr="count + 1"/>
    </transition>
    <transition event="arm">
      <send event="later" delay="3600s"><content expr="_event.data"/></send>
    </transition>
  </state>
</scxml>`;

/** The actors a hibernated population keeps in memory. */
const RESIDENT = 1000;

// ---- in the child process: one measurement ----------------------------------

async function engine() {
  if (globalThis.Bun) {
    const { heapStats } = await import('bun:jsc');
    return { collect: () => globalThis.Bun.gc(true), heap: () => { const s = heapStats(); return s.heapSize + s.extraMemorySize; } };
  }
  const v8 = await import('node:v8');
  const vm = await import('node:vm');
  v8.setFlagsFromString('--expose_gc');
  const gc = vm.runInNewContext('gc');
  return { collect: () => { gc(); gc(); }, heap: () => { const u = process.memoryUsage(); return u.heapUsed + u.external; } };
}

/** The core region of a host sized by `system`, with the image added. */
async function regionOf(charts, options, system) {
  const host = new Host(charts, { ...options, config: { system } });
  const { host: stats } = await host.hostStats();
  host.close();
  return stats.regionBytes;
}

/** A store that keeps only what it was given: the blobs' count and bytes. */
class CountingStore {
  saved = 0;
  bytes = 0;

  save(_session, blob) {
    this.saved++;
    this.bytes += blob.length;
  }

  load() {
    throw new Error('nothing wakes during the measurement');
  }

  delete() {}
}

async function measure({ what, count, datamodel, tracing, chart, data }) {
  const { collect, heap } = await engine();
  const text = chart ?? CHART;
  const name = text.match(/<scxml[^>]*\sname="([^"]+)"/)?.[1];
  if (!name) throw new Error('the chart needs a name attribute on <scxml>');
  const compiled = compileCharts([{ name: 'chart.scxml', text }]);
  const image = compiled.image;
  compiled.close();
  const options = { system: 'memory', datamodel, clock: new SimulatedClock() };

  // the pools, sized for `count` up front; the region of each thing is the
  // difference between a host sized for count and one sized for none of it
  const room = 4096;
  const sizes = {
    actor: (n) => ({ maxActors: n + 16, maxEvents: count + room }),
    // the slabs for the resident actors only: a hibernated actor has no instance
    hibernated: (n) => ({ maxActors: n + RESIDENT + 16, maxEvents: room, instanceBytes: 1 << 20, stringBytes: 1 << 20 }),
    message: (n) => ({ maxEvents: n + room }),
    'delayed send': (n) => ({ maxTimers: n + 64, maxEvents: n + room }),
  };
  const system = (n) => ({ ...sizes[what](n), tracing });
  const region = (await regionOf([image], options, system(count)) - await regionOf([image], options, system(0))) / count;

  // a small host first, so the code each step runs is loaded before the baseline
  const warm = new Host([image], options);
  warm.spawn(name).send('message', data);
  if (name === 'counter') warm.spawn(name).send('arm', data);
  await warm.idle();
  warm.close();

  // memory from before the host, so its region counts as it is used; the
  // heap from after it, so it holds only what the things make
  const ids = new BigInt64Array(what === 'actor' ? count : 0); // the program's own list of actors
  const store = new CountingStore();
  collect();
  const rss = process.memoryUsage().rss;
  const host = new Host([image], {
    ...options, config: { system: system(count) }, ...(what === 'hibernated' ? { store, resident: RESIDENT } : {}),
  });
  // native: the engine's heap; quickjs: its own heap, which the engine's does not hold
  const heapNow = () => (datamodel === 'quickjs' ? host.quickjsHeapBytes : heap());
  collect();
  const before = { resident: rss, heap: heapNow() };
  const sample = () => { collect(); return { resident: process.memoryUsage().rss, heap: heapNow() }; };
  // inputs in batches, so the queue never holds many at once and the peak stays low
  const batches = async (send) => {
    for (let first = 0; first < count; first += 1000) {
      for (let i = first; i < Math.min(count, first + 1000); i++) send(i);
      await host.idle();
    }
  };
  const per = (after) => ({
    resident: Math.round((after.resident - before.resident) / count),
    heap: Math.round((after.heap - before.heap) / count),
    region: Math.round(region),
  });
  const result = {};
  if (what === 'actor') {
    for (let i = 0; i < count; i++) ids[i] = host.spawn(name).id;
    await host.idle();
    result.started = per(sample());
    await batches((i) => host.send(ids[i], 'message', data));
    result.handled = per(sample());
  } else if (what === 'hibernated') {
    await batches(() => host.spawn(name)); // after each turn the host hibernates all but RESIDENT
    result.hibernated = { ...per(sample()), blob: Math.round(store.bytes / store.saved) };
  } else {
    const actor = host.spawn('counter');
    await host.idle();
    if (what === 'message') {
      // the messages wait in the mailbox: nothing runs until the next turn
      for (let i = 0; i < count; i++) actor.send('message', data);
      result.waiting = per(sample());
    } else {
      await batches(() => actor.send('arm', data)); // each arm handled: its delayed send waits an hour
      result.pending = per(sample());
    }
  }
  host.close();
  return result;
}

// ---- in the parent process: one child per measurement, and the report --------

const ROWS = [
  ['per actor, started', 'actor', 'started'],
  ['per actor, after an event', 'actor', 'handled'],
  ['per actor, hibernated', 'hibernated', 'hibernated'],
  ['per message waiting', 'message', 'waiting'],
  ['per delayed send pending', 'delayed send', 'pending'],
];

/** Every row at `count`, each measurement in a process of its own. */
export function run({ count = 100_000, datamodel = 'native', tracing = true, chart, data = { text: 'hello' } } = {}) {
  const results = {};
  for (const what of ['actor', 'hibernated', 'message', 'delayed send']) {
    const job = JSON.stringify({ what, count, datamodel, tracing, chart: what === 'actor' || what === 'hibernated' ? chart : undefined, data });
    const child = spawnSync(process.execPath, [fileURLToPath(import.meta.url), '--job', job], { encoding: 'utf8' });
    if (child.status !== 0) throw new Error(`measuring ${what} at ${count} failed:\n${child.stderr}`);
    Object.assign(results, { [what]: JSON.parse(child.stdout) });
  }
  return results;
}

const runtime = () => (globalThis.Bun ? `bun ${globalThis.Bun.version}` : `node ${process.versions.node}`);

export function report(results, { count, datamodel, tracing }) {
  const lines = [`${runtime()}, datamodel ${datamodel}, tracing ${tracing ? 'on' : 'off'}, ${count} of each`, ''];
  lines.push(`${''.padEnd(28)}${'memory'.padStart(10)}${'heap'.padStart(10)}${'core reserves'.padStart(15)}${'blob'.padStart(8)}`);
  for (const [label, what, key] of ROWS) {
    const row = results[what][key];
    lines.push(`${label.padEnd(28)}${`${row.resident} B`.padStart(10)}${`${row.heap} B`.padStart(10)}`
               + `${`${row.region} B`.padStart(15)}${(row.blob === undefined ? '' : `${row.blob} B`).padStart(8)}`);
  }
  return lines;
}

/** The machine, for the CSV and the report: results are this machine's. */
export function machine() {
  const cpus = os.cpus();
  return `${cpus[0]?.model.trim() ?? 'unknown'}, ${cpus.length} CPUs, ${Math.round(os.totalmem() / 2 ** 30)} GiB, ${os.platform()} ${os.arch()}`;
}

const CSV_HEADER = 'runtime,datamodel,tracing,count,row,memory,heap,core_reserves,blob,machine,date\n';

export function csv(results, { count, datamodel, tracing }) {
  const date = new Date().toISOString().slice(0, 10);
  return ROWS.map(([label, what, key]) => {
    const row = results[what][key];
    return [runtime(), datamodel, tracing ? 'on' : 'off', count, `"${label}"`, row.resident, row.heap, row.region, row.blob ?? '',
            `"${machine()}"`, date].join(',');
  }).join('\n') + '\n';
}

const main = import.meta.main ?? process.argv[1] === fileURLToPath(import.meta.url);
if (main) {
  const { values } = parseArgs({
    options: {
      job: { type: 'string' }, counts: { type: 'string', default: '100000' }, datamodel: { type: 'string', default: 'native' },
      'no-tracing': { type: 'boolean', default: false }, chart: { type: 'string' }, data: { type: 'string' },
      csv: { type: 'string' },
    },
  });
  if (values.job) {
    process.stdout.write(JSON.stringify(await measure(JSON.parse(values.job))));
  } else {
    const options = {
      datamodel: values.datamodel, tracing: !values['no-tracing'],
      chart: values.chart ? readFileSync(values.chart, 'utf8') : undefined,
      data: values.data ? JSON.parse(values.data) : undefined,
    };
    if (values.csv && !existsSync(values.csv)) writeFileSync(values.csv, CSV_HEADER);
    console.log(machine());
    for (const count of values.counts.split(',').map(Number)) {
      const results = run({ ...options, count });
      console.log();
      for (const line of report(results, { ...options, count })) console.log(line);
      if (values.csv) appendFileSync(values.csv, csv(results, { ...options, count }));
    }
  }
}

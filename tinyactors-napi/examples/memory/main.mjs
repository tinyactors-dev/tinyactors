// What an actor, a message and a delayed send cost on this runtime, part
// by part.
//
//   node main.mjs [--counts N,N,...] [--datamodel native|quickjs] [--no-tracing]
//                 [--chart file.scxml] [--data JSON] [--csv file.csv]
//                 [--quickjs-memory-limit BYTES]
//
// Each measurement runs in a process of its own. It sizes every pool it
// fills for the count up front, so nothing grows while it measures, makes
// N of the thing, and prints per thing, in whole bytes:
//
// - core reserved: the core's region for one more of it, the difference
//   between a host sized for N and one sized for none, over N: what a
//   machine holds for a host sized up front (plan with this);
// - core used: what the things use of it, from the core's own counters
//   (host.stats' pools): each pool's records in use times its record's
//   size, and the slab of instances and strings;
// - host other: what the host allocates beside its regions
//   (host.stats' allocatedBytes less regionBytes), over N;
// - quickjs: on quickjs, the engine's heap and the C library's header of
//   each of its blocks (quickjsHeapBytes, quickjsHeapBlocks), over N;
// - accounted: core reserved, host other and quickjs together;
// - process: what the process holds more (its own pages, in memory or
//   swapped out), the host included. Never exact: the gap to accounted is
//   mostly the JavaScript engine holding the chart's data on the native
//   datamodel, then malloc's overhead per block and the fixed costs of a
//   host and an engine spread over N;
// - blob: a hibernated actor's bytes in your store.
//
// --counts measures each count in turn (default 100000), and --csv writes
// every row to a file, one line each, for a chart. --chart measures an
// actor of your chart (its first <scxml>). --data is the data each message
// and each delayed send carries. --quickjs-memory-limit raises quickjs's
// heap limit (1 GiB by default) for a population that needs more. A row
// that fails says what stopped it.
import { spawnSync } from 'node:child_process';
import { appendFileSync, existsSync, readFileSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';

export const CHART = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
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
export const RESIDENT = 1000;
/** Room in the events pool beyond what a row fills. */
const ROOM = 4096;
/** The C library's header beside each block quickjs allocates: two words
 * on a 64-bit machine (core's ta_datamodel_ecmascript_heap_blocks). */
const BLOCK_HEADER = 16;
/** The pool sizes of core's own check of its records (tinyactors-core's
 * tools/layout/layout.c): a record's size is the region's growth from
 * 2^16 to 2^17 of its pool, the slabs fixed, over 2^16. */
const SMALL = 1 << 16;
const FIXED_SLABS = { instanceBytes: 4096, stringBytes: 16384 };

/** The image of `text`, and its chart's name. */
export function compiled(text = CHART) {
  const name = text.match(/<scxml[^>]*\sname="([^"]+)"/)?.[1];
  if (!name) throw new Error('the chart needs a name attribute on <scxml>');
  const result = compileCharts([{ name: 'chart.scxml', text }]);
  try {
    return { image: result.image, name };
  } finally {
    result.close();
  }
}

/**
 * The host config's `system` map of a row's host, sized for `n` of its
 * thing (0: none of it): every pool the row fills holds what it fills, so
 * no pool grows during a measurement of `count`.
 */
export function sizes(what, count, n, tracing) {
  const system = {
    actor: { maxActors: n + 16, maxEvents: count + ROOM },
    // the slabs for the resident actors only: a hibernated actor has no instance
    hibernated: { maxActors: n + RESIDENT + 16, maxEvents: ROOM, instanceBytes: 1 << 20, stringBytes: 1 << 20 },
    message: { maxEvents: n + ROOM },
    'delayed send': { maxTimers: n + 64, maxEvents: n + ROOM },
  }[what];
  return { ...system, tracing };
}

// ---- in the child process: one measurement ----------------------------------

/**
 * The process's own memory in bytes: on Linux its anonymous pages, in memory
 * or swapped out (RssAnon + VmSwap); else the resident set.
 *
 * Not the whole resident set: that also counts the pages of mapped files (the
 * runtime's and the addon's code), which the kernel drops under memory
 * pressure and reads back when they run again. Dropped between the two
 * samples, they made a row negative.
 */
export function memory() {
  let status;
  try {
    status = readFileSync('/proc/self/status', 'utf8');
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    return process.memoryUsage().rss;
  }
  const kB = (name) => Number(status.match(new RegExp(`^${name}:\\s+(\\d+) kB$`, 'm'))[1]);
  return (kB('RssAnon') + kB('VmSwap')) * 1024;
}

/** A full collection, so the process column holds what lives. */
async function collector() {
  if (globalThis.Bun) return () => globalThis.Bun.gc(true);
  const v8 = await import('node:v8');
  const vm = await import('node:vm');
  v8.setFlagsFromString('--expose_gc');
  const gc = vm.runInNewContext('gc');
  return () => { gc(); gc(); };
}

/** host.stats of a host: the host's counters and its pools, summed over its segments. */
async function stats(host) {
  const { host: counters, pools } = await host.hostStats();
  const used = (name) => pools.reduce((sum, pool) => sum + pool[name].used, 0);
  const capacity = (name) => pools.map((pool) => pool[name].capacity).join(',');
  return {
    region: counters.regionBytes,
    allocated: counters.allocatedBytes,
    used: { actors: used('actors'), events: used('events'), timers: used('timers'), slab: used('slabBytes') },
    capacities: Object.fromEntries(['actors', 'events', 'timers', 'slabBytes'].map((name) => [name, capacity(name)])),
  };
}

/** The core region of a host sized by `system`, with the image added. */
async function regionOf(image, options, system) {
  const host = new Host([image], { ...options, config: { system } });
  try {
    return (await stats(host)).region;
  } finally {
    host.close();
  }
}

/**
 * The size of a record of each pool in the core's region, as the core
 * itself checks it (tools/layout/layout.c): the region's growth from 2^16
 * to 2^17 of the pool, the slabs fixed, over 2^16. `event` follows
 * `tracing` (a traced event holds a trace context).
 */
export async function units({ tracing = true, datamodel = 'native' } = {}) {
  const { image } = compiled();
  const options = { system: 'memory', datamodel, clock: new SimulatedClock() };
  const unit = async (pool) => {
    const at = async (n) => regionOf(image, options, { ...FIXED_SLABS, [pool]: n, tracing });
    return ((await at(2 * SMALL)) - (await at(SMALL))) / SMALL;
  };
  return { actor: await unit('maxActors'), event: await unit('maxEvents'), timer: await unit('maxTimers') };
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

async function measure({ what, count, datamodel, tracing, chart, data, quickjsMemoryLimit, unit }) {
  const collect = await collector();
  const { image, name } = compiled(chart ?? CHART);
  const options = {
    system: 'memory', datamodel, clock: new SimulatedClock(),
    ...(quickjsMemoryLimit ? { quickjs: { memoryLimit: quickjsMemoryLimit } } : {}),
  };
  // the region a host sized for count holds beyond one sized for none of it
  const reserved = (await regionOf(image, options, sizes(what, count, count, tracing))
                    - await regionOf(image, options, sizes(what, count, 0, tracing)));

  // a small host first, so the code each step runs is loaded before the baseline
  const warm = new Host([image], options);
  warm.spawn(name).send('message', data);
  if (name === 'counter') warm.spawn(name).send('arm', data);
  await warm.idle();
  warm.close();

  // the process from before the host, so its region counts; the host's
  // counters from after it, and after the actor a message row sends to
  const ids = new BigInt64Array(what === 'actor' ? count : 0); // the program's own list of actors
  const store = new CountingStore();
  collect();
  const own = memory();
  const host = new Host([image], {
    ...options, config: { system: sizes(what, count, count, tracing) },
    ...(what === 'hibernated' ? { store, resident: RESIDENT } : {}),
  });
  const quickjs = () => (datamodel === 'quickjs' ? host.quickjsHeapBytes + BLOCK_HEADER * host.quickjsHeapBlocks : 0);
  const sample = async () => {
    collect();
    return { process: memory(), quickjs: quickjs(), ...(await stats(host)) };
  };
  let actor;
  if (what === 'message' || what === 'delayed send') {
    actor = host.spawn('counter');
    await host.idle();
  }
  const before = { ...(await sample()), process: own };
  // inputs in batches, so the queue never holds many at once and the peak stays low
  const batches = async (send) => {
    for (let first = 0; first < count; first += 1000) {
      for (let i = first; i < Math.min(count, first + 1000); i++) send(i);
      await host.idle();
    }
  };
  const per = (bytes) => Math.round(bytes / count);
  const row = (after) => {
    const used = after.used, was = before.used;
    const core = (used.actors - was.actors) * unit.actor + (used.events - was.events) * unit.event
               + (used.timers - was.timers) * unit.timer + (used.slab - was.slab);
    const hostOther = (after.allocated - after.region) - (before.allocated - before.region);
    const engine = after.quickjs - before.quickjs;
    const grown = Object.keys(before.capacities).filter((name) => before.capacities[name] !== after.capacities[name]);
    return {
      reserved: per(reserved), used: per(core), hostOther: per(hostOther),
      quickjs: datamodel === 'quickjs' ? per(engine) : undefined,
      accounted: per(reserved + hostOther + engine), process: per(after.process - before.process),
      pools: { actors: used.actors - was.actors, events: used.events - was.events, timers: used.timers - was.timers },
      grown,
    };
  };
  const result = {};
  if (what === 'actor') {
    for (let i = 0; i < count; i++) ids[i] = host.spawn(name).id;
    await host.idle();
    result.started = row(await sample());
    try {
      await batches((i) => host.send(ids[i], 'message', data));
      result.handled = row(await sample());
    } catch (error) { // quickjs's heap full, say: the started row stands
      result.handled = { failed: `${error.name}: ${error.message}` };
    }
  } else if (what === 'hibernated') {
    await batches(() => host.spawn(name)); // after each turn the host hibernates all but RESIDENT
    // then the last RESIDENT too, so the row holds hibernated actors only: a
    // resident actor's instance holds whole slab pages. A sweep passes over an
    // actor that ran since the last one, so it takes up to three rounds.
    for (let round = 0; round < 3; round++) {
      for (const session of host.sweep(count, count)) store.save(session, host.hibernate(session));
    }
    result.hibernated = { ...row(await sample()), blob: Math.round(store.bytes / store.saved) };
  } else if (what === 'message') {
    // the messages wait in the mailbox: nothing runs until the next turn
    for (let i = 0; i < count; i++) actor.send('message', data);
    const after = await sample(); // hostStats' reply comes before the next run
    result.waiting = row(after);
  } else {
    await batches(() => actor.send('arm', data)); // each arm handled: its delayed send waits an hour
    result.pending = row(await sample());
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

const COLUMNS = [
  ['core reserved', 'reserved'], ['core used', 'used'], ['host other', 'hostOther'], ['quickjs', 'quickjs'],
  ['accounted', 'accounted'], ['process', 'process'], ['blob', 'blob'],
];

/** Every row at `count`, each measurement in a process of its own, and the
 * record sizes core used is counted with. */
export async function run({ count = 100_000, datamodel = 'native', tracing = true, chart, data = { text: 'hello' }, quickjsMemoryLimit } = {}) {
  const unit = await units({ tracing, datamodel });
  const results = { units: unit };
  for (const what of ['actor', 'hibernated', 'message', 'delayed send']) {
    const job = JSON.stringify({
      what, count, datamodel, tracing, chart: what === 'actor' || what === 'hibernated' ? chart : undefined, data, quickjsMemoryLimit, unit,
    });
    const child = spawnSync(process.execPath, [fileURLToPath(import.meta.url), '--job', job], { encoding: 'utf8' });
    if (child.status === 0) {
      Object.assign(results, { [what]: JSON.parse(child.stdout) });
    } else {
      // a measurement that failed is a row too: what stopped it (quickjs's memoryLimit, the machine's memory)
      const reason = child.stderr.match(/^\w*Error: .*$/m)?.[0] ?? `exit ${child.status ?? child.signal}`;
      const failed = { failed: reason };
      Object.assign(results, { [what]: { started: failed, handled: failed, hibernated: failed, waiting: failed, pending: failed } });
    }
  }
  return results;
}

const runtime = () => (globalThis.Bun ? `bun ${globalThis.Bun.version}` : `node ${process.versions.node}`);

export function report(results, { count, datamodel, tracing }) {
  const lines = [`${runtime()}, datamodel ${datamodel}, tracing ${tracing ? 'on' : 'off'}, ${count} of each`, ''];
  lines.push(''.padEnd(28) + COLUMNS.map(([title]) => title.padStart(title.length + 2)).join(''));
  for (const [label, what, key] of ROWS) {
    const row = results[what][key];
    if (row.failed) {
      lines.push(`${label.padEnd(28)}  failed: ${row.failed}`);
      continue;
    }
    lines.push(label.padEnd(28) + COLUMNS.map(([title, name]) => (row[name] === undefined ? '' : `${row[name]} B`).padStart(title.length + 2)).join(''));
  }
  const { actor, event, timer } = results.units;
  lines.push('', `core's records: actor ${actor} B, event ${event} B, timer ${timer} B`);
  return lines;
}

/** The machine, for the CSV and the report: results are this machine's. */
export function machine() {
  const cpus = os.cpus();
  return `${cpus[0]?.model.trim() ?? 'unknown'}, ${cpus.length} CPUs, ${Math.round(os.totalmem() / 2 ** 30)} GiB, ${os.platform()} ${os.arch()}`;
}

const CSV_HEADER = 'runtime,datamodel,tracing,count,row,core_reserved,core_used,host_other,quickjs,accounted,process,blob,failed,machine,date\n';

export function csv(results, { count, datamodel, tracing }) {
  const date = new Date().toISOString().slice(0, 10);
  return ROWS.map(([label, what, key]) => {
    const row = results[what][key];
    return [runtime(), datamodel, tracing ? 'on' : 'off', count, `"${label}"`, ...COLUMNS.map(([, name]) => row[name] ?? ''),
            row.failed ? `"${row.failed.replaceAll('"', "'")}"` : '', `"${machine()}"`, date].join(',');
  }).join('\n') + '\n';
}

const main = import.meta.main ?? process.argv[1] === fileURLToPath(import.meta.url);
if (main) {
  const { values } = parseArgs({
    options: {
      job: { type: 'string' }, counts: { type: 'string', default: '100000' }, datamodel: { type: 'string', default: 'native' },
      'no-tracing': { type: 'boolean', default: false }, chart: { type: 'string' }, data: { type: 'string' },
      csv: { type: 'string' }, 'quickjs-memory-limit': { type: 'string' },
    },
  });
  if (values.job) {
    process.stdout.write(JSON.stringify(await measure(JSON.parse(values.job))));
  } else {
    const options = {
      datamodel: values.datamodel, tracing: !values['no-tracing'],
      chart: values.chart ? readFileSync(values.chart, 'utf8') : undefined,
      data: values.data ? JSON.parse(values.data) : undefined,
      quickjsMemoryLimit: values['quickjs-memory-limit'] ? Number(values['quickjs-memory-limit']) : undefined,
    };
    if (values.csv && !existsSync(values.csv)) writeFileSync(values.csv, CSV_HEADER);
    console.log(machine());
    for (const count of values.counts.split(',').map(Number)) {
      const results = await run({ ...options, count });
      console.log();
      for (const line of report(results, { ...options, count })) console.log(line);
      if (values.csv) appendFileSync(values.csv, csv(results, { ...options, count }));
    }
  }
}
